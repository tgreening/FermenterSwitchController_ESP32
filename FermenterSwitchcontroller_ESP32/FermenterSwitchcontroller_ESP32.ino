// Import required libraries
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <Wire.h>
#include "Ticker.h"
#include <CircularBuffer.h>
#include <FS.h>
//#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <WiFiUdp.h>
//#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include "SPIFFS.h"
#include <FS.h>           //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>  //https://github.com/bblanchon/ArduinoJson
#define OLED_RESET 4
#include <time.h>
#include "ThingSpeak.h"
#include <vector>
#include "KasaSmartPlug.h"

// WiFi parameters
const char* thingSpeakserver = "api.thingspeak.com";
const char* ssid = "Spartans_2.4";
const char* pword = "G3orgina";
const long READ_MILLIS = 5000UL;
const int NEXT_POST = 300;
const int NEXT_TEMP_CHECK = 30;
const int NEXT_SAFETY_CHECK = 10 * 60;  //adding 10 minutes to next next check to give cooling unit time to warm up
const float SAFETY_LOW_TEMP = 32.5f;
unsigned long NEXT_SLOW_CHANGE = 0UL;
const char* host = "fermenterswitch";
const int SAFETY = 3;
const int COOLING = 2;
const int HEATING = 1;
const int OFF = 0;
const int tsChannel = 453061;
const char* apiKey = "G0VUS3DTWMJXGK10";
const char* HEAT_PLUG_NAME = "FermenterHeat";

const char* COOL_PLUG_NAME = "FermenterCool";
//Adafruit_SSD1306 display(OLED_RESET);

float tolerance = 1.0F;
float finalTemperature, currentStep = 63.0;
int controllerMode = 0;  //1 - heating, 2 - cooling, 3 - safety
unsigned long nextSlowTemperatureChange;
float fermenterTemp, chamberReading, temperatureChange, slowIntermediateTemp, currentReading = 0.0F;

int HighMillis, Rollover = 0;
bool isHeatEnabled, isCoolingEnabled = true;
int slowFlag, slowHours = 0;
float slowDegrees = 0.0f;

//flag for saving data
bool shouldSaveConfig = false;

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(17);
OneWire oneWire2(16);

DallasTemperature fermenterSensor(&oneWire);
DallasTemperature chamberSensor(&oneWire2);
WiFiClient http;
WebServer httpServer(80);
DynamicJsonDocument json(350);
bool checkTemp = false;
bool postData = false;
Ticker tempTicker;    //(setRead, NEXT_TEMP_CHECK, 0, MILLIS);
Ticker postTicker;    //(setPost, POST_MILLIS, 0, MILLIS);
Ticker safetyTicker;  //(setSafety, SAFETY_NEXT_CHECK, 1, MILLIS);
Ticker slowTicker;
std::vector<float> slowChange = {};

KASASmartPlug* heatPlug;
KASASmartPlug* coolPlug;
KASAUtil kasaUtil;

