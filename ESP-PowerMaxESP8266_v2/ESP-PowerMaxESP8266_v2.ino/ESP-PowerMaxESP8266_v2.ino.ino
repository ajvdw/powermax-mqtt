#include <pmax.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <ESP8266SSDP.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <PubSubClient.h>


/* MQTT Status code translation

-4 : MQTT_CONNECTION_TIMEOUT - the server didn't respond within the keepalive time
-3 : MQTT_CONNECTION_LOST - the network connection was broken
-2 : MQTT_CONNECT_FAILED - the network connection failed
-1 : MQTT_DISCONNECTED - the client is disconnected cleanly
0 : MQTT_CONNECTED - the client is connected
1 : MQTT_CONNECT_BAD_PROTOCOL - the server doesn't support the requested version of MQTT
2 : MQTT_CONNECT_BAD_CLIENT_ID - the server rejected the client identifier
3 : MQTT_CONNECT_UNAVAILABLE - the server was unable to accept the connection
4 : MQTT_CONNECT_BAD_CREDENTIALS - the username/password were rejected
5 : MQTT_CONNECT_UNAUTHORIZED - the client was not authorized to connect


Set up using web browser: http://192.168.1.xxx/config?ip_for_mqtt=192.168.1.yyy&port_for_mqtt=1883&mqtt_user=homeassistant&mqtt_pass=hassapipassword
*/


//////////////////// IMPORTANT DEFINES, ADJUST TO YOUR NEEDS //////////////////////
//////////////////// COMMENT DEFINE OUT to disable a feature  /////////////////////
//Telnet allows to see debug logs via network, it allso allows CONTROL of the system if PM_ALLOW_CONTROL is defined
#define PM_ENABLE_TELNET_ACCESS

//This enables control over your system, when commented out, alarm is 'read only' (esp will read the state, but will never arm/disarm)
#define PM_ALLOW_CONTROL

//This enables flashing of ESP via OTA (WiFI)
#define PM_ENABLE_OTA_UPDATES

//This enables ESP to send a multicast UDP packet on LAN with notifications
//Any device on your LAN can register to listen for those messages, and will get a status of the alarm
//#define PM_ENABLE_LAN_BROADCAST

//Those settings control where to send lan broadcast, clients need to use the same value. Note IP specified here has nothing to do with ESP IP, it's only a destination for multicast
//Read more about milticast here: http://en.wikipedia.org/wiki/IP_multicast
IPAddress PM_LAN_BROADCAST_IP(192, 168, 32, 12);
#define   PM_LAN_BROADCAST_PORT  23127



//CHANGE THESE SETTINGS
//Specify your WIFI settings:
//#define WIFI_SSID "SSIDHere"
//#define WIFI_PASS "PSKHere"
#define Firmware_Date __DATE__
#define Firmware_Time __TIME__

#define JsonHeaderText "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n"
// MQTT SETUP
#define PM_ENABLE_MQTT_ACCESS

//MQTT Fail retry interval, default set to 5 seconds, if it fails 3 times it will be set to wait 60 seconds and then try three more times continously.
static unsigned long REFRESH_INTERVAL = 5000; // ms
static unsigned long lastRefreshTime = 0;
int mqttFail = 0;

int mqtt_reconnects = 0;
int mqtt_failed = 0;

//MQTT topic for Alarm state information output from Powermax alarm
char* mqttAlarmStateTopic = "powermax/alarm/state";
//MQTT topic for Zone state information output from Powermax alarm
char* mqttZoneStateTopic = "powermax/zone/state";
//MQTT topic for MQTT input to powermax alarm
char* mqttAlarmInputTopic = "powermax/alarm/input";




//Default values for MQTT, these are only used when first booting and stored in EEPROM from first boot onwards, no need to change
char IP_FOR_MQTT[17];
char PORT_FOR_MQTT[7];
char MQTT_USER[30];
char MQTT_PASS[30];
//Inactivity timer wil always default to this value on boot (it is not stored in EEPROM at the moment, though only resets when the Powermax power cycles (hence rarely))
int inactivity_seconds = 20;
//These define the pins used for triggering alarms (or something else!) and the timer to turn them off
int activatedPinA = -1;
int activatedPinB = -1;
unsigned long userPinResetTime = 0;

//Global variables for managing zones
int zones_enrolled_count = MAX_ZONE_COUNT;
int max_zone_id_enrolled = MAX_ZONE_COUNT;

#define ALARM_STATE_CHANGE 0
#define ZONE_STATE_CHANGE 1
//MQTT Target IP
#define EEPROM_IP_ADDR_LOC 100 
//MQTT Target port
#define EEPROM_PORT_ADDR_LOC 120
//MQTT Userid
#define EEPROM_USER_ADDR_LOC 140
//MQTT Password
#define EEPROM_PASS_ADDR_LOC 170




void ReadEEPROMSettings() {
    for(int ix=0; ix < sizeof(IP_FOR_MQTT); ix++) {
       IP_FOR_MQTT[ix] = EEPROM.read(EEPROM_IP_ADDR_LOC + ix);
    }

    for(int ix=0; ix < sizeof(PORT_FOR_MQTT); ix++) {
       PORT_FOR_MQTT[ix] = EEPROM.read(EEPROM_PORT_ADDR_LOC + ix);
    }

    for(int ix=0; ix < sizeof(MQTT_USER); ix++) {
       MQTT_USER[ix] = EEPROM.read(EEPROM_USER_ADDR_LOC + ix);
    }
    
    for(int ix=0; ix < sizeof(MQTT_PASS); ix++) {
       MQTT_PASS[ix] = EEPROM.read(EEPROM_PASS_ADDR_LOC + ix);
    }
}

