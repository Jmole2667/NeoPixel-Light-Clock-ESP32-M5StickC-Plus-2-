#include <M5Unified.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
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
<head>
<title>NeoPixel Clock</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: Arial, sans-serif; background-color: #f0f0f0; margin: 20px; }
  .container { max-width: 600px; margin: auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
  h1, h2 { color: #333; }
  .status, .control-group { margin-bottom: 20px; }
  .status p, .timer { background: #eee; padding: 10px; border-radius: 4px; }
  .timer { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
  .timer.finished { background-color: #d0d0d0; text-decoration: line-through; }
  .timer-info { display: flex; align-items: center; }
  .color-box { width: 20px; height: 20px; border-radius: 4px; margin-right: 10px; }
  input[type=text], input[type=number], input[type=color], input[type=range] { width: calc(100% - 22px); padding: 10px; margin-top: 5px; border: 1px solid #ccc; border-radius: 4px; }
  button { background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; }
  button:hover { background-color: #0056b3; }
  .danger { background-color: #dc3545; }
  .danger:hover { background-color: #c82333; }
  .action-btn { margin-left: 5px; padding: 5px 10px; }
</style>
</head>
<body>
<div class="container">
  <h1>NeoPixel Clock Control</h1>

  <div class="status">
    <h2>Device Status</h2>
    <p>IP Address: <span id="ip">...</span></p>
    <p>Connection: <span id="connection">...</span></p>
    <p>Battery: <span id="battery">...</span>%</p>
  </div>

  <div class="control-group">
    <h2>Add Timer</h2>
    <form id="addTimerForm">
      <label for="duration">Duration (minutes):</label>
      <input type="number" id="duration" name="duration" min="1" required>
      <label for="color">Color:</label>
      <input type="color" id="color" name="color" value="#ff0000">
      <button type="submit">Add Timer</button>
    </form>
  </div>

  <div class="control-group">
    <h2>Timers</h2>
    <div id="timersList"></div>
  </div>

  <div class="control-group">
    <h2>Settings</h2>
    <label for="brightness">Brightness:</label>
    <input type="range" id="brightness" min="0" max="255" value="50">
    <button id="reboot" class="danger">Reboot Device</button>
  </div>
</div>

<script>
function fetchStatus() {
  fetch('/status')
    .then(response => response.json())
    .then(data => {
      document.getElementById('ip').textContent = data.ip;
      document.getElementById('connection').textContent = data.connection;
      document.getElementById('battery').textContent = data.battery;

      const timersList = document.getElementById('timersList');
      timersList.innerHTML = '';
      if (data.timers.length === 0) {
        timersList.innerHTML = '<p>No timers yet.</p>';
      } else {
        data.timers.forEach(t => {
          const timerDiv = document.createElement('div');
          timerDiv.className = 'timer' + (t.finished ? ' finished' : '');

          let remaining = new Date(t.remainingTime).toISOString().substr(11, 8);
          if (!t.active && !t.finished) remaining = "Paused";
          if (t.finished) remaining = "Finished";

          timerDiv.innerHTML = `
            <div class="timer-info">
              <div class="color-box" style="background-color:${t.color}"></div>
              <span>${remaining}</span>
            </div>
            <div>
              <button class="action-btn" onclick="toggleTimer(${t.id})" ${t.finished ? 'disabled' : ''}>${t.active ? 'Pause' : 'Resume'}</button>
              <button class="action-btn danger" onclick="deleteTimer(${t.id})">Delete</button>
            </div>
          `;
          timersList.appendChild(timerDiv);
        });
      }
    });
}

function addTimer(e) {
  e.preventDefault();
  const duration = document.getElementById('duration').value;
  const color = document.getElementById('color').value.substring(1); // remove #
  fetch('/addTimer', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: `duration=${duration}&color=${color}`
  }).then(fetchStatus);
}

function toggleTimer(id) {
  fetch('/toggleTimer', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: `id=${id}`
  }).then(fetchStatus);
}

function deleteTimer(id) {
  if(confirm('Are you sure you want to delete this timer?')) {
    fetch('/deleteTimer', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: `id=${id}`
    }).then(fetchStatus);
  }
}

function setBrightness(e) {
  fetch('/setBrightness', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: `value=${e.target.value}`
  });
}

function reboot() {
  if(confirm('Are you sure you want to reboot?')) {
    fetch('/reboot', { method: 'POST' });
  }
}

document.getElementById('addTimerForm').addEventListener('submit', addTimer);
document.getElementById('brightness').addEventListener('change', setBrightness);
document.getElementById('reboot').addEventListener('click', reboot);

setInterval(fetchStatus, 2000);
window.onload = fetchStatus;
</script>
</div>
</body>
</html>
)rawliteral";

// ======= BLE WiFi Credentials Callback =======
// BLE callback removed

void startCaptivePortal() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Setup Mode");
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("Connect to WiFi:");
  M5.Lcd.println("'NeoPixel-Clock-Setup'");

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
    M5.Lcd.println("Use AP");
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

// ----------- LED Animations ------------
// Helper for rainbow animations
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void rainbowCycle(uint8_t wait) {
  uint16_t i, j;
  for(j=0; j<256; j++) { // single cycle
    for(i=0; i< strip.numPixels(); i++) {
      strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + j) & 255));
    }
    strip.show();
    delay(wait);
  }
}

// ----------- Factory Reset Support ------------
unsigned long buttonPressStart = 0;
bool resetting = false;

void checkFactoryReset() {
  if (M5.BtnA.isPressed()) {
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
  rainbowCycle(5); // Boot animation
  strip.clear();
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
    bool yellow_on = true;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
      strip.clear();
      if (yellow_on) {
        strip.fill(strip.Color(50, 50, 0)); // Dim Yellow
      }
      strip.show();
      yellow_on = !yellow_on;
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

      strip.fill(strip.Color(0, 80, 0)); // Green
      strip.show();
      delay(2000);
      strip.clear();
      strip.show();

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

    // Blinking blue light for setup mode
    long time = millis();
    strip.clear();
    if((time / 500) % 2 == 0) {
      strip.setPixelColor(0, strip.Color(0, 0, 80)); // Blue
    }
    strip.show();
  } else { // WiFi is connected
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
  }

  delay(100);
}
