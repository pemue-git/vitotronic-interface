//- -------------------------------------------------------------------------------------------------
/* ESP8266 sketch for the Viessmann Optolink WLAN interface.
 * Sketch establshes an option for connection to a WLAN router.
 * If connected, the data from an Optolink to the router and heating can be controlled via fhem.
 *
 * Created by renemt <renemt@forum.fhem.de>
 *
 * REVISION HISTORY
 * Hardware (optolink adapter)
 * Ver. 1.x:
 * - GPIO0 for 1-wire
 * - GPIO2 for debug messages
 * - GPIO12 for config
 * - 512k RAM/64k FS
 * Ver. 2.x:
 * - GPIO0 for 1-wire
 * - GPIO2 for config (or for debug messages alternatively)
 * - 512k RAM/64k FS (in case of blue ESP8266 ESP01 modules)
 * - 1M RAM/64k FS (in case of black ESP8266 ESP01 modules)
 * 
 * Software
 * version 1.0 - renemt see https://github.com/rene-mt/vitotronic-interface-8266/blob/master/vitotronic-interface-8266.ino
 * version 1.1 - Peter Mühlbeyer (PeMue@forum.fhem.de)
 * - added some comments for better understanding
 * - debug output disabled for default (can be switched on again)
 * version 1.2 - Peter Mühlbeyer
 * - added parameter timeout to ensure a proper connection to the router after reboot (e.g. in case of power loss)
 * - changed default port from 8888 to 81 (like in LaCrosse gateway)
 * version 1.3 - Peter Mühlbeyer
 * - added OTA flash and print firmware version on setup and success page
 *
 * version 2.0 - Peter Mühlbeyer
 * - added 1-wire temperature measurement (only for hardware >=v2.2)
 * - check if 1-wire sensors are available and send the values via UDP
 * - added the time for 1-wire measurement to the setup page
 * version 2.1 - Peter Mühlbeyer
 * - reset cycle counter every 24 h, unsigned long instead of long
 * - apply patch for static configuration from: https://forum.fhem.de/index.php/topic,51932.msg451195.html#msg451195
 * - removed WiFi.mode(WIFI_AP) for setup with access point (to be compatible to ESP8266 libraries >2.5.0, does not work
 * - add date of compilation acc. https://forum.arduino.cc/index.php?topic=189325.0, 
 *   https://www.cprogramming.com/reference/preprocessor/__DATE__.html, 
 *   https://www.cprogramming.com/reference/preprocessor/__TIME__.html,
 *   https://gcc.gnu.org/onlinedocs/cpp/Standard-Predefined-Macros.html#Standard-Predefined-Macros,
 *   https://forum.arduino.cc/index.php?topic=158014.0, https://gcc.gnu.org/onlinedocs/gcc-5.1.0/cpp.pdf
 *   directives: __DATE__, __TIME__,  __VERSION__ (not reliable)
 *   Arduino IDE version: ARDUINO (decimal)
 *   from LaCrosse Gateway: ESP.getCoreVersion(); ESP.getSdkVersion();
 * - add solution for ESP8266 library > v2.5.0 acc. https://forum.fhem.de/index.php/topic,51583.msg1170834.html#msg1170834 and
 *   https://stackoverflow.com/questions/58113937/esp8266-arduino-why-is-it-necessary-to-add-the-icache-ram-attr-macro-to-isrs-an or
 *   (solution and explanation see bottom) or
 *   https://dillinger-engineering.de/tag/icache_ram_attr/
 * version 2.1 - Peter Mühlbeyer
 * - change to LittleFS (SPIFFS has been deprecated)
 */
//- -------------------------------------------------------------------------------------------------

// import required libraries, ESP8266 libraries >2.1.0 are required
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
// for OTA update
#include <ArduinoOTA.h>
// for 1-wire
#include <OneWire.h>
#include <DallasTemperature.h>
#define ONE_WIRE_PIN 0                           // works only for hardware v2.2 and bigger
#define T_PRECISION 12                           // always highest precision
#define REQUIRESALARMS false                     // no 1-wire alarms for this firmware necessary, redefinition from library
#define MAX_SENSORS 5                            // max. amount of sensors

// define sketch version, variable for compiler information
#define FIRMWARE_VER "2.2"
String compiler;

