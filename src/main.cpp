#include <M5Unified.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>

// ========== Settings ===========
#define NEOPIXEL_PIN 26
#define NUM_LEDS 60
Adafruit_NeoPixel strip(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

Preferences preferences;
AsyncWebServer server(80);
DNSServer dnsServer;

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define WIFI_CREDS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define IP_ADDRESS_CHAR_UUID "cba1d466-344c-4be3-ab3f-189f80dd7518"

BLECharacteristic* pIpChar = nullptr;
bool bleAdvertising = false;

int brightness = 50;
int battery_level = 0;

unsigned long last_screen_update = 0;
unsigned long last_battery_warning = 0;
unsigned long last_ntp_sync = 0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 3600;

struct Timer {
  int id;
  unsigned long endTime;
  unsigned long duration;
  unsigned long remainingTime;
  uint32_t color;
  bool active;
  bool finished;
};
std::vector<Timer> timers;
int nextTimerId = 0;

// ======= HTML page (same as your version) =======
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<!-- your full HTML here -->
<!-- For brevity, keep your existing index_html unchanged -->
</html>
)rawliteral";

// ======= BLE WiFi Credentials Callback =======
class WiFiCredsCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    std::string val = pCharacteristic->getValue();
    if (val.length() > 0) {
      int sep = val.find(';');
      if (sep != std::string::npos) {
        std::string ssid = val.substr(0, sep);
        std::string password = val.substr(sep + 1);

        preferences.putString("ssid", ssid.c_str());
        preferences.putString("password", password.c_str());
        preferences.end();

        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.setTextSize(2);
        M5.Lcd.println("WiFi Saved!");
        M5.Lcd.println("Restarting...");
        delay(2000);
        ESP.restart();
      }
    }
  }
};

void startBLE() {
  BLEDevice::init("NeoPixel Clock");
  BLEServer* pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* pWiFiCredsChar = pService->createCharacteristic(WIFI_CREDS_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pWiFiCredsChar->setCallbacks(new WiFiCredsCallbacks());

  pIpChar = pService->createCharacteristic(IP_ADDRESS_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pIpChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleAdvertising = true;
}

void startCaptivePortal() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Setup Mode");
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("Connect to WiFi:");
  M5.Lcd.println("'NeoPixel-Clock-Setup'");
  M5.Lcd.println("Or use BLE ('NeoPixel Clock')");

  startBLE();

  WiFi.softAP("NeoPixel-Clock-Setup");
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "<html><body><h1>WiFi Setup</h1><form action='/save' method='POST'>SSID: <input type='text' name='ssid'><br>Password: <input type='password' name='password'><br><input type='submit' value='Save'></form></body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* request) {
    String ssid = request->arg("ssid");
    String pass = request->arg("password");
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    preferences.end();

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextSize(2);
    M5.Lcd.println("WiFi Saved!");
    M5.Lcd.println("Restarting...");
    delay(2000);
    ESP.restart();
  });

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->redirect("http://" + WiFi.softAPIP().toString());
  });

  server.begin();
}

void startWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(1024);
    doc["ip"] = WiFi.localIP().toString();
    doc["battery"] = battery_level;
    doc["connection"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
    doc["ble"] = bleAdvertising ? "On" : "Off";

    JsonArray arr = doc.createNestedArray("timers");
    for (auto& t : timers) {
      JsonObject o = arr.createNestedObject();
      o["id"] = t.id;
      char hexColor[8];
      sprintf(hexColor, "#%02x%02x%02x", (t.color >> 16) & 0xFF, (t.color >> 8) & 0xFF, t.color & 0xFF);
      o["color"] = hexColor;
      o["active"] = t.active;
      o["finished"] = t.finished;
      if (t.active) {
        t.remainingTime = t.endTime - millis();
        if ((long)t.remainingTime < 0) t.remainingTime = 0;
      }
      o["remainingTime"] = t.remainingTime;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/addTimer", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("duration", true) && request->hasParam("color", true)) {
      unsigned long dur = request->getParam("duration", true)->value().toInt() * 60000UL;
      String hexColor = request->getParam("color", true)->value();
      uint32_t cval = (uint32_t)strtol(hexColor.c_str(), NULL, 16);
      uint32_t color = strip.Color((cval >> 16) & 0xFF, (cval >> 8) & 0xFF, cval & 0xFF);
      timers.push_back({nextTimerId++, millis() + dur, dur, dur, color, true, false});
    }
    request->send(200);
  });

  server.on("/toggleTimer", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("id", true)) {
      int id = request->getParam("id", true)->value().toInt();
      for (auto& t : timers) {
        if (t.id == id && !t.finished) {
          t.active = !t.active;
          if (t.active) {
            t.endTime = millis() + t.remainingTime;
          } else {
            t.remainingTime = t.endTime - millis();
          }
        }
      }
    }
    request->send(200);
  });

  server.on("/deleteTimer", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("id", true)) {
      int id = request->getParam("id", true)->value().toInt();
      timers.erase(std::remove_if(timers.begin(), timers.end(), [id](const Timer& t) { return t.id == id; }), timers.end());
    }
    request->send(200);
  });

  server.on("/setBrightness", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("value", true)) {
      brightness = request->getParam("value", true)->value().toInt();
      strip.setBrightness(brightness);
    }
    request->send(200);
  });

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });

  server.begin();
}