void WriteEEPROMSettings() {
    for(int ix=0; ix < sizeof(IP_FOR_MQTT); ix++) {
        EEPROM.write(EEPROM_IP_ADDR_LOC + ix, IP_FOR_MQTT[ix]);
    }

    for(int ix=0; ix < sizeof(PORT_FOR_MQTT); ix++) {
        EEPROM.write(EEPROM_PORT_ADDR_LOC + ix, PORT_FOR_MQTT[ix]);
    }

   for(int ix=0; ix < sizeof(MQTT_USER); ix++) {
        EEPROM.write(EEPROM_USER_ADDR_LOC + ix, MQTT_USER[ix]);
    }

    
    for(int ix=0; ix < sizeof(MQTT_PASS); ix++) {
        EEPROM.write(EEPROM_PASS_ADDR_LOC + ix, MQTT_PASS[ix]);
    }
    EEPROM.commit();
}

//////////////////////////////////////////////////////////////////////////////////
//NOTE: PowerMaxAlarm class should contain ONLY functionality of Powerlink
//If you want to (for example) send an SMS on arm/disarm event, don't add it to PowerMaxAlarm
//Instead create a new class that inherits from PowerMaxAlarm, and override required function
class MyPowerMax : public PowerMaxAlarm
{
public:
    bool zone_motion[MAX_ZONE_COUNT+1] = {0};
    virtual void OnStatusChange(const PlinkBuffer  * Buff)
    {
        //call base class implementation first, this will send ACK back and upate internal state.
        PowerMaxAlarm::OnStatusChange(Buff);

        //Now send update to ST and use zone 0 as system state not zone
        unsigned char zoneId = 0;
        SendMQTTMessage(GetStrPmaxLogEvents(Buff->buffer[4]), GetStrPmaxEventSource(Buff->buffer[3]), zoneId, ALARM_STATE_CHANGE);
        //now our customization:
        switch(Buff->buffer[4])
        {
        case 0x51: //"Arm Home" 
        case 0x53: //"Quick Arm Home"
            //OnSytemArmed(Buff->buffer[4], GetStrPmaxLogEvents(Buff->buffer[4]), Buff->buffer[3], GetStrPmaxEventSource(Buff->buffer[3]));
            break;

        case 0x52: //"Arm Away"
        case 0x54: //"Quick Arm Away"
            //OnSytemArmed(Buff->buffer[4], GetStrPmaxLogEvents(Buff->buffer[4]), Buff->buffer[3], GetStrPmaxEventSource(Buff->buffer[3]));
            break;

        case 0x55: //"Disarm"
            //OnSytemDisarmed(Buff->buffer[3], GetStrPmaxEventSource(Buff->buffer[3]));
            break;
        }        
    }
    
    virtual void OnStatusUpdatePanel(const PlinkBuffer  * Buff)
    {
        //call base class implementation first, to log the event and update states.
        PowerMaxAlarm::OnStatusUpdatePanel(Buff);

        //Now if it is a zone event then send it to SmartThings
        if (this->isZoneEvent()) {
              const unsigned char zoneId = Buff->buffer[5];
              ZoneEvent eventType = (ZoneEvent)Buff->buffer[6];
              
              SendMQTTMessage(this->getZoneName(zoneId), GetStrPmaxZoneEventTypes(Buff->buffer[6]), zoneId, ZONE_STATE_CHANGE);

              //If it is a Violated (motion) event then set zone activated
              if (eventType == ZE_Violated) {
                  zone_motion[zoneId] = true;
              }
        }
    }
    
    const char* getZoneSensorType(unsigned char zoneId)
    {
        if(zoneId < MAX_ZONE_COUNT &&
           zone[zoneId].enrolled)
        {
            return zone[zoneId].sensorType;
        }
        return "Unknown";
    }
    
    void UpdateZonesEnrolledCount() {
        int counter = 0;
        for(int ix=1; ix<MAX_ZONE_COUNT; ix++) {
            zone_motion[ix] = false;
            if (zone[ix].enrolled) {
                counter++;
                max_zone_id_enrolled = ix;
            }
        }
        zones_enrolled_count = counter;
    }
    
    void CheckInactivityTimers() {
        for(int ix=1; ix<=max_zone_id_enrolled; ix++) {
            if (zone_motion[ix]) {
                if ((os_getCurrentTimeSec() - zone[ix].lastEventTime) > inactivity_seconds) {
                    zone_motion[ix]= false;
                    SendMQTTMessage(this->getZoneName(ix), "No Motion", ix, ZONE_STATE_CHANGE);  
                }
            }
        }
    }

    bool sendCommandCC(int valzz) {

        unsigned char buff[] = {0xA1,0x00,0x00,0x07,0x12,0x34,0x00,0x00,0x00,0x00,0x00,0x43}; addPin(buff, 4, true);
        char str[1];
        sprintf(str, "%#X", valzz);
        buff[3] = valzz;
      
        unsigned short i;
        char printBuffer[MAX_BUFFER_SIZE*3+3];
        char *bufptr;      /* Current char in buffer */
        bufptr=printBuffer;
        for (i=0;i<(sizeof(buff));i++) {
            sprintf(bufptr,"%02X ",buff[i]);
            bufptr=bufptr+3;
        }  
      
        DEBUG(LOG_DEBUG,"BufferSize: %d" ,sizeof(buff));
        DEBUG(LOG_DEBUG,"Buffer: %s", printBuffer); 
        return sendBuffer(buff, sizeof(buff));
    }

};
//////////////////////////////////////////////////////////////////////////////////

MyPowerMax pm;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

int telnetDbgLevel = LOG_NO_FILTER; //by default only NO FILTER messages are logged to telnet clients 

#ifdef PM_ENABLE_LAN_BROADCAST
WiFiUDP udp;
unsigned int packetCnt = 0;
#endif

