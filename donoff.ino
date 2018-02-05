/*
 * The alternative firmware for Sonoff switch and S20 socket.
 * 
 * Based (copy-pasted, adjusted) on works of:
 * - KmanOz: https://github.com/KmanOz/KmanSonoff/
 * - El Ape: http://www.instructables.com/id/Hacking-a-Sonoff-to-Work-With-Home-Assistant-and-M/
 * 
 * donoff.ino
 *
 *  Created on: Jan 31, 2018
 *      Author: Darau, blė
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <Ticker.h>

const PROGMEM char* CFG_FILE        = "/cfg.json";
const PROGMEM char* ESP_ID          = "%06X";
const PROGMEM char* HOST_NAME       = "DONOFF-%s";
const PROGMEM char* MQTT_BASE_TOPIC = "darauble/donoff/%s/%s";
const PROGMEM char* MQTT_STATUS     = "status";
const PROGMEM char* MQTT_SWITCH     = "switch";
const PROGMEM char* MQTT_LWT        = "hb";
const PROGMEM char* SW_ON           = "on";
const PROGMEM char* SW_OFF          = "off";
const PROGMEM char* ONLINE          = "online";
const PROGMEM char* OFFLINE         = "offline";

#define D0              16        //WAKE  =>  16
#define D1              5         //IOS   =>  5
#define D2              4         //      =>  4
#define D3              0         //      =>  0
#define D4              2         //      =>  2
#define D5              14        //CLK   =>  14
#define D6              12        //MISO  =>  12 
#define D7              13        //MOSI  =>  13
#define D8              15        //CS    =>  15
#define D9              3         //RX    =>  3

#define RELAY_PIN       D6
#define BUTTON_PIN      D3
#define LED_PIN         D7

#define ESP_ID_LEN 7
#define TOPIC_LEN 30
#define HOSTNAME_LEN 20
#define SERVER_LEN 40
#define PORT_LEN 6
#define USR_PWD_LEN 20

#define RESET_CNT 100
#define MQTT_RETRY_CNT 10

char hostname[HOSTNAME_LEN];
char esp_id[ESP_ID_LEN];

char mqtt_server[SERVER_LEN];
char mqtt_port[PORT_LEN] = "1883";
char mqtt_usr[USR_PWD_LEN];
char mqtt_pwd[USR_PWD_LEN];

char status_topic[TOPIC_LEN];
char switch_topic[TOPIC_LEN];
char lwt_topic[TOPIC_LEN];

//int light = 0;
boolean saveConfig = false;
boolean OTAupdate = false;

Ticker btn_timer;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
IPAddress mqtt_server_ip;

/*void ISR_Reset(){
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  ESP.reset();  
}//*/


/*void buttonDown() {
  Serial.println("Button down!");
  light = !light;
}//*/

boolean resetFlag = false;
void check_reset()
{
  if (resetFlag) {
    WiFi.disconnect();
    delay(3000);
    ESP.reset();
    delay(3000);
  }
}

int sendStatus = 0;
unsigned long count = 0;

void button()
{
  if (!digitalRead(BUTTON_PIN)) {
    count++;
  } 
  else {
    if (count > 1 && count <= RESET_CNT) {   
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
      sendStatus = true;
    } 
    else if (count > RESET_CNT){
      Serial.println("Schedule reset"); 
      resetFlag = true;
    } 
    count=0;
  }
}

void saveConfigCb()
{
  Serial.println("Schedule config save");
  saveConfig = true;
}

void subCb(char* t, byte* p, unsigned int plen)
{
  if (strcmp(t, switch_topic) == 0) {
    if (strncmp((char*) p, SW_ON, plen) == 0) {
      digitalWrite(LED_PIN, LOW);
      digitalWrite(RELAY_PIN, HIGH);
    } else if (strncmp((char*) p, SW_OFF, plen) == 0) {
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(RELAY_PIN, LOW);
    }
    sendStatus = true;
  }
}

