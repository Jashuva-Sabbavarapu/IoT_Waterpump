#include <WiFi.h>
#include <EEPROM.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Accespoint SSID and PASSWORD
#define WIFI_SSID "********"
#define WIFI_PASSWORD "*******"

// Firebase API and URL
#define API_KEY "your_firebase_realtime_database_api_key"
#define DATABASE_URL "your_firebase_realtime_database_url"

#define TRIGGER_PIN 16
#define ECHO_PIN 17
#define PUMP 18
#define LED 19
#define WIFI_LED 21

uint8_t waterlevel = 0;
uint8_t pumpstatus = 0;
uint8_t pumpstate = 0;
uint8_t mode = 1;
uint8_t levelLow = 0;
uint8_t levelHigh = 0;
uint32_t currentTime = 0;
uint8_t reconnectCount = 0;

bool wifistatus = false;
bool mode_change_manually = false;

FirebaseData fbdo_waterlevel, fbdo_pumpstatus, fbdo_pump, fbdo_mode, fbdo_min, fbdo_max, fbdo_modestatus;
FirebaseAuth auth;
FirebaseConfig config;

bool signup = false;

TaskHandle_t MeasureWaterLevel, ControlPump, WifiConnect, WriteFirebaseDB, ReadFirebaseDB;

void MeasureWaterLevel(void* parameter) {
  while (true) {
    uint32_t duration = 0;
    float distance = 0;
    uint8_t level = 0;
    if (fbdo_min.streamAvailable() && fbdo_max.streamAvailable()) {
      levelLow = EEPROM.read(0);
      levelHigh = EEPROM.read(1);
    }

    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    duration = pulseIn(ECHO_PIN, HIGH);
    distance = duration * 0.017;

    level = ((distance - levelLow) / (levelHigh - levelLow)) * 100;

    if (level < 0)
      level = 0;

    if (level > 100)
      level = 100;

    if (level != waterlevel)
      waterlevel = level;

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void ControlPump(void* parameter) {
  while (true) {
    if (mode) {
      if (waterlevel == 100) {
        pumpControl(0);
      } 
      else if (waterlevel == 0) {
        pumpControl(1);
      }
    } 
    else if (!mode) {
      pumpControl(pumpstate);
    }

    if (!mode && waterlevel >= 95) {
      mode = 1;
      mode_change_manually = true;
    }
  }
}

void WifiConnect(void* parameter) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(10000));
    } 
    else if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(WIFI_LED, HIGH);
      if (reconnectCount < 3) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (WiFi.status() != WL_CONNECTED) {
          reconnectCount++;
        }
        else if(WiFi.status() == WL_CONNECTED) {
          digitalWrite(WIFI_LED, LOW);
          reconnectCount = 0;
        }
        if (reconnectCount == 3) {
          for(uint8_t i = 0; i <= 6; i++)
          {
            digitalWrite(WIFI_LED, !digitalRead(WIFI_LED));
          }
          ESP.restart();
        }
      }
    }
  }
}

void WriteFirebaseDB(void* parameter) {
  while (true) {
    uint8_t tempLevel = 0;
    uint8_t tempPump = 0;
    if (Firebase.ready() && signup && (millis() - currentTime > 1000 || currentTime == 0)) {
      currentTime = millis();

      // Sending the waterlevel
      if (tempLevel != waterlevel) {
        tempLevel = waterlevel;
        Firebase.RTDB.setInt(&fbdo_waterlevel, "IoT_Waterpump/waterLevel", waterlevel);
      }

      // Send the pump status
      if (mode) {
        if (tempPump != pumpstatus) {
          tempPump = pumpstatus;
          Firebase.RTDB.setInt(&fbdo_pumpstatus, "IoT_Waterpump/pumpstatus", tempPump);
        }
      }

      if (mode_change_manually) {
        Firebase.RTDB.setInt(&fbdo_modestatus, "IoT_Waterpump/modestatus", 1);
        mode_change_manually = false;
      }
    }
  }
}

void ReadFirebaseDB(void* parameter) {
  while (true) {
    uint8_t levelLow = 0;
    uint8_t levelHigh = 0;
    if (Firebase.ready() && signup) {
      // Fetch the data from JSON "IoT_Waterpump/mode"
      Firebase.RTDB.readStream(&fbdo_mode);

      if (fbdo_mode.streamAvailable()) {
        if (fbdo_mode.dataType() == "int") {
          mode = fbdo_mode.intData();
        }
      }

      // Fetch the data from JSON "IoT_Waterpump/pump"
      if (!mode) {
        Firebase.RTDB.readStream(&fbdo_pump);

        if (fbdo_pump.streamAvailable()) {
          if (fbdo_pump.dataType() == "int") {
            pumpstate = fbdo_pump.intData();
          }
        }
      }

      // Fetch the low level from JSON "IoT_Waterpump/min"
      Firebase.RTDB.readStream(&fbdo_min);

      if (fbdo_min.streamAvailable()) {
        if (fbdo_min.dataType() == "int") {
          levelLow = fbdo_min.intData();
          if (levelLow != EEPROM.read(0)) {
            EEPROM.write(0, levelLow);
            EEPROM.commit();
          }
        }
      }

      // Fetch the high level from JSON "IoT_Waterpump/max"
      Firebase.RTDB.readStream(&fbdo_max);

      if (fbdo_max.streamAvailable()) {
        if (fbdo_max.dataType() == "int") {
          levelHigh = fbdo_max.intData();
          if (levelHigh != EEPROM.read(1)) {
            EEPROM.write(1, levelHigh);
            EEPROM.commit();
          }
        }
      }
    }
  }
}

void setup() {
  //GPIO configuration
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PUMP, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(WIFI_LED, OUTPUT);

  // Setup FirebaseRB
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("SignUp OK!");
    signup = true;
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Enabling the stream
  Firebase.RTDB.beginStream(&fbdo_waterlevel, "IoT_Waterpump/waterLevel");
  Firebase.RTDB.beginStream(&fbdo_pumpstatus, "IoT_Waterpump/pumpstatus");
  Firebase.RTDB.beginStream(&fbdo_pump, "IoT_Waterpump/pump");
  Firebase.RTDB.beginStream(&fbdo_mode, "IoT_Waterpump/mode");
  Firebase.RTDB.beginStream(&fbdo_min, "IoT_Waterpump/min");
  Firebase.RTDB.beginStream(&fbdo_max, "IoT_Waterpump/max");
  Firebase.RTDB.beginStream(&fbdo_modestatus, "IoT_Waterpump/modestatus");

  // Flash begin
  EEPROM.begin(2);

  // Create tasks with adjusted stack sizes
  xTaskCreate(MeasureWaterLevel, "Measure water level", 1000, NULL, 1, NULL);
  xTaskCreate(ControlPump, "Control the waterpump", 1000, NULL, 2, NULL);
  xTaskCreate(WifiConnect, "WIFI connect", 1000, NULL, 3, NULL);
  xTaskCreate(WriteFirebaseDB, "Send to firebaseDB", 1000, NULL, 4, NULL);
  xTaskCreate(ReadFirebaseDB, "Receive from firebaseDB", 1500, NULL, 5, NULL);
}

void loop();

void pumpControl(uint8_t pumpstate) {
  if (pumpstate) {
    digitalWrite(PUMP, HIGH);
    digitalWrite(LED, LOW);
    pumpstatus = 1;
  } else if (!pumpstate) {
    digitalWrite(PUMP, LOW);
    digitalWrite(LED, HIGH);
    pumpstatus = 0;
  }
}