#ifdef PM_ENABLE_TELNET_ACCESS
WiFiServer telnetServer(23); //telnet server
WiFiClient telnetClient;
#endif

#ifdef PM_ENABLE_MQTT_ACCESS
WiFiClient mqttWiFiClient;
PubSubClient mqttClient(mqttWiFiClient);
#endif


// MQTT RELATED -- START ---
void SendMQTTMessage(const char* ZoneOrEvent, const char* WhoOrState, const unsigned char zoneID, int zone_or_system_update) {

  //Serial.println("MQTT Message initialized");
  char message_text[600];
  message_text[0] = '\0';

  //Convert zone ID to text
  char zoneIDtext[10];
  itoa(zoneID, zoneIDtext, 10);

  DEBUG(LOG_NOTICE,"Creating JSON string for MQTT");
  //Build key JSON headers and structure  
  if (zone_or_system_update == ALARM_STATE_CHANGE) {
    //Here we have an alarm status change (zone 0) so put the status into JSON
    strncpy(message_text, "{\r\n\"stat_str\": \"", 600);
    strcat(message_text, ZoneOrEvent);
    strcat(message_text, "\",\r\n\"stat_update_from\": \"");
    strcat(message_text, WhoOrState);
    strcat(message_text, "\"");
    strcat(message_text, "\r\n}\r\n");
    //Send alarm state

    if (mqttClient.publish(mqttAlarmStateTopic, message_text, true) == true) {
       DEBUG(LOG_NOTICE,"Success sending MQTT message");
      } else {
       DEBUG(LOG_NOTICE,"Error sending MQTT message");
      }     
  }
  else if (zone_or_system_update == ZONE_STATE_CHANGE) {
    //Here we have a zone status change so put this information into JSON
    strncpy(message_text, "{\r\n\"zone_id\": \"", 600);
    strcat(message_text, zoneIDtext);
    strcat(message_text, "\",\r\n\"zone_name\": \"");
    strcat(message_text, ZoneOrEvent);
    strcat(message_text, "\",\r\n\"zone_status\": \"");
    strcat(message_text, WhoOrState);
    strcat(message_text, "\"");
    strcat(message_text, "\r\n}\r\n");
    //Send zone state
    if (mqttClient.publish(mqttZoneStateTopic, message_text, true) == true) {
       DEBUG(LOG_NOTICE,"Success sending MQTT message");
      } else {
       DEBUG(LOG_NOTICE,"Error sending MQTT message");
      }  
  }
 
}


void MQTTcallback(char* topic, byte* payload, unsigned int length) {
  // Callback from mqtt remains to be done for powermax.. this is just dummy function for now.
  
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

/*
  // Switch on the LED if an 1 was received as first character
  if ((char)payload[0] == '1') {
    digitalWrite(BUILTIN_LED, LOW);   // Turn the LED on (Note that LOW is the voltage level
    // but actually the LED is on; this is because
    // it is acive low on the ESP-01)
  } else {
    digitalWrite(BUILTIN_LED, HIGH);  // Turn the LED off by making the voltage HIGH
  }
*/
 

}


//MQTT Reconnect if not connected
void reconnect() {
  
  // Loop until we're reconnected
  if (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect("PowerMaxClient", MQTT_USER, MQTT_PASS)) {
      Serial.println("connected");
      //mqttClient.publish("home/alarm/powermax", "Powermax Connected");
      // Once connected, publish an announcement...
      mqttClient.subscribe(mqttAlarmInputTopic);
      mqtt_reconnects++;
    } else {
      mqttFail++;
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      //delay(5000);
    }
  }
}


// MQTT RELATED -- DONE --



#define PRINTF_BUF 512 // define the tmp buffer size (change if desired)
void LOG(const char *format, ...)
{
  char buf[PRINTF_BUF];
  va_list ap;
  
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
#ifdef PM_ENABLE_TELNET_ACCESS  
  if(telnetClient.connected())
  {
    telnetClient.write((const uint8_t *)buf, strlen(buf));
  }
#endif  
  va_end(ap);
}

void handleRoot() {
  //This returns the 'homepage' with links to each other main page
  unsigned long days = 0, hours = 0, minutes = 0;
  unsigned long val = os_getCurrentTimeSec();
  
  days = val / (3600*24);
  val -= days * (3600*24);
  
  hours = val / 3600;
  val -= hours * 3600;
  
  minutes = val / 60;
  val -= minutes*60;

  byte mac[6];
  WiFi.macAddress(mac);

  char szTmp[PRINTF_BUF*2];
  sprintf(szTmp, "<html>"
  		"<b>Dashboard for esp8266 controlled Visonic Powermax.</b><br><br>"
		"MAC Address: %02X%02X%02X%02X%02X%02X<br>"
		"Uptime: %02d:%02d:%02d.%02d<br>Free heap: %u<br>MQTT Status code: %u<br>"
		"MQTT Reconnects: %u<br><br>Web Commands<br>"
		"<a href='/armaway' target='_blank'>Arm Away</a><br>"
		"<a href='/armhome' target='_blank'>Arm Home</a><br>"
		"<a href='/disarm' target='_blank'>Disarm</a><br><br>"
		"<a href='/armhome' target='_blank'>Arm Home</a><br>"
                "<a href='/armawayinstant' target='_blank'>Instant Arm Away</a><br>"
                "<a href='/armhomeinstant' target='_blank'>Instant Arm Home</a><br>"
                "<a href='/alarm' target='_blank'>Sound the Alarm Options</a><br><br>"
		"JSON Endpoints<br>"
		"<a href='/status'>Alarm Status</a><br>"
		"<a href='/settings'>Smart Things Details</a><br>"
		"<a href='/getzonenames'>Get Zone Names</a><br><br>"
		"Configuration<br>"
		"<a href='/config'>inactivity_seconds, ip_for_mqtt, port_for_mqtt, mqtt_user, mqtt_pass</a><br>"
		"<a href='/update'>Update Firmware</a>"
		"</html>",
		mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
		(int)days, (int)hours, (int)minutes, (int)val, 
		ESP.getFreeHeap(),
		mqttClient.state(), 
		mqtt_reconnects);"

  server.send(200, "text/html", szTmp);
}

