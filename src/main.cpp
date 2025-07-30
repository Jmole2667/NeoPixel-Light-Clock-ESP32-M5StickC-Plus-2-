#include <M5StickCPlus2.h>
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

// =================================================================
// Global Settings & Variables
// =================================================================

// NeoPixel settings
#define NEOPIXEL_PIN 26
#define NUM_LEDS 60
Adafruit_NeoPixel strip(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
int brightness = 50;

// Battery Monitoring
unsigned long last_screen_update = 0;
unsigned long last_battery_warning_time = 0;
int battery_level = 0;

// Timer Management
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

// NTP & Time
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 3600;
unsigned long last_ntp_sync = 0;

// Clock Hand Colors
uint32_t hourColor = strip.Color(255, 0, 0);
uint32_t minuteColor = strip.Color(0, 255, 0);
uint32_t secondColor = strip.Color(0, 0, 255);

// Web Server & DNS
AsyncWebServer server(80);
DNSServer dnsServer;

// Persistent Storage
Preferences preferences;

// Bluetooth Low Energy
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define WIFI_CREDS_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define IP_ADDRESS_CHARACTERISTIC_UUID "cba1d466-344c-4be3-ab3f-189f80dd7518"
BLECharacteristic *pIpAddressCharacteristic;
bool bleAdvertising = false;


// =================================================================
// Web Interface HTML
// =================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>NeoPixel Clock Control</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; background-color: #222; color: #eee; }
        .container { max-width: 600px; margin: 0 auto; padding: 20px; }
        .card { background-color: #333; padding: 15px; border-radius: 10px; margin-bottom: 20px; }
        h1, h2 { color: #00bcd4; }
        label { display: block; margin-bottom: 5px; }
        input, button { padding: 10px; margin-bottom: 10px; border-radius: 5px; border: 1px solid #555; background-color: #444; color: #eee; }
        input[type=color] { padding: 0; }
        input[type=range] { width: 100%; }
        button { width: 100%; cursor: pointer; background-color: #00bcd4; }
        .red-button { background-color: #f44336; }
        .green-button { background-color: #4CAF50; }
        .status-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .status-item { background-color: #444; padding: 10px; border-radius: 5px; }
        #timers-list .timer { display: flex; justify-content: space-between; align-items: center; padding: 10px; border-bottom: 1px solid #444; }
        #timers-list .timer:last-child { border-bottom: none; }
    </style>
</head>
<body>
    <div class="container">
        <h1>NeoPixel Clock Control</h1>
        <div class="card">
            <h2>Status</h2>
            <div class="status-grid">
                <div class="status-item"><strong>IP Address:</strong> <span id="ip"></span></div>
                <div class="status-item"><strong>Battery:</strong> <span id="battery"></span></div>
                <div class="status-item"><strong>Connection:</strong> <span id="connection"></span></div>
                <div class="status-item"><strong>Bluetooth:</strong> <span id="ble"></span></div>
            </div>
        </div>
        <div class="card">
            <h2>Controls</h2>
            <label for="brightness">Brightness</label>
            <input type="range" min="1" max="255" value="50" id="brightness">
            <br><br>
            <button id="reset-button" class="red-button">Reset Device</button>
        </div>
        <div class="card">
            <h2>Timers</h2>
            <div>
                <label for="timer-duration">Duration (minutes)</label>
                <input type="number" id="timer-duration" min="1" value="5" style="width: calc(100% - 22px);">
                <label for="timer-color">Color</label>
                <input type="color" id="timer-color" value="#ff00ff" style="width: calc(100% - 22px);">
                <button id="add-timer">Add Timer</button>
            </div>
            <div id="timers-list"></div>
        </div>
    </div>
    <script>
        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('ip').innerText = data.ip;
                    document.getElementById('battery').innerText = data.battery + '%';
                    document.getElementById('connection').innerText = data.connection;
                    document.getElementById('ble').innerText = data.ble;
                    updateTimersList(data.timers);
                });
        }

        function updateTimersList(timers) {
            const list = document.getElementById('timers-list');
            list.innerHTML = '';
            timers.forEach(timer => {
                const remaining = Math.ceil(timer.remainingTime / 1000);
                const div = document.createElement('div');
                div.className = 'timer';
                let statusText = 'Finished';
                if (timer.active) {
                    statusText = `Running - ${remaining}s left`;
                } else if (!timer.finished) {
                    statusText = `Paused - ${remaining}s left`;
                }

                div.innerHTML = `
                    <span style="color:${timer.color};">&#9632;</span>
                    <span>${statusText}</span>
                    <div>
                        <button class="${timer.active ? '' : 'green-button'}" onclick="toggleTimer(${timer.id})" ${timer.finished ? 'disabled' : ''}>${timer.active ? 'Pause' : 'Resume'}</button>
                        <button class="red-button" onclick="deleteTimer(${timer.id})">Delete</button>
                    </div>
                `;
                list.appendChild(div);
            });
        }

        document.getElementById('add-timer').addEventListener('click', () => {
            const duration = document.getElementById('timer-duration').value;
            const color = document.getElementById('timer-color').value;
            fetch('/addTimer', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: `duration=${duration}&color=${color.substring(1)}`
            }).then(updateStatus);
        });

        function toggleTimer(id) {
            fetch('/toggleTimer', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'id=' + id
            }).then(updateStatus);
        }

        function deleteTimer(id) {
            fetch('/deleteTimer', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'id=' + id
            }).then(updateStatus);
        }

        document.getElementById('brightness').addEventListener('change', (event) => {
            fetch('/setBrightness', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'value=' + event.target.value
            });
        });

        document.getElementById('reset-button').addEventListener('click', () => {
            if (confirm('Are you sure?')) { fetch('/reboot', { method: 'POST' }); }
        });

        setInterval(updateStatus, 2000);
        window.onload = updateStatus;
    </script>
</body>
</html>
)rawliteral";