void initStrings()
{
  sprintf(esp_id, ESP_ID, ESP.getChipId());
  sprintf(hostname, HOST_NAME, esp_id);
  sprintf(status_topic, MQTT_BASE_TOPIC, esp_id, MQTT_STATUS);
  sprintf(switch_topic, MQTT_BASE_TOPIC, esp_id, MQTT_SWITCH);
  sprintf(lwt_topic, MQTT_BASE_TOPIC, esp_id, MQTT_LWT);
  //Serial.print("Host name: ");
  //Serial.println(hostname);
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Darau, ble - Sonoff hack");

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(RELAY_PIN, LOW);

  initStrings();
  
  //attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonDown, FALLING);

  btn_timer.attach(0.05, button);
  
  // Read config
  Serial.println("Mount SPIFFS...");
  if (SPIFFS.begin()) {
    if (SPIFFS.exists(CFG_FILE)) {
      Serial.println("Read config...");
      File configFile = SPIFFS.open(CFG_FILE, "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_usr, json["mqtt_usr"]);
          strcpy(mqtt_pwd, json["mqtt_pwd"]);
        } else {
          Serial.println("Failed to parse json");
        }
      }
    } else {
      Serial.println("No config yet.");
    }
  } else {
    Serial.println("SPIFFS failed :-(");
  }

  WiFi.hostname(hostname);

  // WifiManager init
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, SERVER_LEN);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, PORT_LEN);
  WiFiManagerParameter custom_mqtt_usr("usr", "mqtt usr", mqtt_usr, USR_PWD_LEN);
  WiFiManagerParameter custom_mqtt_pwd("pwd", "mqtt pwd", mqtt_pwd, USR_PWD_LEN);

  WiFiManager wifiManager;

  wifiManager.setSaveConfigCallback(saveConfigCb);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_usr);
  wifiManager.addParameter(&custom_mqtt_pwd);

  if (!wifiManager.autoConnect(hostname)) {
    delay(3000);
    ESP.reset();
    delay(3000);
  }

  Serial.println("Connected to WiFi");
  
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_usr, custom_mqtt_usr.getValue());
  strcpy(mqtt_pwd, custom_mqtt_pwd.getValue());

  if (saveConfig) {
    Serial.println("Save the config!");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_usr"] = mqtt_usr;
    json["mqtt_pwd"] = mqtt_pwd;

    File configFile = SPIFFS.open(CFG_FILE, "w");
    if (configFile) {
      json.printTo(Serial);
      json.printTo(configFile);
      configFile.close();
    } else {
      Serial.println("Failed to open config file for writing");
    }
    
    saveConfig = false;
  }

  WiFi.hostByName(mqtt_server, mqtt_server_ip);
  mqttClient.setServer(mqtt_server_ip, atoi(mqtt_port));
  mqttClient.setCallback(subCb);

  for (int i=0; i<MQTT_RETRY_CNT; i++) {
    if (strlen(mqtt_usr) > 0) {
      if (!mqttClient.connect(hostname, mqtt_usr, mqtt_pwd, lwt_topic, 0, 1, OFFLINE)) {
        delay(3000);
      }
    } else {
      if (!mqttClient.connect(hostname, lwt_topic, 0, 1, OFFLINE)) {
        delay(3000);
      }
    }
  }

  if (mqttClient.connected()) {
    mqttClient.subscribe(switch_topic);
    mqttClient.publish(lwt_topic, ONLINE, 1);
  } else {
    Serial.println("MQTT connection failed!");
  }

  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword((const char *)WiFi.psk().c_str());
  ArduinoOTA.onStart([]() {
    OTAupdate = true;
    //blinkLED(LED, 400, 2);
    digitalWrite(LED_PIN, HIGH);
    Serial.println("OTA start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA End");
    ESP.restart();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    digitalWrite(LED_PIN, LOW);
    delay(5);
    digitalWrite(LED_PIN, HIGH);
    Serial.printf("Progress: %u%%\r\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    //blinkLED(LED, 40, 2);
    OTAupdate = false;
    Serial.printf("OTA Error [%u] ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("OTA Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("OTA Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("OTA  Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("OTA  Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("OTA End Failed");
  });
  ArduinoOTA.begin();

  // Send initial status on startup
  sendStatus = true;
}

void check_status()
{
  if (sendStatus) {
    if (digitalRead(RELAY_PIN) == LOW) {
      mqttClient.publish(status_topic, SW_OFF, true);
    } else {
      mqttClient.publish(status_topic, SW_ON, true);
    }
    sendStatus = false;
  }
}

boolean restartFlag = false;

void check_restart()
{
  if (restartFlag) {
    delay(3000);
    ESP.restart();
  }
}

int connect_ms = 0;

void check_connection()
{
  int cur_ms = millis();
  if (cur_ms > connect_ms+60000 || cur_ms < connect_ms) {
    connect_ms = cur_ms;
    if (WiFi.status() == WL_CONNECTED)  {
      if (mqttClient.connected()) {
        Serial.println("MQTT connection ok");
      } 
      else {
        Serial.println("MQTT connection failed");
        restartFlag = true;
      }
    }
    else { 
      Serial.println("WiFi connection failed");
      restartFlag = true;
    }
  }
}

void loop()
{
  ArduinoOTA.handle();
  if (!OTAupdate) {
    mqttClient.loop();
    check_status();
    check_connection();
    check_reset();
  }
}