// GPIO pin triggering setup interrupt for re-configuring the server
//#define SETUP_INTERRUPT_PIN 12                 // for optolink adapter v1.x
#define SETUP_INTERRUPT_PIN 2                    // for optolink adapter v2.x
void ICACHE_RAM_ATTR setupInterrupt();           // to use ESP8266 library >v2.5.0

// SSID and password for the configuration WiFi network established bei the ESP in setup mode
#define SETUP_AP_SSID "vitotronic-interface"
#define SETUP_AP_PASSWORD "vitotronic"

// define, if debug output via serial interface will be enabled (true) or not (false)
#define DEBUG_SERIAL1 false

// setup mode flag, 1 when in setup mode, 0 otherwise
uint8_t _setupMode = 0;

// constant for time between measurements in ms (not necessary, integrated in setup (interval))
//const int UPDATE_TIME = 20000;
unsigned int interval_1wire = 0;

// setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_PIN);
// pass the oneWire reference to Dallas Temperature
DallasTemperature sensors(&oneWire);
// how many devices are connected?
int devicesFound = 0;
// arrays to store device addresses
DeviceAddress devices[MAX_SENSORS];
// array for raw temperature
float tempC[MAX_SENSORS];

// for UDP cnonnection, multicast declaration
WiFiUDP Udp;
IPAddress ipMulti(239, 0, 0, 57);                // multicast address
unsigned int portMulti = 12345;                  // local UDP port

/*
  Config file layout - one entry per line, eight lines overall, terminated by '\n':
    <ssid>
    <password>
    <port>
    <static IP - or empty line>
    <dnsServer IP - or empty line>
    <gateway IP - or empty line>
    <subnet mask - or empty line>
    <timeout>
    <1-wire interval>
*/

// path+filename of the WiFi configuration file in the ESP's internal file system
const char* _configFile = "/config/config.txt";

// some basic WLAN definitions
#define FIELD_SSID "ssid"
#define FIELD_PASSWORD "password"
#define FIELD_PORT "port"
#define FIELD_IP "ip"
#define FIELD_DNS "dns"
#define FIELD_GATEWAY "gateway"
#define FIELD_SUBNET "subnet"
#define FIELD_TIMEOUT "timeout"
#define FIELD_1WINTERVAL "interval"

const char* _htmlConfigTemplate =
  "<html>" \
    "<head>" \
      "<title>Vitotronic WiFi Interface</title>" \
    "</head>" \
    "<body>" \
      "<h1>Vitotronic WiFi Interface v%%ver</h1>" \
      "<h2>Setup</h2>" \
      "<form action=\"update\" id=\"update\" method=\"post\">" \
        "<p>Fields marked by (*) are mandatory.</p>" \
        "<h3>WiFi Network Configuration Data</h3>" \
        "<p>" \
          "<div>The following information is required to set up the WiFi connection of the server.</div>" \
          "<label for=\"" FIELD_SSID "\">SSID (*):</label><input type=\"text\" name=\"" FIELD_SSID "\" required  />" \
          "<label for=\"" FIELD_PASSWORD "\">Password: (*)</label><input type=\"password\" name=\"" FIELD_PASSWORD "\" />" \
        "</p>" \
        "<h3>Static IP settings</h3>" \
        "<p>" \
          "<div>If you want to assing a static IP address fill out the following information. All addresses have to by given in IPv4 format (xxx.xxx.xxx.xxx). <br/>" \
          "Leave the fields empty to rely on DHCP to obtain an IP address.</div>" \
          "<label for=\"" FIELD_IP "\">Static IP:</label><input type=\"text\" name=\"" FIELD_IP "\" pattern=\"^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$\" /><br/>" \
          "<label for=\"" FIELD_DNS "\">DNS Server:</label><input type=\"text\" name=\"" FIELD_DNS "\" pattern=\"^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$\" /><br/>" \
          "<label for=\"" FIELD_GATEWAY "\">Gateway:</label><input type=\"text\" name=\"" FIELD_GATEWAY "\" pattern=\"^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$\" /><br/>" \
          "<label for=\"" FIELD_SUBNET "\">Subnet mask:</label><input type=\"text\" name=\"" FIELD_SUBNET "\" pattern=\"^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$\" /><br/>"
        "</p>" \
        "<h3>Server Port</h3>" \
        "<p>" \
          "<div>The Vitotronic WiFi Interface will listen at the following port for incoming telnet connections:</div>" \
          "<label for=\"" FIELD_PORT "\">Port (*):</label><input type=\"number\" name=\"" FIELD_PORT "\" value=\"81\" required />" \
        "</p>" \
        "<p>" \
          "<div>The 1-wire UDP port is fixed to: 12345</div>" \
        "</p>" \
        "<h3>Timeout</h3>" \
        "<p>" \
          "<div>The Vitotronic WiFi Interface will try to connect (timeout) s after the reboot of the router:</div>" \
          "<label for=\"" FIELD_TIMEOUT "\">Timeout (*):</label><input type=\"number\" name=\"" FIELD_TIMEOUT "\" value=\"60\" required />" \
        "</p>" \
        "<h3>1-wire interval</h3>" \
        "<p>" \
          "<div>In case of 1-wire temperature sensors are connected, send the temperatures every (interval) s (between 20 and 3600 s):</div>" \
          "<label for=\"" FIELD_1WINTERVAL "\">Interval (*):</label><input type=\"number\" name=\"" FIELD_1WINTERVAL "\" value=\"360\" required />" \
        "</p>" \
        "<p>" \
          "<div>Compiled with %%compiler.</div>"\
        "<p>" \
        "<div>" \
          "<button type=\"reset\">Reset</button>&nbsp;" \
          "<button type=\"submit\">Submit</button>" \
        "</div>" \
      "</form>" \
    "</body>" \
  "</html>";