// =================================================================
// Function Prototypes
// =================================================================
void updateLeds();
void startCaptivePortal();
void startWebServer();
void pulseBlue(int duration_ms);
void pulseGreen(int duration_ms);
void startBLE();
void updateTimers();
void flashColor(uint32_t color, int flashes, int flash_delay);
void lowBatteryWarning(int pulses);
void updateScreen();


// =================================================================
// BLE Callback Class
// =================================================================
class WiFiCredsCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            int separator = value.find(';');
            if (separator != std::string::npos) {
                std::string ssid = value.substr(0, separator);
                std::string password = value.substr(separator + 1);
                preferences.putString("ssid", ssid.c_str());
                preferences.putString("password", password.c_str());
                preferences.end();
                ESP.restart();
            }
        }
    }
};

// =================================================================
// Main Setup & Loop
// =================================================================

void setup() {
    M5.begin();
    M5.Axp.SetLed(false); // Disable the built-in yellow LED

    strip.begin();
    strip.setBrightness(brightness);
    strip.show();

    M5.Lcd.setRotation(1);
    M5.Lcd.setTextSize(1);

    preferences.begin("wifi-creds", false);
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");

    if (ssid == "" || password == "") {
        startCaptivePortal();
    } else {
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.println("Connecting to " + ssid);
        WiFi.begin(ssid.c_str(), password.c_str());
        int connect_timeout = 20;
        while (WiFi.status() != WL_CONNECTED && connect_timeout > 0) {
            pulseBlue(250);
            delay(250);
            connect_timeout--;
        }

        if (WiFi.status() != WL_CONNECTED) {
            startCaptivePortal();
        } else {
            pulseGreen(1000);
            if (bleAdvertising) {
                BLEDevice::getAdvertising()->stop();
                bleAdvertising = false;
            }
            if(pIpAddressCharacteristic) {
                pIpAddressCharacteristic->setValue(WiFi.localIP().toString().c_str());
                pIpAddressCharacteristic->notify();
            }
            startWebServer();
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
            last_ntp_sync = millis();
        }
    }
}