//callback notifying us of the need to save config
void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup(void) {
  SPIFFS.begin(true);
  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body><h1>Current Status</h1><br>Fermenter temperature: ";
    html += currentReading;
    html += "<br>Chamber temperature: ";
    html += chamberReading;
    html += "<br>Temperature Change: ";
    html += temperatureChange;
    html += "<br>Mode: ";
    if (controllerMode == OFF) {
      html += "Off<br>";
    }
    if (controllerMode == HEATING) {
      html += "Heating<br>";
    }
    if (controllerMode == COOLING) {
      html += "Cooling<br>";
    }
    if (controllerMode == SAFETY) {
      html += "Protecting<br>";
    }

    html += "<br>Uptime: ";
    html += uptimeString(millis());


    html += "<br>";
    html += "<br><h1>Update</h1><br><form method=\"GET\" action=\"update\">Desired temperature:<input name=\"finalTemperature\" type=\"text\" maxlength=\"5\" size=\"5\" value=\"" + String(finalTemperature) + "\" />";
    html += "<br>Tolerance: <input name=\"tolerance\" type=\"text\" maxlength=\"5\" size=\"5\" value=\"" + String(tolerance) + "\" />";
    html += "<br>Cooling: <input name=\"cooling\" type=\"radio\" value=\"1\" ";
    if (isCoolingEnabled) {
      html += " checked";
    }
    html += ">On<input name=\"cooling\" type=\"radio\" value=\"0\" ";
    if (!isCoolingEnabled) {
      html += " checked";
    }
    html += ">Off<br>";
    html += "<br>Heating: <input name=\"heating\" type=\"radio\" value=\"1\" ";
    if (isHeatEnabled) {
      html += " checked";
    }
    html += ">On<input name=\"heating\" type=\"radio\" value=\"0\" ";
    if (!isHeatEnabled) {
      html += " checked";
    }
    html += ">Off<br>";
    html += "Slow Change: <input name=\"slowChange\" type=\"radio\" value=\"1\" ";
    if (slowFlag) {
      html += " checked";
    }
    html += ">On<input name=\"slowChange\" type=\"radio\" value=\"0\" ";
    if (!slowFlag) {
      html += " checked";
    };
    html += ">Off<br>";
    html += "Current Goal: " + String(currentStep) + "<br>";
    html += "Slow Change Degrees:<input name=\"slowChangeDegrees\" type=\"text\" maxlength=\"4\" size=\"4\" value=\"" + String(slowDegrees) + "\" /><br>";
    html += "Slow Change Hours:<input name=\"slowChangeHours\" type=\"text\" maxlength=\"4\" size=\"4\" value=\"" + String(slowHours) + "\" /><br>";

    html += "<br><INPUT type=\"submit\" value=\"Send\"> <INPUT type=\"reset\"></form>";
    html += "<br></body></html>";
    Serial.print("Done serving up HTML...");
    Serial.println(html);
    httpServer.send(200, "text/html", html);
  });
  httpServer.on("/update", HTTP_GET, []() {
    if (httpServer.arg("finalTemperature") != "") {
      finalTemperature = httpServer.arg("finalTemperature").toFloat();
    }
    if (httpServer.arg("tolerance") != "") {
      tolerance = httpServer.arg("tolerance").toFloat();
    }
    if (httpServer.arg("cooling") != "") {
      isCoolingEnabled = httpServer.arg("cooling").toInt();
    }
    if (httpServer.arg("heating") != "") {
      isHeatEnabled = httpServer.arg("heating").toInt();
    }
    if (httpServer.arg("slowChange") != "") {
      slowFlag = httpServer.arg("slowChange").toInt();
    }
    if (httpServer.arg("slowChangeDegrees") != "") {
      slowDegrees = httpServer.arg("slowChangeDegrees").toFloat();
    }
    if (httpServer.arg("slowChangeHours") != "") {
      slowHours = httpServer.arg("slowChangeHours").toInt();
    }

    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access - Control - Allow - Origin", "*");
    writeSettingsFile();
    String html = "<!DOCTYPE html>";
    html += "<html>";
    html += "  <head>";
    html += "      <title>HTML Meta Tag</title>";
    html += "      <meta http-equiv = \"refresh\" content = \"10; url = \\\" />";
    html += "   </head>";
    html += "   <body>";
    html += "      <p><h3>Update done!</h3><br><br> Redirecting....</p>";
    html += "   </body>";
    html += "</html>";
    httpServer.send(200, "text/html", html);
    if (slowFlag == 1) {
      updateTemperatures();
      float totalTempChange = finalTemperature - currentReading;
      float iterations = (int)(abs(totalTempChange) / slowDegrees) + 1;
      Serial.print("Iterations: ");
      Serial.println(iterations);
      if (totalTempChange > 0) {
        currentStep = currentReading + slowDegrees;
      } else {
        currentStep = currentReading - slowDegrees;
      }
      float nextStep = currentStep;

      for (int i = 2; i < iterations + 1; i++) {
        if (totalTempChange > 0) {
          nextStep += slowDegrees;
          if (nextStep > finalTemperature) {
            nextStep = finalTemperature;
          }
        } else {
          nextStep -= slowDegrees;
          if (nextStep < finalTemperature) {
            nextStep = finalTemperature;
          }
        }
        //Serial.println(nextStep);
        slowChange.push_back(nextStep);
      }
      //Serial.print("Slow change array size: ");
      //Serial.println(slowChange.size());
      //Serial.println(slowHours * 60);
      slowTicker.attach(slowHours * 3600, nextChange);
    } else {
      currentStep = finalTemperature;
    }
  });
  // Start Serial
  Serial.begin(115200);
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        DeserializationError error = deserializeJson(json, configFile);

        // Test if parsing succeeds.
        if (error) {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.c_str());
          return;
        }

        Serial.println("\nparsed json");
        if (json["tolerance"])
          tolerance = json["tolerance"];
        if (json["finalTemperature"])
          finalTemperature = json["finalTemperature"];
        if (json["SlowChangeFlag"])
          slowFlag = json["SlowChangeFlag"];
        if (json["SlowChangeDegrees"])
          slowDegrees = json["SlowChangeDegrees"];
        if (json["SlowChangeHours"])
          slowHours = json["SlowChangeHours"];
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  connectWifi();
  // Print the IP address
  Serial.println(WiFi.localIP());
  WiFi.mode(WIFI_STA);
  //have to do this to initialize the devices
  kasaUtil.ScanDevices();
  heatPlug = kasaUtil.GetSmartPlug(HEAT_PLUG_NAME);
  coolPlug = kasaUtil.GetSmartPlug(COOL_PLUG_NAME);
  turnOff();
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else  // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  fermenterSensor.begin();
  chamberSensor.begin();

  httpServer.begin();
  updateTemperatures();
  fermenterTemp = currentReading;
  turnOff();
  MDNS.begin(host);
  tempTicker.attach(NEXT_TEMP_CHECK, setRead);
  postTicker.attach(NEXT_POST, setPost);
  slowIntermediateTemp = fermenterTemp;
}