const char* _htmlSuccessTemplate =
  "<html>" \
    "<head>" \
      "<title>Vitotronic WiFi Interface</title>" \
    "</head>" \
    "<body>" \
      "<h1>Vitotronic WiFi Interface v%%ver</h1>" \
      "<h2>Configuration saved</h2>" \
      "<p>" \
        "The configuration has been successfully saved to the adapter:<br/><br/>" \
        "- SSID: %%ssid<br/>" \
        "- Password: %%password<br/>" \
        "- Port: %%port<br/><br/>" \
        "- Static IP: %%ip<br/>" \
        "- DNS server: %%dns<br/>" \
        "- Gateway: %%gateway<br/>" \
        "- Subnet mask: %%subnet<br/><br/>" \
        "- Timeout: %%timeout<br/><br/>" \
        "- 1-wire interval: %%interval<br/><br/>" \
        "The adapter will reboot now and connect to the specified WiFi network. In case of a successful connection the <em>vitotronic-interface</em> network will be gone. <br/>" \
        "<strong>If no connection is possible, e.g. because the password is wrong or the network is not available, the adapter will return to setup mode again.</strong>" \
      "</p>" \
      "<p>" \
        "<strong><em>IMPORTANT NOTICE:</em></strong><br/>" \
        "Some of the ESP8266 microcontrollers, built into the adapter, need a hard reset to be able to connect to the new WiFi network. " \
        "Therefore it is recommended to interrupt the power supply for a short time within the next 10 seconds.</strong>" \
      "</p>" \
    "</body>" \
  "</html>";

ESP8266WebServer* _setupServer = NULL;
WiFiServer* server = NULL;
WiFiClient serverClient;