void loop() {
    // Handle captive portal DNS requests if in setup mode
    if (WiFi.status() != WL_CONNECTED) {
        dnsServer.processNextRequest();
    }

    // Check battery level
    battery_level = constrain((int)map(M5.Axp.GetBatVoltage() * 1000, 3200, 4200, 0, 100), 0, 100);

    // Show low battery warning every 5 minutes if critical
    if (battery_level <= 5 && millis() - last_battery_warning_time > 300000) {
        lowBatteryWarning(battery_level);
        last_battery_warning_time = millis();
    }

    // Update the LCD screen every 5 seconds
    if(millis() - last_screen_update > 5000) {
        updateScreen();
        last_screen_update = millis();
    }

    // Resync NTP time every hour
    if (millis() - last_ntp_sync > 3600000) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        last_ntp_sync = millis();
    }

    updateTimers();
    updateLeds();
    delay(100);
}

// =================================================================
// Display & Animation Functions
// =================================================================

void updateScreen() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextSize(2);
    if(WiFi.status() == WL_CONNECTED) {
        M5.Lcd.println(WiFi.localIP());
    } else {
        M5.Lcd.println("Setup Mode");
    }
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(200, 0);
    M5.Lcd.printf("Batt:%d%%", battery_level);
}

void lowBatteryWarning(int pulses) {
    uint8_t original_brightness = strip.getBrightness();
    strip.setBrightness(30);
    for (int i = 0; i < pulses; i++) {
        strip.fill(strip.Color(255, 0, 0));
        strip.show();
        delay(300);
        strip.clear();
        strip.show();
        delay(300);
    }
    strip.setBrightness(original_brightness);
}

void updateLeds() {
    strip.clear();

    // Draw timers first as shrinking arcs
    for (auto const& timer : timers) {
        if (timer.active) {
            float progress = (float)(timer.endTime - millis()) / timer.duration;
            if (progress < 0) progress = 0;
            int ledsToShow = (int)(progress * NUM_LEDS);

            for (int i = 0; i < ledsToShow; i++) {
                int ledIndex = (45 - i + NUM_LEDS) % NUM_LEDS; // Start at 12 o'clock and go counter-clockwise
                ledIndex = (ledIndex + 30) % NUM_LEDS; // Apply 180-degree rotation
                strip.setPixelColor(ledIndex, timer.color);
            }
        }
    }

    // Draw clock hands over timers
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        int hourLed = (timeinfo.tm_hour % 12) * 5 + (timeinfo.tm_min / 12);
        hourLed = (hourLed + 30) % 60;
        strip.setPixelColor(hourLed, hourColor);

        int minuteLed = timeinfo.tm_min;
        minuteLed = (minuteLed + 30) % 60;
        strip.setPixelColor(minuteLed, minuteColor);

        int secondLed = timeinfo.tm_sec;
        secondLed = (secondLed + 30) % 60;
        strip.setPixelColor(secondLed, secondColor);
    }

    strip.show();
}

void flashColor(uint32_t color, int flashes, int flash_delay) {
    uint8_t original_brightness = strip.getBrightness();
    strip.setBrightness(255);
    for (int i = 0; i < flashes; i++) {
        strip.fill(color);
        strip.show();
        delay(flash_delay);
        strip.clear();
        strip.show();
        delay(flash_delay);
    }
    strip.setBrightness(original_brightness);
}

void pulseBlue(int duration_ms) {
    for (int j = 0; j < 128; j+=15) { strip.fill(strip.Color(0, 0, j)); strip.show(); delay(duration_ms/17); }
    for (int j = 128; j > 0; j-=15) { strip.fill(strip.Color(0, 0, j)); strip.show(); delay(duration_ms/17); }
}

