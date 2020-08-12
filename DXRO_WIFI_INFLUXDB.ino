//start include section - FS should be first if not bad things would happen
#include "FS.h"
#include <Wire.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <DHT.h>
//end include 

#define I2C_ADDRESS 0x3C      //oled LCD i2c address

SSD1306AsciiWire oled;        //oled constructor
HTTPClient http;              //HTTP client constructor
Ticker TASK_01;               //task constructor
DHT11 dht11_sensor_1;         //DHT11 sensor constructor

volatile float humidity = 0;      //gonna run in ISR so ... it has to be volatile
volatile float temperature = 0;   //same shit ... ISR ... volatile ...
volatile uint8_t error = 0;       //store the error
volatile int8_t result = 0;       //and the result

char Http_Payload[255];       //HTTP POST payload buffer - 255 is more than enough (probably less than that is needed)

const char* sensorid = "";    //sensor id from json
const char* zn = "";          //zone id from json
const char* ssid = "";        //wifi ssid from json
const char* pass = "";        //ssid password
const char* idb = "";         //influxdb IP address
const char* idbp = "";        //influxdb port
const char* st = "";          //sampling time

char json[210];                     //json char buffer
StaticJsonBuffer<209> jsonBuffer;   //static JSON constructor

void DataTicker(){
  dht11_sensor_1.read();            //read the sensor when sample time had passed (configurable in the JSON)
}

void ICACHE_RAM_ATTR handleData(float h, float t) {     //put it in RAM and handle the data from the sensor
  humidity = h;
  temperature = t;
  result = 1;
}

void ICACHE_RAM_ATTR handleError(uint8_t e) {           //put in in RAM and handle the error
  error = e;
  result = -1;
}

void ReadJsonCfg(){                 //ok now is time to read the JSON config avialable in the SPIFFS 
  oled.clear();
  
  bool success = SPIFFS.begin();
  if (!success) {
    oled.println("SPIFFS ERROR!");
    oled.println("SENSOR LOCKED!");
    while(1){
      //this loop is added here to prevent starting the program in case SPIFFS is corrupted or cannot be read
      //using exit(0) is not advised in embedded systems - so locking in a loop is the best way to do it
    }
  }
  
  File cfg_json = SPIFFS.open("/DEXRO.json", "r");    //open the file from SPIFFS - hopefully will be there
 
  if (!cfg_json) {
    oled.println("CFG FILE ERR!");
    oled.println("SENSOR LOCKED!");
    while(1){
      //this loop is added here to prevent starting the program in case configuration file is not present or is corrupted or cannot be read
      //using exit(0) is not advised in embedded systems - so locking in a loop is the best way to do it
    }    
  }
 
  cfg_json.readString().toCharArray(json, 200);     //read the whole content of json file as string (is easier)
  cfg_json.close();                                 //close the file
  
  JsonObject& root = jsonBuffer.parseObject(json);  //parse the json char array into a json structure
  if(!root.success()) {
    oled.println("JSON CFG FAIL!");
    oled.println("SENSOR LOCKED!");
    while(1){
      //this loop is added here to prevent starting the program in case configuration file is not a valid JSON structure
      //using exit(0) is not advised in embedded systems - so locking in a loop is the best way to do it
    }    
  }
  
  sensorid = root["ID"];                //read sensor ID
  zn = root["ZN"];                      //read sensor Zone
  ssid = root["SSID"];                  //read wifi SSID
  pass = root["PASS"];                  //read wifi passwd
  idb = root["IDB"];                    //read InfluxDB IP
  idbp = root["IDBP"];                  //read InfluxDB port
  st = root["ST"];                      //read sampling time
  jsonBuffer.clear();                   //clear the json structure - no need to keep it in RAM
}

void setup() {
  WiFi.disconnect();                    //just make sure the WiFi is not going to connect randomly to something from memory 
  
  Serial.begin(115200);                 //debug only to be removed @ release
  
  //---Oled LCD Init
  Wire.begin();
  Wire.setClock(400000L);
  oled.begin(&Adafruit128x32, I2C_ADDRESS);
  oled.setFont(font5x7);
  ////---END Oled LCD Init
  
  oled.clear();                         //clear the lcd
  
  ReadJsonCfg();                        //run the configuration rutine to read the json file in SPIFFS

  WiFi.mode(WIFI_STA);                  //wifi mode is client not AP
  WiFi.begin(ssid, pass);               //use the JSON ssid and pass to connect

  oled.println("=CONNECTING TO WIFI=");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    oled.print(".");
  }
  
  oled.clear();
  oled.println("WIFI CONFIGURATION");
  oled.print("IP: ");
  oled.println(WiFi.localIP());
  oled.print("NM: ");
  oled.println(WiFi.subnetMask());
  oled.print("GW: ");
  oled.print(WiFi.gatewayIP());
  
  dht11_sensor_1.setup(0);                //setup DHT11 sensor on D0
  dht11_sensor_1.onData(handleData);      //callback for data handle
  dht11_sensor_1.onError(handleError);    //callback for error handle 
  
  TASK_01.attach(atoi(st), DataTicker);   //set the ticker with the value from JSON config file
}
//------------------------------------------------------------------------------

void loop() {
  if (WiFi.status() == WL_CONNECTED){
    if (result > 0) {
        getData();
        delay(1000);
        WifiStatus();
        
    } else if (result < 0) {
        oled.clear();
        oled.println("SNSR READ ERR");
    }
    result = 0;
  }
  
}

void WifiStatus(){
  if (WiFi.status() == WL_CONNECTED){
    oled.clear();
    oled.println("WIFI STAT: OK");
    oled.print("SSID: ");
    oled.println(ssid);
    oled.print("RSSI: ");
    oled.print(WiFi.RSSI());
    oled.print(" dBm");
  } 
  else{
    oled.clear();
    oled.println("WIFI STAT: ERR");
    delay(1000);
  }
}

void getData(){
    oled.clear();
    oled.print("TEMPERATURE: ");
    oled.print((int)temperature);
    oled.println(" *C");
    oled.print("HUMIDITY: "); 
    oled.print((int)humidity-7); 
    oled.println(" %");
    delay (1000);
    SendData(temperature, humidity-7);
  }


void SendData(byte temp, byte humid){
  
  http.begin(idb,atoi(idbp), "/write?db=dexro&precision=ms"); //connect to the influxdb using REST API idb=server ip address; idbp=server port (needs an int - use atoi to convert from char array to int)
  http.addHeader("Content-Type", "text/plain");               //set content type to plain text
  
  sprintf(Http_Payload,"sensors,sensor_id=%s,location=%s temperature=%i,humidity=%i",sensorid, zn, temp, humid); //use sprintf to construct the request string
  Serial.println(Http_Payload);                                                                                  //to be removed @ release 
  int httpCode = http.POST(Http_Payload);                                                                        //make a POST and get the status code

  
  if ((httpCode == 200) or (httpCode == 204)) {               //see if is 200 or 204 (204 is request ok but no content received)
    oled.clear();
    oled.println("   DATA SENT OK!");
    http.end();                                               //close the http connection ... and DONE
    oled.print("CLOSING CONNECTION");
  } else {
    oled.clear();
    oled.println("SERVICE IS DOWN");
    http.end();                                               //close the http connection ... and DONE
    oled.print("CLOSING CONNECTION");
  }
                                                  
}