//writes to webpage without storing large buffers, only small buffer is used to improve performance
class WebOutput : public IOutput
{
  WiFiClient* c;
  char buffer[PRINTF_BUF+1];
  int bufferLen;
  
public:
  WebOutput(WiFiClient* a_c)
  {
    c = a_c;
    bufferLen = 0;
    memset(buffer, 0, sizeof(buffer));
  }
  
  void write(const char* str)
  {
    int len = strlen(str);
    if(len < (PRINTF_BUF/2))
    {
      if(bufferLen+len < PRINTF_BUF)
      {
        strcat(buffer, str);
        bufferLen += len;
        return;
      }
    }
    
    flush();
    c->write(str, len);
        
    yield();
  }

  void flush()
  {
    if(bufferLen)
    {
      c->write((const uint8_t *)buffer, bufferLen);
      buffer[0] = 0;
      bufferLen = 0;
    }   
  }
};

void handleStatus() {
  //This sends full system status including status/panel PIN codes/zones/... etc
  
  //First update zone count to reduce checking on unused zones (not actually needed in this function but called often so a good place to put it)
  pm.UpdateZonesEnrolledCount();

  //Now collect all status information and send it to the user
  WiFiClient client = server.client();
  
  client.print(JsonHeaderText);

  WebOutput out(&client);
  pm.dumpToJson(&out);
  out.flush();

  client.stop();
}

void handleRefresh() {
  //This returns the current arm state in case it gets out of sync somehow
  WiFiClient client = server.client();
  
  client.print(JsonHeaderText);
  client.print("{\"stat_str\":\"");

  client.print(pm.GetStrPmaxSystemStatus(pm.GetSystemStatus()));

  client.print("\"}\r\n");

  client.stop();
}

void handleSettings() {
  //This returns the current arm state in case it gets out of sync somehow
  WiFiClient client = server.client();
  
  client.print(JsonHeaderText);

  client.print("{\"ip_for_mqtt\":\"");
  client.print(IP_FOR_MQTT);
  client.print("\",\r\n\"port_for_mqtt\":");
  client.print(PORT_FOR_MQTT);
  client.print("\",\r\n\"mqtt_user\":");
  client.print(MQTT_USER);
  client.print("\",\r\n\"mqtt_pass\":");
  client.print("*************");
  client.print(",\r\n\"inactivity_seconds\":");
  client.print((inactivity_seconds));
  
  client.print(",\r\n\"neohub_relay_firmware_date\":\"");
  client.print(Firmware_Date);
  client.print(" - ");
  client.print(Firmware_Time);
  
  client.print("\"}\r\n");
  client.stop();
}