void updateScreen() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(2);

  if (WiFi.status() == WL_CONNECTED) {
    M5.Lcd.println("WiFi Connected!");
    M5.Lcd.println(WiFi.localIP());
  } else {
    M5.Lcd.println("Setup Mode");
    M5.Lcd.println("Use AP or BLE");
  }
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(0, 50);
  M5.Lcd.printf("Battery: %d%%\n", battery_level);
  M5.Lcd.printf("FW v1.1\n");
}

void lowBatteryWarning(int pulses) {
  uint8_t orig = strip.getBrightness();
  strip.setBrightness(30);
  for (int i = 0; i < pulses; i++) {
    strip.fill(strip.Color(255, 0, 0));
    strip.show();
    delay(300);
    strip.clear();
    strip.show();
    delay(300);
  }
  strip.setBrightness(orig);
}

void updateLeds() {
  strip.clear();

  // Draw timers arcs
  for (auto const& t : timers) {
    if (t.active) {
      float progress = (float)(t.endTime - millis()) / t.duration;
      if (progress < 0) progress = 0;
      int ledsToShow = (int)(progress * NUM_LEDS);
      for (int i = 0; i < ledsToShow; i++) {
        int ledIndex = (45 - i + NUM_LEDS) % NUM_LEDS;
        ledIndex = (ledIndex + 30) % NUM_LEDS;  // 180 deg rotation
        strip.setPixelColor(ledIndex, t.color);
      }
    }
  }

  // Draw clock hands (simple colors)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int hourLed = (timeinfo.tm_hour % 12) * 5 + (timeinfo.tm_min / 12);
    hourLed = (hourLed + 30) % NUM_LEDS;
    strip.setPixelColor(hourLed, strip.Color(255, 0, 0)); // red

    int minuteLed = (timeinfo.tm_min + 30) % NUM_LEDS;
    strip.setPixelColor(minuteLed, strip.Color(0, 255, 0)); // green

    int secondLed = (timeinfo.tm_sec + 30) % NUM_LEDS;
    strip.setPixelColor(secondLed, strip.Color(0, 0, 255)); // blue
  }

  strip.setBrightness(brightness);
  strip.show();
}

void updateTimers() {
  for (auto& t : timers) {
    if (t.active && !t.finished && millis() >= t.endTime) {
      t.finished = true;
      t.active = false;
      // Flash color 5 times when timer finishes
      uint8_t orig = strip.getBrightness();
      strip.setBrightness(255);
      for (int i = 0; i < 5; i++) {
        strip.fill(t.color);
        strip.show();
        delay(200);
        strip.clear();
        strip.show();
        delay(200);
      }
      strip.setBrightness(orig);
    }
  }
}

// ----------- Factory Reset Support ------------
unsigned long buttonPressStart = 0;
bool resetting = false;

void checkFactoryReset() {
  if (M5.BtnP.isPressed()) {
    if (buttonPressStart == 0) {
      buttonPressStart = millis();
    } else {
      if (millis() - buttonPressStart >= 5000 && !resetting) {
        resetting = true;
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.setTextSize(2);
        M5.Lcd.println("Factory Reset");
        M5.Lcd.println("Clearing WiFi...");
        preferences.clear();
        preferences.end();
        delay(2000);
        ESP.restart();
      }
    }
  } else {
    buttonPressStart = 0;  // reset timer if button released early
  }
}

void setup() {
  M5.begin();
  M5.Power.setLed(false);  // Disable yellow LED

  strip.begin();
  strip.setBrightness(brightness);
  strip.show();

  M5.Lcd.setRotation(1);

  preferences.begin("wifi-creds", false);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");

  if (ssid == "" || password == "") {
    startCaptivePortal();
  } else {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextSize(2);
    M5.Lcd.printf("Connecting to\n%s\n", ssid.c_str());

    WiFi.begin(ssid.c_str(), password.c_str());
    int timeout = 20;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
      delay(500);
      M5.Lcd.print(".");
      timeout--;
    }

    if (WiFi.status() != WL_CONNECTED) {
      startCaptivePortal();
    } else {
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextSize(2);
      M5.Lcd.println("WiFi Connected!");
      M5.Lcd.println(WiFi.localIP());

      if (bleAdvertising) {
        BLEDevice::getAdvertising()->stop();
        bleAdvertising = false;
      }

      pIpChar = nullptr;  // reset BLE IP char pointer to avoid stale data

      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      last_ntp_sync = millis();

      startWebServer();
    }
  }
}

void loop() {
  M5.update();

  checkFactoryReset();

  if (WiFi.status() != WL_CONNECTED) {
    dnsServer.processNextRequest();
  }

  battery_level = M5.Power.getBatteryLevel();

  if (battery_level <= 5 && (millis() - last_battery_warning) > 300000) {
    lowBatteryWarning(battery_level);
    last_battery_warning = millis();
  }

  if ((millis() - last_screen_update) > 5000) {
    updateScreen();
    last_screen_update = millis();
  }

  if ((millis() - last_ntp_sync) > 3600000) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    last_ntp_sync = millis();
  }

  updateTimers();
  updateLeds();

  delay(100);
}