void setRead() {
  checkTemp = true;
}

void nextChange() {
  Serial.println("In step change...");
  Serial.print("Array size...");
  Serial.println(slowChange.size());
  if (slowChange.size() > 0) {
    currentStep = slowChange.at(0);
    slowChange.erase(slowChange.begin());
    Serial.println(slowChange.size());
    if (slowChange.size() == 0) {
      slowTicker.detach();
    }
  }
}
void setPost() {
  postData = true;
}

void setSafety() {
  if (SAFETY_LOW_TEMP < chamberReading) {
    controllerMode = COOLING;
    safetyTicker.detach();
  }
}

void loop() {
  if (checkTemp && controllerMode != SAFETY) {
    int retryCount = 0;
    updateTemperatures();
    checkTemp = false;
    if (SAFETY_LOW_TEMP > chamberReading) {
      turnOff();
      Serial.println("In safety mode");
      controllerMode = SAFETY;
      safetyTicker.attach(NEXT_SAFETY_CHECK, setSafety);
    } else {
      temperatureChange = currentReading - fermenterTemp;
      fermenterTemp = currentReading;

      float currentGoal = 0.0f;
      if (slowFlag == 1) {
        currentGoal = currentStep;
      } else {
        currentGoal = finalTemperature;
      }

      if (controllerMode == OFF) {
        if (currentReading > currentGoal + tolerance && isCoolingEnabled) {
          turnOnCooling();
        } else if (currentGoal > currentReading + tolerance && isHeatEnabled) {
          turnOnHeat();
        } else {
          turnOff();
        }
      } else {
        Serial.println("Already doing something check on status");
        int multiplier = 1;
        if (controllerMode == COOLING) {
          Serial.println("Too hot");
          if (currentReading > chamberReading) {
            multiplier += (currentReading - chamberReading) / 6;
          }
          if ((currentReading + (temperatureChange * multiplier)) < currentGoal || currentReading < currentGoal || !isCoolingEnabled) {
            Serial.println("Will get cool in a few minutes");
            turnOff();
          }
        } else if (controllerMode == HEATING) {
          Serial.println("Too cold");
          if (chamberReading > currentReading) {
            multiplier += (chamberReading - currentReading) / 3;
          }
          if ((currentReading + (temperatureChange * multiplier)) > currentGoal || currentReading > currentGoal || !isHeatEnabled) {
            Serial.println("Will get hot in a few minutes");
            turnOff();
          }
        }
      }
    }
  }

  if (postData) {
    Serial.println("Posting....");
    if ((WiFi.status() != WL_CONNECTED)) {
      delay(1000);
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.reconnect();
    }
    float currentGoal = 0.0f;
    if (slowFlag == 1) {
      currentGoal = currentStep;
    } else {
      currentGoal = finalTemperature;
    }

    postReadingData(currentReading, chamberReading, currentGoal, temperatureChange, tolerance);
    yield();
    postData = false;
  }

  uptime();
  httpServer.handleClient();
  ArduinoOTA.handle();
}