void handleGetZoneNames() {
  //This returns all zone names and types
  int sizeofoutputstring = 170;
  char outputstring[sizeofoutputstring+1]; //Used to store the output for each zone to speed output
  char currentzonename[70]; //Max zone name of 70 bytes hence copy in one less to ensure an ending
  char numberasstring[5];

  //First update zone count to reduce checking on unused zones
  pm.UpdateZonesEnrolledCount();

  //Now setup the webserver connection and JSON creation
  WiFiClient client = server.client();

  strncpy(outputstring, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n{\"namedzonesenrolled\":", sizeofoutputstring);
  itoa(zones_enrolled_count, numberasstring, 10);
  strncat(outputstring, numberasstring, _max(0,sizeofoutputstring-strlen(outputstring))); //.c_str()
  strncat(outputstring, ",\r\n\"maxzoneid\":", _max(0,sizeofoutputstring-strlen(outputstring)));
  itoa(max_zone_id_enrolled, numberasstring, 10);
  strncat(outputstring, numberasstring, _max(0,sizeofoutputstring-strlen(outputstring)));
  strncat(outputstring, ",\r\n\"zones\":[\r\n", _max(0,sizeofoutputstring-strlen(outputstring)));
  client.print(outputstring);
  //client.write(outputstring, strlen(outputstring));
  
  for(int ix=1; ix<=max_zone_id_enrolled; ix++) {
     strncpy(currentzonename, pm.getZoneName(ix), 69);
     if (currentzonename != "Unknown") {
        //It isnt unknown so create an output text that we can send back to the client
        strncpy(outputstring, "{\"zoneid\":", sizeofoutputstring);
        itoa(ix, numberasstring, 10);
        strncat(outputstring, numberasstring, _max(0,sizeofoutputstring-strlen(outputstring))); //.c_str()
        strncat(outputstring, ",\r\n\"zonename\":\"", _max(0,sizeofoutputstring-strlen(outputstring)));
        strncat(outputstring, currentzonename, _max(0,sizeofoutputstring-strlen(outputstring)));
        strncat(outputstring, "\",\r\n\"zonetype\":\"", _max(0,sizeofoutputstring-strlen(outputstring)));
        strncat(outputstring, pm.getZoneSensorType(ix), _max(0,sizeofoutputstring-strlen(outputstring)));
        strncat(outputstring, "\"},\r\n", _max(0,sizeofoutputstring-strlen(outputstring)));
        client.print(outputstring);
        //client.write(outputstring, strlen(outputstring));
     }
  }
  client.println("]}");
  
  client.stop();
}

void handleConfig() {
  //Start the response
  bool updateEEPROM = false;
  WiFiClient client = server.client();
  client.print(JsonHeaderText);
  client.print("{\"update_config\":\"success\",\r\n");
  
  //If we have received an inactivity timer variable then update it
  if (server.hasArg("inactivity_seconds")) {
      inactivity_seconds = atoi(server.arg("inactivity_seconds").c_str());
    
      client.print("\"inactivity_seconds\":\"");
      client.print(server.arg("inactivity_seconds").c_str());
      client.print("\",\r\n");
  }

  //If we have received an MQTT IP then update as needed
  if (server.hasArg("ip_for_mqtt")) {
      //Check if it has changed before writing to EEPROM
      if (server.arg("ip_for_mqtt") != IP_FOR_MQTT) {
          server.arg("ip_for_mqtt").toCharArray(IP_FOR_MQTT, sizeof(IP_FOR_MQTT));
          updateEEPROM = true;
      }
      //Respond with current value, even if not needing an update
      client.print("\"ip_for_mqtt\":\"");
      client.print(server.arg("ip_for_mqtt").c_str());
      client.print("\",\r\n");
  }

  //If we have received an MQTT Port then update as needed
  if (server.hasArg("port_for_mqtt")) {
      //Check if it has changed before writing to EEPROM
      if (server.arg("port_for_mqtt") != PORT_FOR_MQTT) { 
          server.arg("port_for_mqtt").toCharArray(PORT_FOR_MQTT, sizeof(PORT_FOR_MQTT));
          updateEEPROM = true;
      }
      //Respond with current value, even if not needing an update
      client.print("\"port_for_mqtt\":\"");
      client.print(server.arg("port_for_mqtt").c_str());
      client.print("\",\r\n");
  }

  //If we have received an MQTT Username then update as needed
  if (server.hasArg("mqtt_user")) {
      //Check if it has changed before writing to EEPROM
      if (server.arg("mqtt_user") != MQTT_USER) { 
          server.arg("mqtt_user").toCharArray(MQTT_USER, sizeof(MQTT_USER));
          updateEEPROM = true;
      }
      //Respond with current value, even if not needing an update
      client.print("\"mqtt_user\":\"");
      client.print(server.arg("mqtt_user").c_str());
      client.print("\",\r\n");
  }

  //If we have received an MQTT Password then update as needed
  if (server.hasArg("mqtt_pass")) {
      //Check if it has changed before writing to EEPROM
      if (server.arg("mqtt_pass") != MQTT_PASS) { 
          server.arg("mqtt_pass").toCharArray(MQTT_PASS, sizeof(MQTT_PASS));
          updateEEPROM = true;
      }
      //Respond with current value, even if not needing an update
      client.print("\"mqtt_pass\":\"");
      client.print(server.arg("mqtt_pass").c_str());
      client.print("\",\r\n");
  }


  client.print("}\r\n");
  client.stop();

  //Now save the values to EEPROM if updated values received
  if (updateEEPROM) {
      REFRESH_INTERVAL = 5000;
      mqttClient.setServer(IP_FOR_MQTT, atoi(PORT_FOR_MQTT));
      WriteEEPROMSettings();
  }

}

void handleAlarm() {
  //Check for a method and if so, try to process it
  if (server.hasArg("method")) {
    int tempnumber = -1;
    
    WiFiClient client = server.client();
    client.print(JsonHeaderText);
    client.print("{\"alarm_triggered\":\"true\"}");
    
    if (isDigit(server.arg("method").charAt(0))) {
      tempnumber = (int)strtol(server.arg("method").c_str(), NULL, 10);
      //If pin number between 0 and 16 then probably valid pin so lets try switching it!
      if ((tempnumber <= 16) && (tempnumber >= 0)) {
        if (activatedPinA == -1) {
          activatedPinA = tempnumber;
        }
        else {
          activatedPinB = tempnumber;
        }
        userPinResetTime = millis();
        digitalWrite(tempnumber, HIGH);
        pinMode(tempnumber, OUTPUT);
        digitalWrite(tempnumber, LOW);
      }
    }
    else if (server.arg("method") == "Serial") {
      //Found word serial so send serial trigger to alarm
      handleTriggerAlarm();
    }
    
    //Can't easily close the window now, buts leave here anyway
    //server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  }
  else {
    //No command so send a response to say so
    server.send(200, "text/html", "<html><b>To trigger an alarm you must add the argument 'method' e.g. /alarm?method=12 - but use carefully as some pins may crash the Wemos!</b></html>");
    return;
  }
}

void handleTriggerAlarm() {
  DEBUG(LOG_NOTICE,"Trigger Alarm Command received from Web");
  pm.sendCommand(Pmax_ALARM);
}

void handleArmAway() {
  DEBUG(LOG_NOTICE,"Arm Away Command received from Web");
  pm.sendCommand(Pmax_ARMAWAY);        
}

void handleArmHome() {
  DEBUG(LOG_NOTICE,"Arm Home command received from Web");
  pm.sendCommand(Pmax_ARMHOME);
}

void handleDisarm() {
  DEBUG(LOG_NOTICE,"Disarm command received from Web");
  pm.sendCommand(Pmax_DISARM);
}

void handleArmAwayInstant() {
  DEBUG(LOG_NOTICE,"Instant Arm Away Command from Web");
  pm.sendCommand(Pmax_ARMAWAY_INSTANT);
}

void handleArmHomeInstant() {
  DEBUG(LOG_NOTICE,"Instant Arm Home command from Web");
  pm.sendCommand(Pmax_ARMHOME_INSTANT);
}

void handleRestart() {
  //This restarts the ESP from a web command in case it is needed
  server.send(200, "text/plain", "ESP is restarting (can also reset with changed URL)");
  server.stop();
  delay(2000);
  ESP.restart();
}

void handleReboot() {
  handleRestart();
}

void handleReset() {
  //This resets the ESP from a web command in case it is needed
  server.send(200, "text/plain", "ESP is resetting (can also restart with changed URL)");
  server.stop();
  delay(2000);
  ESP.reset();
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void handleTest() {
  //Start the response
  WiFiClient client = server.client();
  client.print(JsonHeaderText);
  client.print("{\"test\":\"");
  
  //If we have received an inactivity timer variable then update it
  if (server.hasArg("value")) {
      char str[8];
      int tempnumber = atoi(server.arg("value").c_str());
      sprintf(str, "%#0X", tempnumber);
      client.print(str);
      client.print("\"}\r\n");
      pm.sendCommandCC(tempnumber);
      client.stop();
  }
}

void setup(void){

  //First lets make a few common output pins as high just to prevent accidental triggering of the alarm
  //Paranoid process is to set it high, then make it an output, and then set high just to be super safe
  digitalWrite(12, HIGH);
  digitalWrite(13, HIGH);
  digitalWrite(14, HIGH);
  digitalWrite(0, HIGH);

  Serial.begin(9600); //connect to PowerMax interface

  //Add the EEPROM in order to save values for ST and immediately read IP and port for SmartThings
  EEPROM.begin(512);
  ReadEEPROMSettings();

  /*
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  */

  
  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  //reset settings - for testing
  //wifiManager.resetSettings();

  //sets timeout until configuration portal gets turned off
  //useful to make it all retry or go to sleep
  //in seconds
  wifiManager.setTimeout(180);
  
  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  if(!wifiManager.autoConnect("VisonicPowermaxBridge")) {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
  }
  
  // Set up mDNS responder:
  // - first argument is the domain name, in this example
  //   the fully-qualified domain name is "esp8266.local"
  // - second argument is the IP address to advertise
  //   we send our IP address on the WiFi network
  if (!MDNS.begin("esp8266-PowermaxBridge")) {
    Serial.println("Error setting up MDNS responder!");
    while(1) { 
      delay(1000);
    }
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/settings", handleSettings);
  server.on("/refresh", handleRefresh);
  server.on("/getzonenames", handleGetZoneNames);
  server.on("/config", handleConfig);
  server.on("/restart", handleRestart);
  server.on("/reset", handleReset);
  server.on("/reboot", handleReboot);
  server.on("/test", handleTest);
  server.on("/alarm", handleAlarm);
  server.on("/armaway", [](){
    handleArmAway();
    server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  });
  server.on("/armhome", [](){
    handleArmHome();
    server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  });
  server.on("/disarm", [](){
    handleDisarm();
    server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  });
  server.on("/armawayinstant", [](){
    handleArmAwayInstant();
    server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  });
  server.on("/armhomeinstant", [](){
    handleArmHomeInstant();
    server.send(200, "text/html", " {} <body> window.onload = <script> window.close() </script>; </body>");
  });
  server.on("/ping", [](){
    server.send(200, "text/plain", "{\"ping_alive\": true}");
  });
  server.on("/description.xml", HTTP_GET, [](){
    SSDP.schema(server.client());
  });

#ifdef PM_ENABLE_OTA_UPDATES
    //Link the HTTP Updater with the web sesrver
    httpUpdater.setup(&server);
    ArduinoOTA.begin();
#endif

#ifdef PM_ENABLE_MQTT_ACCESS
    mqttClient.setServer(IP_FOR_MQTT, atoi(PORT_FOR_MQTT));
    mqttClient.setCallback(MQTTcallback);
#endif


  server.onNotFound(handleNotFound);
  server.begin();

  //Now setup variables for use in SSDP section
  char macstring[16];
  byte mac[6];
  WiFi.macAddress(mac);
  sprintf(macstring, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  //Now initialise SSDP
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("Powermax Alarm Interface Device");
  SSDP.setSerialNumber(macstring);
  SSDP.setURL("\\");
  SSDP.setModelName("Powermax Alarm ESP Interface");
  SSDP.setModelNumber(macstring);
  SSDP.setModelURL("https://community.smartthings.com/t/release-visonic-powermax-alarm/84119");
  SSDP.setManufacturer("Chris Charles");
  SSDP.setManufacturerURL("https://community.smartthings.com/t/release-visonic-powermax-alarm/84119");
  SSDP.setDeviceType("upnp:rootdevice");
  SSDP.begin();

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

#ifdef PM_ENABLE_TELNET_ACCESS
  telnetServer.begin();
  telnetServer.setNoDelay(true);
#endif

  //if you have a fast board (like PowerMax Complete) you can pass 0 to init function like this: pm.init(0);
  //this will speed up the boot process, keep it as it is, if you have issues downloading the settings from the board.
  pm.init();
}

#ifdef PM_ENABLE_TELNET_ACCESS
void handleNewTelnetClients()
{
  if(telnetServer.hasClient())
  {
    if(telnetClient.connected())
    {
      //no free/disconnected spot so reject
      WiFiClient newClient = telnetServer.available();
      newClient.stop();
    }
    else
    {
      telnetClient = telnetServer.available();
      LOG("Connected to WiFi, type '?' for help.\r\n");
    }
  }
}

void runDirectRelayLoop()
{
  while(telnetClient.connected())
  { 
    bool wait = true;
    
    //we want to read/write in bulk as it's much faster than read one byte -> write one byte (this is known to create problems with PowerMax)
    unsigned char buffer[256];
    int readCnt = 0;
    while(readCnt < 256)
    {
      if(Serial.available())
      {
        buffer[readCnt] = Serial.read();
        readCnt++;
        wait = false;
      }
      else
      {
        break;
      }
    }
    
    if(readCnt > 0)
    {
      telnetClient.write((const uint8_t *)buffer, readCnt );
    }

    if(telnetClient.available())
    {
      Serial.write( telnetClient.read() );
      wait = false;
    }

    if(wait)
    {
      delay(10);
    }
  } 
}

void handleTelnetRequests(PowerMaxAlarm* pm) {
  char c; 
  if ( telnetClient.connected() && telnetClient.available() )  {
    c = telnetClient.read();

    if(pm->isConfigParsed() == false &&
       (c == 'h' || //arm home
        c == 'a' || //arm away
        c == 'd'))  //disarm
    {
        //DEBUG(LOG_NOTICE,"EPROM is not download yet (no PIN requred to perform this operation)");
        return;
    }

#ifdef PM_ALLOW_CONTROL
    if ( c == 'h' ) {
      DEBUG(LOG_NOTICE,"Arming home");
      pm->sendCommand(Pmax_ARMHOME);
    }
    else if ( c == 'd' ) {
      DEBUG(LOG_NOTICE,"Disarm");
      pm->sendCommand(Pmax_DISARM);
    }  
    else if ( c == 'a' ) {
      DEBUG(LOG_NOTICE,"Arming away");
      pm->sendCommand(Pmax_ARMAWAY);
    }
    else if ( c == 'D' ) {
      DEBUG(LOG_NO_FILTER,"Direct relay enabled, disconnect to stop...");
      runDirectRelayLoop();
    }
#endif
 
    if ( c == 'g' ) {
      DEBUG(LOG_NOTICE,"Get Event log");
      pm->sendCommand(Pmax_GETEVENTLOG);
    }
    else if ( c == 't' ) {
      DEBUG(LOG_NOTICE,"Restore comms");
      pm->sendCommand(Pmax_RESTORE);
    }
    else if ( c == 'v' ) {
      DEBUG(LOG_NOTICE,"Exit Download mode");
      pm->sendCommand(Pmax_DL_EXIT);
    }
    else if ( c == 'r' ) {
      DEBUG(LOG_NOTICE,"Request Status Update");
      pm->sendCommand(Pmax_REQSTATUS);
    }
    else if ( c == 'j' ) {
        ConsoleOutput out;
        pm->dumpToJson(&out);
    }
    else if ( c == 'c' ) {
        DEBUG(LOG_NOTICE,"Exiting...");
        telnetClient.stop();
    }    
    else if( c == 'C' ) {
      DEBUG(LOG_NOTICE,"Reseting...");
      ESP.reset();
    }    
    else if ( c == 'p' ) {
      telnetDbgLevel = LOG_DEBUG;
      DEBUG(LOG_NOTICE,"Debug Logs enabled type 'P' (capital) to disable");
    }    
    else if ( c == 'P' ) {
      telnetDbgLevel = LOG_NO_FILTER;
      DEBUG(LOG_NO_FILTER,"Debug Logs disabled");
    }       
    else if ( c == 'H' ) {
      DEBUG(LOG_NO_FILTER,"Free Heap: %u", ESP.getFreeHeap());
    }  
    else if ( c == '?' )
    {
        DEBUG(LOG_NO_FILTER,"Allowed commands:");
        DEBUG(LOG_NO_FILTER,"\t c - exit");
        DEBUG(LOG_NO_FILTER,"\t C - reset device");        
        DEBUG(LOG_NO_FILTER,"\t p - output debug messages");
        DEBUG(LOG_NO_FILTER,"\t P - stop outputing debug messages");
#ifdef PM_ALLOW_CONTROL        
        DEBUG(LOG_NO_FILTER,"\t h - Arm Home");
        DEBUG(LOG_NO_FILTER,"\t d - Disarm");
        DEBUG(LOG_NO_FILTER,"\t a - Arm Away");
        DEBUG(LOG_NO_FILTER,"\t D - Direct mode (relay all bytes from client to PMC and back with no processing, close connection to exit");  
#endif        
        DEBUG(LOG_NO_FILTER,"\t g - Get Event Log");
        DEBUG(LOG_NO_FILTER,"\t t - Restore Comms");
        DEBUG(LOG_NO_FILTER,"\t v - Exit download mode");
        DEBUG(LOG_NO_FILTER,"\t r - Request Status Update");
        DEBUG(LOG_NO_FILTER,"\t j - Dump Application Status to JSON");  
        DEBUG(LOG_NO_FILTER,"\t H - Get free heap");  

        
    }
  }
}
#endif

#ifdef PM_ENABLE_LAN_BROADCAST
void broadcastPacketOnLan(const PlinkBuffer* commandBuffer, bool packetOk)
{
  udp.beginPacketMulticast(PM_LAN_BROADCAST_IP, PM_LAN_BROADCAST_PORT, WiFi.localIP());
  udp.write("{ data: [");
  for(int ix=0; ix<commandBuffer->size; ix++)
  {
    char szDigit[20];
    //sprintf(szDigit, "%d", commandBuffer->buffer[ix]);
    sprintf(szDigit, "%02x", commandBuffer->buffer[ix]); //IZIZTODO: temp

    if(ix+1 != commandBuffer->size)
    {
      strcat(szDigit, ", ");
    }

    udp.write(szDigit);
  }
  
  udp.write("], ok: ");
  if(packetOk)
  {
    udp.write("true");
  }
  else
  {
    udp.write("false");
  }

  char szTmp[50];
  sprintf(szTmp, ", seq: %u }", packetCnt++);
  udp.write(szTmp);
  
  udp.endPacket();
}
#endif

bool serialHandler(PowerMaxAlarm* pm) {

  bool packetHandled = false;
  
  PlinkBuffer commandBuffer ;
  memset(&commandBuffer, 0, sizeof(commandBuffer));
  
  char oneByte = 0;  
  while (  (os_pmComPortRead(&oneByte, 1) == 1)  ) 
  {     
    if (commandBuffer.size<(MAX_BUFFER_SIZE-1))
    {
      *(commandBuffer.size+commandBuffer.buffer) = oneByte;
      commandBuffer.size++;
    
      if(oneByte == 0x0A) //postamble received, let's see if we have full message
      {
        if(PowerMaxAlarm::isBufferOK(&commandBuffer))
        {
          DEBUG(LOG_INFO,"--- new packet %d ----", millis());

#ifdef PM_ENABLE_LAN_BROADCAST
          broadcastPacketOnLan(&commandBuffer, true);
#endif

          packetHandled = true;
          pm->handlePacket(&commandBuffer);
          commandBuffer.size = 0;
          break;
        }
      }
    }
    else
    {
      DEBUG(LOG_WARNING,"Packet too big detected");
    }
  }

  if(commandBuffer.size > 0)
  {
#ifdef PM_ENABLE_LAN_BROADCAST
    broadcastPacketOnLan(&commandBuffer, false);
#endif
    
    packetHandled = true;
    //this will be an invalid packet:
    DEBUG(LOG_WARNING,"Passing invalid packet to packetManager");
    pm->handlePacket(&commandBuffer);
  }

  return packetHandled;
}

void loop(void){
#ifdef PM_ENABLE_OTA_UPDATES
  ArduinoOTA.handle();
#endif

  server.handleClient();

  //Here we also check if we need to send any inactivity messages to ST
  pm.CheckInactivityTimers();

  //Here we reset manual pin timers in case they are running (i.e. output relay pin is on)
  if (userPinResetTime > 0) {
    if ((millis() - userPinResetTime >= 5000)) {
      userPinResetTime = 0;
      if (activatedPinA != -1) {
        digitalWrite(activatedPinA, HIGH);
        activatedPinA = -1;
      }
      if (activatedPinB != -1) {
        digitalWrite(activatedPinB, HIGH);
        activatedPinB = -1;
      }
    }
  }

  static unsigned long lastMsg = 0;
  if(serialHandler(&pm) == true)
  {
    lastMsg = millis();
  }

  if(millis() - lastMsg > 300 || millis() < lastMsg) //we ensure a small delay between commands, as it can confuse the alarm (it has a slow CPU)
  {
    pm.sendNextCommand();
  }

  if(pm.restoreCommsIfLost()) //if we fail to get PINGs from the alarm - we will attempt to restore the connection
  {
      DEBUG(LOG_WARNING,"Connection lost. Sending RESTORE request.");   
  }   

#ifdef PM_ENABLE_TELNET_ACCESS
  handleNewTelnetClients();
  handleTelnetRequests(&pm);
#endif  


#ifdef PM_ENABLE_MQTT_ACCESS

  if (!mqttClient.connected() && (millis() - lastRefreshTime >= REFRESH_INTERVAL)) {
    lastRefreshTime += REFRESH_INTERVAL;
    reconnect();
    if (mqttFail > 3) {
        REFRESH_INTERVAL = 60000;
        mqttFail = 0;
    }  
  }
  mqttClient.loop();
#endif

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//This file contains OS specific implementation for ESP8266 used by PowerMax library
//If you build for other platrorms (like Linux or Windows, don't include this file, but provide your own)

int log_console_setlogmask(int mask)
{
  int oldmask = telnetDbgLevel;
  if(mask == 0)
    return oldmask; /* POSIX definition for 0 mask */
  telnetDbgLevel = mask;
  return oldmask;
} 

void os_debugLog(int priority, bool raw, const char *function, int line, const char *format, ...)
{
  if(priority <= telnetDbgLevel)
  {
    char buf[PRINTF_BUF];
    va_list ap;
    
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    
  #ifdef PM_ENABLE_TELNET_ACCESS
    if(telnetClient.connected())
    { 
      telnetClient.write((const uint8_t *)buf, strlen(buf));
      if(raw == false)
      {
        telnetClient.write((const uint8_t *)"\r\n", 2);
      }    
    }
  #endif
    
    va_end(ap);
  
    yield();
    }
}

void os_usleep(int microseconds)
{
    delay(microseconds / 1000);
}

unsigned long os_getCurrentTimeSec()
{
  static unsigned int wrapCnt = 0;
  static unsigned long lastVal = 0;
  unsigned long currentVal = millis();

  if(currentVal < lastVal)
  {
    wrapCnt++;
  }

  unsigned long seconds = currentVal/1000;
  
  //millis will wrap each 50 days, as we are interested only in seconds, let's keep the wrap counter
  return (wrapCnt*4294967) + seconds;
}

int os_pmComPortRead(void* readBuff, int bytesToRead)
{
    int dwTotalRead = 0;
    while(bytesToRead > 0)
    {
        for(int ix=0; ix<10; ix++)
        {
          if(Serial.available())
          {
            break;
          }
          delay(5);
        }
        
        if(Serial.available() == false)
        {
            break;
        }

        *((char*)readBuff) = Serial.read();
        dwTotalRead ++;
        readBuff = ((char*)readBuff) + 1;
        bytesToRead--;
    }

    return dwTotalRead;
}

int os_pmComPortWrite(const void* dataToWrite, int bytesToWrite)
{
    Serial.write((const uint8_t*)dataToWrite, bytesToWrite);
    return bytesToWrite;
}

bool os_pmComPortClose()
{
    return true;
}

bool os_pmComPortInit(const char* portName) {
    return true;
} 

void os_strncat_s(char* dst, int dst_size, const char* src)
{
    strncat(dst, src, dst_size);
}

int os_cfg_getPacketTimeout()
{
    return PACKET_TIMEOUT_DEFINED;
}

//see PowerMaxAlarm::setDateTime for details of the parameters, if your OS does not have a RTC clock, simply return false
bool os_getLocalTime(unsigned char& year, unsigned char& month, unsigned char& day, unsigned char& hour, unsigned char& minutes, unsigned char& seconds)
{
    return false; //IZIZTODO
}
//////End of OS specific part of PM library/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