//- -------------------------------------------------------------------------------------------------
// helper function to remove trailing CR (0x0d) from strings read from config file
//- -------------------------------------------------------------------------------------------------
String removeTrailingCR(String input)
{
  if (!input)
    return String();
  if (input.charAt(input.length()-1) == 0x0d)
  {
    input.remove(input.length()-1);
  }
  return input;
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// function to print a device address
//- -------------------------------------------------------------------------------------------------
void printAddress(DeviceAddress deviceAddress)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16) Serial1.print("0");
    Serial1.print(deviceAddress[i], HEX);
  }
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// function to print a device address
//- -------------------------------------------------------------------------------------------------
String printAddressStr(DeviceAddress deviceAddress)
{
  String outstr = "";
  for (uint8_t i = 0; i < 8; i++)
  {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16) outstr += "0";
    outstr += String(deviceAddress[i], HEX);
  }
  outstr.toUpperCase();
  return outstr;
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// function to print the temperature for a device
//- -------------------------------------------------------------------------------------------------
String printTemperatureStr(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  if (tempC < 10)
    return "0" + (String)tempC;
  else
    return (String)tempC;
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// setup of the program
//- -------------------------------------------------------------------------------------------------
void setup()
{
  // get compile date, time, Arduino IDE version and ESP8266 core version
  // Arduino IDE: ARDUINO as DEC variable
  //const char ver_date[100] = FIRMWARE_VER " compiled with gcc v" __VERSION__ " on " __DATE__ ", " __TIME__;
  //String compiler = String("Arduino IDE v" + ARDUINO + "on " + __DATE__ + ", " + __TIME__ + ESP.getCoreVersion());
  compiler = String(ARDUINO, DEC);     // gets Arduino version, eg. 10808 or 10811
  compiler.replace ("0", ".");         // replace 0 by . in Arduino IDE version
  //compiler = compiler_temp.substring(1, 1) + "." + compiler_temp.substring(2, 2) + "." + compiler_temp.substring(3, compiler_temp.length());
  // collect compiling date, time and ESP8266 core version
  compiler = "Arduino IDE v" + compiler + " on " + __DATE__ + ", " + __TIME__ + ", ESP8266 core v" + ESP.getCoreVersion() + ", SDK v" + ESP.getSdkVersion();
  compiler.replace ("_", ".");         // replace _ by . in ESP8266 core version info

  Serial1.begin(115200); // serial1 (GPIO2) as debug output (TX), with 115200,N,1
  // uncomment next line to enable debugging, not for productive use
  //Serial1.setDebugOutput(true);
  Serial1.setDebugOutput(DEBUG_SERIAL1);
  Serial1.printf("\nVitotronic WiFi Interface v'%s'\n\n", FIRMWARE_VER);
  Serial1.printf("\nCompiled with "); Serial1.print(compiler); Serial1.print("\n\n");
  yield();

  // try to read config file from internal file system
  LittleFS.begin();
  File configFile = LittleFS.open(_configFile, "r");
  if (configFile)
  {
    Serial1.println("Using existing WiFi config to connect");

    String ssid = removeTrailingCR(configFile.readStringUntil('\n'));
    String password = removeTrailingCR(configFile.readStringUntil('\n'));

    String port = removeTrailingCR(configFile.readStringUntil('\n'));

    if (!ssid || ssid.length() == 0
        || !port || port.length() == 0)
    {
      // reset & return to setup mode if minium configuration data is missing
      Serial1.println("Minimum configuration data is missing (ssid, port) - resetting to setup mode");
      LittleFS.remove(_configFile);

      yield();
      ESP.reset();
      return;
    }

    String ip = removeTrailingCR(configFile.readStringUntil('\n'));
    String dns = removeTrailingCR(configFile.readStringUntil('\n'));
    String gateway = removeTrailingCR(configFile.readStringUntil('\n'));
    String subnet = removeTrailingCR(configFile.readStringUntil('\n'));
    String timeout = removeTrailingCR(configFile.readStringUntil('\n'));
    String interval = removeTrailingCR(configFile.readStringUntil('\n'));
    interval_1wire = interval.toInt()*1000; // convert to ms

    configFile.close();

    // initialize WiFi
    WiFi.disconnect();
    yield();
    WiFi.mode(WIFI_STA);
    yield();
    
    if (ip && ip.length() > 0
        && dns && dns.length() > 0
        && gateway && gateway.length() > 0
        && subnet && subnet.length() > 0)
    {
      //set static IP configuration, if available
      Serial1.println("Static IP configuration available:");
      Serial1.printf("IP: '%s', DNS: '%s', gateway: '%s', subnet mask: '%s'\n", ip.c_str(), dns.c_str(), gateway.c_str(), subnet.c_str());
      // change from: https://forum.fhem.de/index.php/topic,51932.msg451195.html#msg451195
      //WiFi.config(IPAddress().fromString(ip), IPAddress().fromString(dns), IPAddress().fromString(gateway), IPAddress().fromString(subnet));
      IPAddress ipIP;
      ipIP.fromString(ip);
      IPAddress dnsIP;
      dnsIP.fromString(dns);
      IPAddress gatewayIP;
      gatewayIP.fromString(gateway);
      IPAddress subnetIP;
      subnetIP.fromString(subnet);
      WiFi.config(ipIP, dnsIP, gatewayIP, subnetIP);
      yield();
    }

    Serial1.printf("\nConnecting to WiFi network '%s' password: '", ssid.c_str());
    for (unsigned int i=0; i<password.length(); i++)
    {
      Serial1.print("*");
    }
    Serial1.print("'");

    Serial1.printf("\nUsing timeout of '%s' s for reboot of router in case of power loss.'", timeout.c_str());
    Serial1.printf("\nUsing 1-wire interval of '%s' s.'", interval.c_str());

    uint8_t wifiAttempts = 0;
    WiFi.begin(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED && wifiAttempts++ < 40)
    {
      Serial1.print(".");
      delay(1000);
      if (wifiAttempts == 21) // half the number used for connecting, now assuming that router is rebooting and needs timeout s
      {
        Serial1.print("... waiting ...");
        delay(timeout.toInt()*1000);
      }
    }
    if (wifiAttempts == 41)
    {
      Serial1.printf("\n\nCould not connect to WiFi network '%s'.\n", ssid.c_str());
      Serial1.println("Deleting configuration and resetting ESP to return to configuration mode");
      LittleFS.remove(_configFile);
      yield();
      ESP.reset();
    }

    Serial1.printf("\n\nReady! Server available at %s:%s\n", WiFi.localIP().toString().c_str(), port.c_str());
    
    // initialize OTA
    ArduinoOTA.setPort(8266);
    ArduinoOTA.begin();

    // start up 1-wire bus
    sensors.begin();
    //sensors.setResolution(T_PRECISION); -> later
    // locate devices on the bus
    Serial1.println("Locating devices ...");
    Serial1.print("Found ");
    Serial1.print(sensors.getDeviceCount(), DEC);
    Serial1.println(" device(s).");
    devicesFound = sensors.getDeviceCount();  
    // report parasite power requirements
    Serial1.print("Parasite power is: "); 
    if (sensors.isParasitePowerMode()) Serial1.println("ON");
      else Serial1.println("OFF");
    for (int i = 0; i < devicesFound; i++)
      if (!sensors.getAddress(devices[i], i)) 
        Serial1.println("Unable to find address for Device " + i); 
    // show the addresses we found on the bus
    for (int i = 0; i < devicesFound; i++)
    {    
      Serial1.print("Device " + (String)i + " Address: ");
      printAddress(devices[i]);
      Serial1.println();
    }
    // set precision for all found sensors
    for (int i = 0; i < devicesFound; i++)
      sensors.setResolution(devices[i], T_PRECISION);
    // set wait routine for async
    sensors.setWaitForConversion(false);
   
    // only if 1-wire devices found start UDP server
    if (devicesFound > 0)
    {
      Udp.begin(portMulti);
      Serial1.printf("UDP port at IP %s, UDP port %d opened\n", WiFi.localIP().toString().c_str(), portMulti);
    }

    Serial.begin(4800, SERIAL_8E2); // Vitotronic connection runs at 4800,E,2
    Serial1.println("Serial port to Vitotronic opened at 4800 bps, 8E2");

    server = new WiFiServer(port.toInt());
    server->begin();
  }
  else
  {
    //start ESP in access point mode and provide a HTTP server at port 80 to handle the configuration page.
    Serial1.println("No WiFi config exists, switching to setup mode");
    //WiFi.mode(WIFI_AP); // might not be needed acc. http://onlineshouter.com/use-esp8266-wifi-modes-station-access-point/
    WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);

    _setupServer = new ESP8266WebServer(80);
    _setupServer->on("/", handleRoot);
    _setupServer->on("/update", handleUpdate);
    _setupServer->onNotFound(handleRoot);
    _setupServer->begin();

    Serial1.println("WiFi access point with SSID \"" SETUP_AP_SSID "\" opened");
    Serial1.printf("Configuration web server started at %s:80\n\n", WiFi.softAPIP().toString().c_str());

    _setupMode = 1;
  }

  //configure GPIO for setup interrupt by push button
  pinMode(SETUP_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SETUP_INTERRUPT_PIN), setupInterrupt, FALLING);
  Serial1.printf("Interrupt for re-entering setup mode attached to GPIO%d\n\n", SETUP_INTERRUPT_PIN); 
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// wifiSerialLoop, serial loop (serial <-> WLAN)
//- -------------------------------------------------------------------------------------------------
void wifiSerialLoop()
{
  //uint8_t i;

  // check if there is a new client trying to connect
  if (server && server->hasClient())
  {
    // check, if no client is already connected
    if (!serverClient || !serverClient.connected())
    {
      if (serverClient) serverClient.stop();
      serverClient = server->available();
      Serial1.println("New client connected\n");
    }
    else
    {
      // reject additional connection requests.
      WiFiClient serverClient = server->available();
      serverClient.stop();
    }
  }
  yield();

  // check client for data
  if (serverClient && serverClient.connected())
  {
    size_t len = serverClient.available();

    if (len)
    {
      uint8_t sbuf[len];
      
      serverClient.read(sbuf, len);
      // write received WiFi data to serial
      Serial.write(sbuf, len);

      yield();

      // debug output received WiFi data to Serial1
      //Serial1.println();
      Serial1.print("WiFi: ");
      for (uint8_t n = 0; n < len; n++)
      {
        Serial1.print(sbuf[n], HEX);
      }
      Serial1.println();
    }
  }
  yield();

  // check UART for data
  if (Serial.available())
  {
    size_t len = Serial.available();
    uint8_t sbuf[len];

    Serial.readBytes(sbuf, len);
    // push UART data to connected WiFi client
    if (serverClient && serverClient.connected())
    {
      serverClient.write(sbuf, len);
    }
    yield();

    // debug output received Serial data to Serial1
    //Serial1.println();
    Serial1.print("Serial: ");
    for (uint8_t n = 0; n < len; n++)
    {
      Serial1.printf("%02x ", sbuf[n]);
    }
    Serial1.println();
  }
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// OneWireLoop
//- -------------------------------------------------------------------------------------------------
void OneWireLoop(void)
{
  // cycle counter
  static unsigned long cycle_count = 0; 
  // start time of the last action
  static unsigned long startTime = 0; 
  // save the time (in ms) since start of processor
  //static unsigned long currentStateMillis = 0;
  // variable for waiting time (depending on precision)
  static unsigned int wait_conv_ms = 750 / (1 << (12 - T_PRECISION));
  // variable for sending data via UDP
  static String sendstr="";
  // variable for time needed for complete 1-wire conversion
  static long time_between_measurements;

  // variables for state machine
  static enum {START, WAIT_CONV, READ, COLLECT, SEND, WAIT_NEXT} state=START;
  
  // loop variable
  static int j=0;
  
  // time in s
  float tmess_s;

  // state machine, depending on stati
  switch (state)
  {
    // START: actual time stored, count incremented, start measurement
    case START:
      // overwrite time stamp with actual time
      startTime = millis();
      // avoid overflow -> removed
      //if (cycle_count >= 4294967295) cycle_count = 0;
      // reset cycle counter every 24 h
      if (cycle_count*interval_1wire/1000 >= 86400) cycle_count = 0;
      // increment cycle_count
      cycle_count++;
      sensors.requestTemperatures();
      state=WAIT_CONV;
      yield();
    break;

    case WAIT_CONV:
      if ((millis()-startTime)>wait_conv_ms) // if wait_conv_ms has been reached, switch to COLLECT (collect data)
      {
        //currentStateMillis=millis();
        state=READ;
      }
    break;

    case READ:
      if (j < devicesFound) // if still devices available to read temperature
      {
        //currentStateMillis=millis();
        // getTempC requires approx. 13 ms
        // getTemp (raw) is same as getTempC
        // getTempCByIndex requires approx. 22 ms -> too slow
        tempC[j]=sensors.getTempC(devices[j]);
        //Serial1.print("After read temperature (READ) loop "); Serial.print(j, DEC); 
        //Serial1.print(" "); Serial.print(millis()-startTime, DEC); Serial.print(" ms, ");
        //Serial1.print(millis()-currentStateMillis, DEC); Serial.println(" ms.");
        j++;
        //yield();
      }
      else state=COLLECT;
    break;

    case COLLECT:
      j = 0; // set loop variable to 0 again
      //currentStateMillis=millis();
      sendstr = "OK VALUES THz 1 cnt=";
      sendstr += String(cycle_count, DEC);
      sendstr += ",";

      // print the device information
      for (int i = 0; i < devicesFound; i++)
      {
        sendstr += printAddressStr(devices[i]);
        sendstr += "=";
        sendstr += String(tempC[i]);
        if (i != devicesFound - 1) sendstr += ",";
      }
      //Serial1.print("After string collection (COLLECT) "); Serial.print(millis()-startTime, DEC); Serial.print(" ms, ");
      //Serial1.print(millis()-currentStateMillis, DEC); Serial.println(" ms.");
      // get the time for conversion
      time_between_measurements = millis() - startTime;
      sendstr += ",t_mess=";
      //sendstr += String(time_between_measurements, DEC);
      tmess_s=(float) time_between_measurements/1000;
      sendstr += String(tmess_s, 3);
      //yield();
      state=SEND;
    break;

    case SEND:
      //currentStateMillis=millis();
      // send string to UPD port
      if (Udp.beginPacketMulticast(ipMulti, portMulti, WiFi.localIP()) == 1) 
      {
        //Serial1.println("after Udp.beginPacketMulticast ...");
	    Udp.write(sendstr.c_str());
        //Serial1.println("after Udp.write ...");
        Udp.endPacket();
        //Serial1.println("after Udp.endPacket ...");
	  }
      //yield();
      // get the time for sending, only for debug
      //time_between_measurements = millis() - startTime;
      //sendstr += ",t_tot=";
      //sendstr += String(time_between_measurements, DEC);
      //Serial.print("After sending "); Serial.print(millis()-startTime, DEC); Serial.print(" ms, ");
      //Serial.print(millis()-currentStateMillis, DEC); Serial.println(" ms.");
      //Serial.println(sendstr);
      state=WAIT_NEXT;
    break;
    
    case WAIT_NEXT:
      if ((millis()-startTime)>=interval_1wire) // if UPDATE_TIME/interval_1wire has been reached, switch to START again
      {
        //currentStateMillis=millis();
        state=START;
        //Serial.print("After restart again "); Serial.print(millis()-startTime, DEC); Serial.print(" ms, ");
        //Serial.print(millis()-currentStateMillis, DEC); Serial.println(" ms.");
      }
    break;
  }  
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// handleRoot: goes to setup page
//- -------------------------------------------------------------------------------------------------
void handleRoot()
{
  if (_setupServer)
  {
    String htmlConfig(_htmlConfigTemplate);
    htmlConfig.replace("%%ver", FIRMWARE_VER);
    htmlConfig.replace("%%compiler", compiler);
    //htmlConfig.replace("%%ver", firmware_ver_date);

    //_setupServer->send(200, "text/html", _htmlConfigTemplate);
    _setupServer->send(200, "text/html", htmlConfig);
  }
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// handleUpdate: goto update page
//- -------------------------------------------------------------------------------------------------
void handleUpdate()
{
  Serial1.println("handleUpdate()");
  if (!_setupServer)
  {
    Serial1.println("_setupServer is NULL, exiting");
    return;
  }
  if (_setupServer->uri() != "/update")
  {
    Serial1.printf("URI is '%s', which does not match expected URI '/update', exiting\n", _setupServer->uri().c_str());
    return;
  }

  String ssid = _setupServer->arg(FIELD_SSID);
  String password = _setupServer->arg(FIELD_PASSWORD);
  String port = _setupServer->arg(FIELD_PORT);
  String ip = _setupServer->arg(FIELD_IP);
  String dns = _setupServer->arg(FIELD_DNS);
  String gateway = _setupServer->arg(FIELD_GATEWAY);
  String subnet = _setupServer->arg(FIELD_SUBNET);
  String timeout = _setupServer->arg(FIELD_TIMEOUT);
  String interval = _setupServer->arg(FIELD_1WINTERVAL);

  Serial1.println("Submitted parameters:");
  Serial1.printf("SSID: '%s'\n", ssid.c_str());
  Serial1.print("Password: '");
  for (unsigned int i=0; i < password.length(); i++)
  {
    Serial1.print('*');
  }
  Serial1.println("'");
  Serial1.printf("Port: '%s'\n", port.c_str());
  Serial1.printf("IP: '%s'\n", ip.c_str());
  Serial1.printf("DNS: '%s'\n", dns.c_str());
  Serial1.printf("Gateway: '%s'\n", gateway.c_str());
  Serial1.printf("Subnet: '%s'\n", subnet.c_str());
  Serial1.printf("Timeout: '%s'\n", timeout.c_str());
  Serial1.printf("1-wire interval: '%s'\n", interval.c_str());

  // check if mandatory parameters have been set
  if (ssid.length() == 0 || port.length() == 0)
  {
    Serial1.println("Missing SSID or port parameter!");
    handleRoot();
    return;
  }

  // for static IP configuration: check if all parameters have been set
  //if (ip.length() > 0 || dns.length() > 0 || gateway.length() > 0 || subnet.length() > 0 &&
  //    (ip.length() == 0 || dns.length() == 0 || gateway.length() == 0 || subnet.length() == 0))
  if ((ip.length() > 0 || dns.length() > 0 || gateway.length() > 0 || subnet.length() > 0) &&
      (ip.length() == 0 || dns.length() == 0 || gateway.length() == 0 || subnet.length() == 0))
  {
    Serial1.println("Missing static IP parameters!");
    handleRoot();
    return;
  }

  // check, if values are out of range, if not, correct them
  if (timeout.toInt() < 0) timeout = "0";
  if (timeout.toInt() > 300) timeout = "300";
  if (interval.toInt() < 5) interval = "20";  // allow also 5 s time between 1-wire measurement for testing
  if (interval.toInt() > 3600) interval = "3600";
  
  // write configuration data to file
  Serial1.printf("Writing config to file '%s'", _configFile);
  LittleFS.begin();
  File configFile = LittleFS.open(_configFile, "w");

  configFile.println(ssid);
  configFile.println(password);
  configFile.println(port);
  configFile.println(ip);
  configFile.println(dns);
  configFile.println(gateway);
  configFile.println(subnet);
  configFile.println(timeout);
  configFile.println(interval);

  configFile.close();
  yield();
  
  String maskedPassword = "";
  for (unsigned int i=0; i<password.length(); i++)
  {
    maskedPassword+="*";
  }

  String htmlSuccess(_htmlSuccessTemplate);
  htmlSuccess.replace("%%ver", FIRMWARE_VER);
  //htmlSuccess.replace("%%ver", firmware_ver_date);
  htmlSuccess.replace("%%ssid", ssid);
  htmlSuccess.replace("%%password", maskedPassword);
  htmlSuccess.replace("%%port", port);
  htmlSuccess.replace("%%ip", ip);
  htmlSuccess.replace("%%dns", dns);
  htmlSuccess.replace("%%gateway", gateway);
  htmlSuccess.replace("%%subnet", subnet);
  htmlSuccess.replace("%%timeout", timeout);
  htmlSuccess.replace("%%interval", interval);
  
  //leave setup mode and restart with existing configuration
  _setupServer->send(200, "text/html", htmlSuccess);
  
  Serial1.println("Config saved, resetting ESP8266");
  delay(10);
  ESP.reset();
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// setupInterrupt: goes to config in case button has been pushed
//- -------------------------------------------------------------------------------------------------
void setupInterrupt()
{
  // if the setup button has been pushed delete the existing configuration and reset the ESP to enter setup mode again
  Serial1.println("Reset button pressed, deleting existing configuration");
  LittleFS.begin();
  LittleFS.remove(_configFile);
  yield();
  ESP.reset();
}
//- -------------------------------------------------------------------------------------------------

//- -------------------------------------------------------------------------------------------------
// main loop, runs until infinity
//- -------------------------------------------------------------------------------------------------
void loop()
{
  if (!_setupMode)
  {
    // establish main serial server
    wifiSerialLoop();
    // handle OTA flash, in case WLAN connection is available
    ArduinoOTA.handle();
    // perform 1-wire measurement loop
    if (devicesFound == 0)
    {
       Serial1.println("No devices found.");
    }
    else OneWireLoop();
  }
  else if (_setupServer)
    _setupServer->handleClient();
}
//- -------------------------------------------------------------------------------------------------