float getReading(DallasTemperature sensor) {
  int retryCount = 0;
  float firstReading = sensor.getTempFByIndex(0);
  //always good to wait between readings
  delay(500);
  //Get second reading to ensure that we don't have an anomaly
  float secondReading = sensor.getTempFByIndex(0);
  //If the two readings are more than a degree celsius different - retake both
  while (((firstReading - secondReading) > 1.0F || (secondReading - firstReading) > 1.0F) && ((int)firstReading * 100) < 200 && retryCount < 10) {
    firstReading = sensor.getTempFByIndex(0);
    retryCount++;
    if (retryCount != 10) {
      delay(retryCount * 1000);
    }
    secondReading = sensor.getTempFByIndex(0);
  }
  //If after ten tries we're still off - restart
  if (retryCount == 10) {
    ESP.restart();
  }
  //secondReading = 61.23;
  return secondReading;
}

void turnOnHeat() {
  Serial.println("Turning heat on....");
  heatPlug->SetRelayState(1);
  delay(5000);
  coolPlug->SetRelayState(0);
  controllerMode = HEATING;
}

void turnOnCooling() {
  Serial.println("Turning cooling on....");
  heatPlug->SetRelayState(0);
  delay(5000);
  coolPlug->SetRelayState(1);
  controllerMode = COOLING;
}

void turnOff() {
  Serial.println("Turning everything off....");
  heatPlug->SetRelayState(0);
  delay(5000);
  coolPlug->SetRelayState(0);
  controllerMode = OFF;
}

void updateTemperatures() {
  fermenterSensor.requestTemperatures();  // Send the command to get temperatures
  chamberSensor.requestTemperatures();    // Send the command to get temperatures
  currentReading = getReading(fermenterSensor);
  chamberReading = getReading(chamberSensor);
  Serial.printf("Fermenter Temp: %f\nChamber Temp %f\n\r", currentReading, chamberReading);
}

void postReadingData(float fermenter, float chamber, int finalTemperature, float avgChange, float tolerance) {
  connectWifi();
  ThingSpeak.setField(1, fermenter);
  ThingSpeak.setField(2, chamber);
  ThingSpeak.setField(3, finalTemperature);
  ThingSpeak.setField(4, controllerMode);
  ThingSpeak.setField(5, avgChange);
  ThingSpeak.setField(6, tolerance);
  int x = ThingSpeak.writeFields(tsChannel, apiKey);
  Serial.println(x);
}


void writeSettingsFile() {
  //  Serial.printf("update file settings heap size: %u\n", ESP.getFreeHeap());
  if (SPIFFS.exists("/config.json")) {
    if (SPIFFS.exists("/config.old")) {
      SPIFFS.remove("/config.old");
    }
    SPIFFS.rename("/config.json", "/config.old");
    SPIFFS.remove("/config.json");
  }
  json["finalTemperature"] = finalTemperature;
  json["tolerance"] = tolerance;
  json["SlowChangeFlag"] = slowFlag;
  json["SlowChangeDegrees"] = slowDegrees;
  json["SlowChangeHours"] = slowHours;


  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
  }
  serializeJson(json, configFile);
  serializeJson(json, Serial);
  configFile.close();
  //end save
  yield();
}

void uptime() {
  //** Making Note of an expected rollover *****//
  if (millis() >= 3000000000) {
    HighMillis = 1;
  }
  //** Making note of actual rollover **//
  if (millis() <= 100000 && HighMillis == 1) {
    Rollover++;
    HighMillis = 0;
  }
}

String uptimeString(unsigned long timeElapsed) {
  long Day = 0;
  int Hour = 0;
  int Minute = 0;
  int Second = 0;
  long secsUp = timeElapsed / 1000;
  Second = secsUp % 60;
  Minute = (secsUp / 60) % 60;
  Hour = (secsUp / (60 * 60)) % 24;
  Day = (Rollover * 50) + (secsUp / (60 * 60 * 24));  //First portion takes care of a rollover [around 50 days]
  char buff[32];
  sprintf(buff, "%3d Days %02d:%02d:%02d", Day, Hour, Minute, Second);
  String retVal = String(buff);
  Serial.print("Uptime String: ");
  Serial.println(retVal);
  return retVal;
}


void connectWifi() {
  WiFi.hostname(String(host));

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pword);  // Connect to the network
    Serial.print("Connecting to ");
    Serial.print(ssid);
    Serial.println(" ...");

    int i = 1;
    while (WiFi.status() != WL_CONNECTED && i < 8) {  // Wait for the Wi-Fi to connect
      delay(1000 * i);
      Serial.print(++i);
      Serial.print(' ');
    }  //if you get here you have connected to the WiFi

    if (i >= 8) {
      ESP.restart();
    }
  }
  Serial.println("connected...yay :)");
}
