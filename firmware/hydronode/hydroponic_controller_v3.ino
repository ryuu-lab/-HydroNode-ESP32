/*
 * ============================================================
 *  HydroNode ESP32 Firmware  v3  — Render cloud backend
 * ============================================================
 *  Hardware:
 *    GPIO26 — Relay        (active-LOW, pump)
 *    GPIO27 — Float switch (INPUT_PULLUP, LOW = water OK)
 *    GPIO34 — TDS AOUT     (ADC)
 *    GPIO32 — TDS power    (NPN transistor base via 1kΩ)
 *    GPIO25 — Push button  (INPUT_PULLUP, hold 800ms = manual burst)
 *
 *  Libraries needed:
 *    - ESPAsyncWebServer + AsyncTCP
 *    - ArduinoJson        (Benoit Blanchon)
 *    - Preferences        (built-in)
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ── Config ───────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Your Render URL — no trailing slash
const char* RENDER_URL    = "https://your-app-name.onrender.com";

// Must match API_KEY environment variable set on Render
const char* API_KEY       = "hydronode-secret";

// ── Pins ─────────────────────────────────────────────────────
#define RELAY_PIN       26
#define FLOAT_PIN       27
#define TDS_AOUT_PIN    34
#define TDS_POWER_PIN   32
#define BUTTON_PIN      25

// ── Active-LOW relay ─────────────────────────────────────────
#define RELAY_ACTIVE_LOW true
inline void pumpOn()  { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW  : HIGH); }
inline void pumpOff() { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);  }

// ── TDS calibration ──────────────────────────────────────────
#define TDS_COEFF 0.5f

// ── NVS ──────────────────────────────────────────────────────
Preferences prefs;

// ── State ────────────────────────────────────────────────────
enum PumpMode { AUTO, MANUAL_ON, MANUAL_OFF };
PumpMode      pumpMode      = AUTO;
bool          pumpRunning   = false;
unsigned long pumpTimer     = 0;
bool          waterLow      = false;
float         tdsPPM        = 0.0;
String        statusMsg     = "Starting...";
unsigned long pumpOnDuration  = 15000;
unsigned long pumpOffDuration = 60000;

// ── Button ───────────────────────────────────────────────────
unsigned long btnPressTime  = 0;
bool          btnWasPressed = false;
#define BTN_HOLD_MS       800
#define BTN_PUMP_DURATION 5000

// ── Timing ───────────────────────────────────────────────────
unsigned long lastPush    = 0;
unsigned long lastPoll    = 0;
#define PUSH_INTERVAL  5000   // push data every 5s
#define POLL_INTERVAL  3000   // poll commands every 3s

// ── TDS read ─────────────────────────────────────────────────
float readTDS() {
  digitalWrite(TDS_POWER_PIN, HIGH);
  delay(100);
  analogRead(TDS_AOUT_PIN);    // dummy
  delay(10);
  int raw = analogRead(TDS_AOUT_PIN);
  digitalWrite(TDS_POWER_PIN, LOW);
  float v = raw * 3.3f / 4095.0f;
  float ppm = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * TDS_COEFF;
  return max(0.0f, ppm);
}

// ── NVS ──────────────────────────────────────────────────────
void loadSettings() {
  prefs.begin("pump", false);
  pumpOnDuration  = prefs.getULong("onDur",  15000);
  pumpOffDuration = prefs.getULong("offDur", 60000);
  pumpMode        = (PumpMode)prefs.getInt("mode", 0);
  prefs.end();
}
void saveSettings() {
  prefs.begin("pump", false);
  prefs.putULong("onDur",  pumpOnDuration);
  prefs.putULong("offDur", pumpOffDuration);
  prefs.putInt("mode",     (int)pumpMode);
  prefs.end();
}

// ── Push data to Render ──────────────────────────────────────
void pushData() {
  if (WiFi.status() != WL_CONNECTED) return;

  String modeStr = pumpMode == AUTO      ? "AUTO" :
                   pumpMode == MANUAL_ON ? "ON"   : "OFF";

  StaticJsonDocument<200> doc;
  doc["tds"]      = (int)tdsPPM;
  doc["waterLow"] = waterLow;
  doc["pump"]     = pumpRunning;
  doc["mode"]     = modeStr;
  doc["status"]   = statusMsg;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  String url = String(RENDER_URL) + "/api/update";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int code = http.POST(body);
  if (code > 0) Serial.printf("[Push] HTTP %d\n", code);
  else          Serial.printf("[Push] Error: %s\n", http.errorToString(code).c_str());
  http.end();
}

// ── Poll Render for commands ──────────────────────────────────
void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(RENDER_URL) + "/api/command";
  http.begin(url);
  http.addHeader("X-API-Key", API_KEY);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<200> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && !doc["command"].isNull()) {
      JsonObject cmd = doc["command"];
      String type = cmd["type"].as<String>();

      if (type == "MODE") {
        String val = cmd["value"].as<String>();
        if      (val == "AUTO") pumpMode = AUTO;
        else if (val == "ON")   pumpMode = MANUAL_ON;
        else if (val == "OFF")  pumpMode = MANUAL_OFF;
        saveSettings();
        Serial.println("[CMD] Mode → " + val);

      } else if (type == "TIMER") {
        pumpOnDuration  = (unsigned long)cmd["onSec"]  * 1000;
        pumpOffDuration = (unsigned long)cmd["offSec"] * 1000;
        pumpTimer = millis();
        saveSettings();
        Serial.printf("[CMD] Timer → ON:%lus OFF:%lus\n",
          pumpOnDuration/1000, pumpOffDuration/1000);
      }
    }
  }
  http.end();
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN,     OUTPUT);
  pinMode(FLOAT_PIN,     INPUT_PULLUP);
  pinMode(TDS_POWER_PIN, OUTPUT);
  pinMode(BUTTON_PIN,    INPUT_PULLUP);
  pumpOff();
  digitalWrite(TDS_POWER_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadSettings();

  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\nConnected. IP: " + WiFi.localIP().toString());
  else
    Serial.println("\nWiFi failed — running in local-only mode");

  pumpTimer = millis();
  Serial.println("Ready.");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── Float switch ──────────────────────────────────────────
  waterLow = (digitalRead(FLOAT_PIN) == HIGH);

  // ── TDS read every 5s ─────────────────────────────────────
  static unsigned long lastTDS = 0;
  if (now - lastTDS > 5000) {
    tdsPPM  = readTDS();
    lastTDS = now;
  }

  // ── Push button ───────────────────────────────────────────
  bool btnDown = (digitalRead(BUTTON_PIN) == LOW);
  if (btnDown && !btnWasPressed) {
    btnPressTime  = now;
    btnWasPressed = true;
  }
  if (!btnDown && btnWasPressed) {
    if (now - btnPressTime >= BTN_HOLD_MS && !waterLow) {
      Serial.println("[Button] Manual pump burst");
      pumpOn();
      delay(BTN_PUMP_DURATION);
      pumpOff();
    }
    btnWasPressed = false;
  }

  // ── Pump logic ────────────────────────────────────────────
  if (waterLow) {
    pumpOff();
    pumpRunning = false;
    statusMsg   = "LOW WATER — pump disabled";

  } else if (pumpMode == MANUAL_ON) {
    pumpOn();
    pumpRunning = true;
    statusMsg   = "Manual ON";

  } else if (pumpMode == MANUAL_OFF) {
    pumpOff();
    pumpRunning = false;
    statusMsg   = "Manual OFF";

  } else {
    if (!pumpRunning && (now - pumpTimer >= pumpOffDuration)) {
      pumpOn();
      pumpRunning = true;
      pumpTimer   = now;
      statusMsg   = "Pump running";
    } else if (pumpRunning && (now - pumpTimer >= pumpOnDuration)) {
      pumpOff();
      pumpRunning = false;
      pumpTimer   = now;
      statusMsg   = "Pump resting";
    } else if (!pumpRunning) {
      unsigned long remaining = (pumpOffDuration - (now - pumpTimer)) / 1000;
      statusMsg = "Next cycle in " + String(remaining) + "s";
    }
  }

  // ── Push to Render every 5s ───────────────────────────────
  if (now - lastPush > PUSH_INTERVAL) {
    pushData();
    lastPush = now;
  }

  // ── Poll Render for commands every 3s ─────────────────────
  if (now - lastPoll > POLL_INTERVAL) {
    pollCommands();
    lastPoll = now;
  }
}