void pulseGreen(int duration_ms) {
    for (int j = 0; j < 128; j+=15) { strip.fill(strip.Color(0, j, 0)); strip.show(); delay(duration_ms/17); }
    for (int j = 128; j > 0; j-=15) { strip.fill(strip.Color(0, j, 0)); strip.show(); delay(duration_ms/17); }
}


// =================================================================
// Web Server & System Setup
// =================================================================

void startWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["ip"] = WiFi.localIP().toString();
        doc["battery"] = battery_level;
        doc["connection"] = "Connected";
        doc["ble"] = bleAdvertising ? "On" : "Off";

        JsonArray timersArray = doc.createNestedArray("timers");
        for (auto &timer : timers) {
            JsonObject timerObj = timersArray.createNestedObject();
            timerObj["id"] = timer.id;
            char hexColor[8];
            uint32_t c = timer.color;
            sprintf(hexColor, "#%02x%02x%02x", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            timerObj["color"] = hexColor;
            timerObj["active"] = timer.active;
            timerObj["finished"] = timer.finished;
            if (timer.active) {
                timer.remainingTime = timer.endTime - millis();
            }
            timerObj["remainingTime"] = timer.remainingTime;
        }

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    server.on("/addTimer", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("duration", true) && request->hasParam("color", true)) {
            unsigned long duration = request->getParam("duration", true)->value().toInt() * 60000;
            const char* hex_color_string = request->getParam("color", true)->value().c_str();
            uint32_t color_val = strtol(hex_color_string, NULL, 16);
            uint32_t color = strip.Color((color_val >> 16) & 0xFF, (color_val >> 8) & 0xFF, color_val & 0xFF);
            timers.push_back({nextTimerId++, millis() + duration, duration, duration, color, true, false});
        }
        request->send(200);
    });

    server.on("/toggleTimer", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("id", true)) {
            int id = request->getParam("id", true)->value().toInt();
            for (auto &timer : timers) {
                if (timer.id == id && !timer.finished) {
                    timer.active = !timer.active;
                    if (timer.active) {
                        timer.endTime = millis() + timer.remainingTime;
                    } else {
                        timer.remainingTime = timer.endTime - millis();
                    }
                }
            }
        }
        request->send(200);
    });

    server.on("/deleteTimer", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("id", true)) {
            int id = request->getParam("id", true)->value().toInt();
            timers.erase(std::remove_if(timers.begin(), timers.end(), [id](const Timer& t){ return t.id == id; }), timers.end());
        }
        request->send(200);
    });

    server.on("/setBrightness", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("value", true)) {
            brightness = request->getParam("value", true)->value().toInt();
            strip.setBrightness(brightness);
        }
        request->send(200);
    });

    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Rebooting...");
        delay(1000);
        ESP.restart();
    });

    server.begin();
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

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><body><h1>WiFi Setup</h1><form action='/save' method='POST'>SSID: <input type='text' name='ssid'><br>Password: <input type='password' name='password'><br><input type='submit' value='Save'></form></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        preferences.putString("ssid", request->arg("ssid"));
        preferences.putString("password", request->arg("password"));
        preferences.end();
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest *request){
        request->redirect("http://" + WiFi.softAPIP().toString());
    });

    server.begin();
}

void startBLE() {
    BLEDevice::init("NeoPixel Clock");
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(SERVICE_UUID);
    BLECharacteristic *pWiFiCredsCharacteristic = pService->createCharacteristic(WIFI_CREDS_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
    pWiFiCredsCharacteristic->setCallbacks(new WiFiCredsCharacteristicCallbacks());
    pIpAddressCharacteristic = pService->createCharacteristic(IP_ADDRESS_CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pIpAddressCharacteristic->addDescriptor(new BLE2902());
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    bleAdvertising = true;
}

void updateTimers() {
    for (auto &timer : timers) {
        if (timer.active && !timer.finished && millis() >= timer.endTime) {
            timer.finished = true;
            timer.active = false;
            flashColor(timer.color, 5, 200);
        }
    }
}
