
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <time.h>
#include <math.h>
#include "rabtech_secrets.h"

// =============================================
// RABTECH - PHYSICAL HARDWARE + CLOUD
// =============================================

#define SIMULATION_MODE false

const char* FIREBASE_URL =
  "https://rabtech-80cc2-default-rtdb.asia-southeast1.firebasedatabase.app";

const char* AUTH_URL =
  "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";

// CONFIRMED GPIO PINS
const int TRIG[3] = { 5, 2, 21 };
const int ECHO[3] = { 18, 4, 22 };
const int SERVO_PINS[3] = { 13, 12, 14 };
const int FAN_PIN = 27;
const int WATER_TRIG_PIN = 26;
const int WATER_ECHO_PIN = 25;

// SERVO CALIBRATION
const int SERVO_CLOSED_ANGLE[3] = { 0, 0, 0 };
const int SERVO_OPEN_ANGLE[3] = { 90, 90, 90 };
const unsigned long SERVO_STAGGER_MS = 150; // Keep short; verify servo supply

// RELAY CONFIGURATION
const bool RELAY_ACTIVE_LOW = true;
const float FAN_ON_C = 38.0f;
const float FAN_OFF_C = 37.0f;

// PELLET SENSOR CALIBRATION
const float PELLET_FULL_CM[3] = { 3, 3, 3 };
const float PELLET_EMPTY_CM[3] = { 17.3f, 17.6f, 17.9f }; // Verify against repeated empty readings

// WATER SENSOR CALIBRATION
const float WATER_FULL_CM = 25.0f;
const float WATER_EMPTY_CM = 34.0f;
const float WATER_MIN_RELIABLE_CM = 20.1f;

// WEATHER API
// Approximate Victoria, Oriental Mindoro coordinates.
const float WEATHER_LATITUDE = 13.1770f;
const float WEATHER_LONGITUDE = 121.2780f;

const unsigned long WEATHER_INTERVAL_MS = 300000UL;
const unsigned long WEATHER_STALE_MS = 900000UL;
const unsigned long PELLET_INTERVAL_MS = 6000UL;
const unsigned long WATER_INTERVAL_MS = 3000UL;

WebServer server(80);
Preferences prefs;
Servo servos[3];

// STATES
int pellet[3] = { 0, 0, 0 };
bool pelletValid[3] = { false, false, false };
bool hatch[3] = { false, false, false };
bool fanOn = false;
// Manual commands override temperature automation until auto mode is restored.
bool fanManualMode = false;
bool fanManualValue = false;
volatile bool autoFeeding = false;

float temperature = NAN;
bool temperatureValid = false;
unsigned long lastWeatherSuccess = 0;

float waterDistanceCm = -1;
int waterLevel = -1;
bool waterValid = false;
bool waterReliable = false;

bool scheduleEnabled[3] = { false, false, false };
int scheduleHour[3] = { 0, 0, 0 };
int scheduleMinute[3] = { 0, 0, 0 };
// Daily deduplication is persisted in Preferences, not just RAM.
// Tracks the last date each schedule was executed
int lastFedDate[3] = {-1, -1, -1};
int64_t lastFedMinuteKey = -1; // Prevent overlapping slots from firing in the same minute

// Servo open duration: 3 seconds
const unsigned long FEED_DURATION_MS = 3000UL;

volatile unsigned long feedStarted = 0;
unsigned long lastSafetyCheck = 0;
unsigned long lastWaterRead = 0;
unsigned long lastPelletRead = 0;
unsigned long lastWeatherPoll = 0;
unsigned long lastPublish = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastSchedulePoll = 0;
unsigned long lastLoginAttempt = 0;
unsigned long tokenCreated = 0;

String idToken = "";
String lastCommandId = "";
String lastCommandResult = "none";

// =============================================
// HELPERS
// =============================================

bool anyHatchOpen() {
  return hatch[0] || hatch[1] || hatch[2];
}

bool freshTemperature() {
  return temperatureValid && millis() - lastWeatherSuccess <= WEATHER_STALE_MS;
}

void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader(
    "Access-Control-Allow-Methods",
    "GET, POST, OPTIONS");
}

void logEvent(const String& message) {
  Serial.println("[RABTECH] " + message);

  struct tm t;
  if (!getLocalTime(&t, 100)) return;

  char stamp[32];
  strftime(stamp, sizeof(stamp),
           "%Y-%m-%d %H:%M:%S", &t);

  File file = LittleFS.open("/logs.txt", FILE_APPEND);

  if (file) {
    file.println(String(stamp) + " - " + message);
    file.close();
  }
}

// =============================================
// SERVO CONTROL
// =============================================

void setHatch(int index, bool open) {
  if (index < 0 || index >= 3) return;
  if (hatch[index] == open) return;

  if (!SIMULATION_MODE) {
    int angle = open
                  ? SERVO_OPEN_ANGLE[index]
                  : SERVO_CLOSED_ANGLE[index];

    servos[index].write(angle);
  }

  hatch[index] = open;

  logEvent(
    "Hatch " + String(index + 1) + (open ? " opened" : " closed"));
}

void setAllHatches(bool open) {
  for (int i = 0; i < 3; i++) {
    setHatch(i, open);

    if (!SIMULATION_MODE && i < 2) {
      delay(SERVO_STAGGER_MS);
    }
  }
}

// =============================================
// FAN CONTROL
// =============================================

void setFan(bool on) {
  if (fanOn == on) return;

  if (!SIMULATION_MODE) {
    int level;

    if (RELAY_ACTIVE_LOW) {
      level = on ? LOW : HIGH;
    } else {
      level = on ? HIGH : LOW;
    }

    digitalWrite(FAN_PIN, level);
  }

  fanOn = on;
  logEvent(on ? "Fan ON" : "Fan OFF");
}

void applyFanTemperatureRule() {
  // A manual ON or OFF is always respected, including above 38 C.
  if (fanManualMode) {
    setFan(fanManualValue);
    return;
  }

  // Unknown weather is NOT a reason to turn the fan on.
  // For animal safety, use a physical temperature sensor and
  // independent ventilation; outdoor weather is not enclosure temperature.
  if (!freshTemperature()) {
    setFan(false);
    return;
  }

  // Automatic fan is ON strictly above 38 C, OFF otherwise.
  setFan(temperature > FAN_ON_C);
}

void setManualFan(bool on) {
  fanManualMode = true;
  fanManualValue = on;
  prefs.putBool("fanManual", true);
  prefs.putBool("fanValue", on);
  setFan(on);
  logEvent(on ? "Fan manual ON" : "Fan manual OFF");
}

void setFanAuto() {
  fanManualMode = false;
  prefs.putBool("fanManual", false);
  applyFanTemperatureRule();
  logEvent("Fan returned to automatic mode");
}

// =============================================
// ULTRASONIC READING
// =============================================

float readDistanceCm(int trigPin,
                     int echoPin,
                     unsigned long timeoutUs) {

  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  unsigned long duration =
    pulseIn(echoPin, HIGH, timeoutUs);

  if (duration == 0) return NAN;

  return duration * 0.0343f / 2.0f;
}

// =============================================
// PELLET SENSORS
// =============================================

void readPelletSensors() {
  if (SIMULATION_MODE) return;

  for (int i = 0; i < 3; i++) {
    float cm = readDistanceCm(
      TRIG[i], ECHO[i], 25000UL);

    if (!isfinite(cm) || cm < 2 || cm > 400) {
      pelletValid[i] = false;
      pellet[i] = -1;

      Serial.printf(
        "[Pellet %d] Invalid/no echo\n", i + 1);

    } else {
      float span =
        PELLET_EMPTY_CM[i] - PELLET_FULL_CM[i];

      if (span <= 0) {
        pelletValid[i] = false;
        pellet[i] = -1;
      } else {
        float percent =
          (PELLET_EMPTY_CM[i] - cm) * 100.0f / span;

        // Account for small sensor fluctuations near empty
        if (cm >= PELLET_EMPTY_CM[i] - 0.5f) {
          percent = 0.0f;
        }

        // Keep percentage within 0-100
        pellet[i] = constrain(
          (int)roundf(percent), 0, 100);

        pelletValid[i] = true;

        Serial.printf(
          "[Pellet %d] %.1f cm = %d%%\n",
          i + 1, cm, pellet[i]);
      }
    }

    // Prevent ultrasonic cross-talk
    delay(65);
  }
}

// =============================================
// WATER SENSOR
// =============================================

void readWaterSensor() {
  if (SIMULATION_MODE) return;

  float cm = readDistanceCm(
    WATER_TRIG_PIN,
    WATER_ECHO_PIN,
    30000UL);

  if (!isfinite(cm) || cm < 2 || cm > 450) {
    waterValid = false;
    waterReliable = false;
    waterDistanceCm = -1;
    waterLevel = -1;

    Serial.println("[Water] Invalid/no echo");
    return;
  }

  waterDistanceCm = cm;
  waterValid = true;
  waterReliable = cm >= WATER_MIN_RELIABLE_CM;

  float percent =
    (WATER_EMPTY_CM - cm) * 100.0f / (WATER_EMPTY_CM - WATER_FULL_CM);

  waterLevel = constrain(
    (int)roundf(percent), 0, 100);

  Serial.printf(
    "[Water] %.1f cm = %d%% | reliable=%s\n",
    cm, waterLevel,
    waterReliable ? "yes" : "no");
}

void addWaterFields(JsonDocument& doc) {
  if (waterValid) {
    doc["waterDistanceCm"] = waterDistanceCm;
  } else {
    doc["waterDistanceCm"] = nullptr;
  }

  if (waterValid && waterReliable) {
    doc["waterLevel"] = waterLevel;
  } else {
    doc["waterLevel"] = nullptr;
  }

  doc["waterValid"] = waterValid;
  doc["waterReliable"] = waterReliable;
}

// =============================================
// WEATHER API
// =============================================

void pollWeatherTemperature() {
  if (!isfinite(WEATHER_LATITUDE) || !isfinite(WEATHER_LONGITUDE) ||
      WEATHER_LATITUDE < -90 || WEATHER_LATITUDE > 90 ||
      WEATHER_LONGITUDE < -180 || WEATHER_LONGITUDE > 180) {

    Serial.println(
      "[Weather] Configure latitude/longitude");
    return;
  }

  String url =
    "https://api.open-meteo.com/v1/forecast?latitude=" + String(WEATHER_LATITUDE, 6) + "&longitude=" + String(WEATHER_LONGITUDE, 6) + "&current=temperature_2m"
                                                                                                                                      "&temperature_unit=celsius";

  WiFiClientSecure client;
  client.setInsecure();  // Testing only

  HTTPClient http;
  http.setTimeout(5000);

  if (!http.begin(client, url)) return;

  int code = http.GET();

  if (code == 200) {
    JsonDocument doc;

    if (!deserializeJson(doc, http.getString())) {
      JsonVariant v =
        doc["current"]["temperature_2m"];

      if (!v.isNull()) {
        float t = v.as<float>();

        if (isfinite(t) && t >= -60 && t <= 70) {
          temperature = t;
          temperatureValid = true;
          lastWeatherSuccess = millis();

          Serial.printf(
            "[Weather] Temperature: %.1f C\n",
            temperature);

          applyFanTemperatureRule();
        }
      }
    }
  } else {
    Serial.printf("[Weather] HTTP %d\n", code);
  }

  http.end();
}

// =============================================
// FIREBASE HTTP
// =============================================

bool httpRequest(String url,
                 String method,
                 String payload,
                 String& response,
                 int& status) {

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // Testing only

  HTTPClient http;
  http.setTimeout(5000);

  if (!http.begin(client, url)) return false;

  if (method != "GET") {
    http.addHeader(
      "Content-Type", "application/json");
  }

  if (method == "GET") {
    status = http.GET();
  } else if (method == "POST") {
    status = http.POST(payload);
  } else if (method == "PUT") {
    status = http.PUT(payload);
  } else if (method == "PATCH") {
    status = http.sendRequest("PATCH", payload);
  } else {
    http.end();
    return false;
  }

  if (status > 0) {
    response = http.getString();
  }

  http.end();

  return status >= 200 && status < 300;
}

bool firebaseLogin() {
  if (strlen(RABTECH_DEVICE_EMAIL) == 0 || strlen(RABTECH_DEVICE_PASSWORD) == 0) {
    return false;
  }

  JsonDocument body;
  body["email"] = RABTECH_DEVICE_EMAIL;
  body["password"] = RABTECH_DEVICE_PASSWORD;
  body["returnSecureToken"] = true;

  String payload;
  serializeJson(body, payload);

  String response;
  int code = 0;

  bool success = httpRequest(
    String(AUTH_URL) + RABTECH_FIREBASE_API_KEY,
    "POST", payload, response, code);

  if (!success) {
    Serial.printf(
      "[Firebase] Login failed HTTP %d\n", code);
    return false;
  }

  JsonDocument result;

  if (deserializeJson(result, response)) {
    return false;
  }

  idToken = result["idToken"].as<String>();

  if (idToken.isEmpty()) return false;

  tokenCreated = millis();

  Serial.println("[Firebase] Signed in");
  return true;
}

bool ensureFirebaseLogin() {
  if (!idToken.isEmpty() && millis() - tokenCreated < 3000000UL) {
    return true;
  }

  if (lastLoginAttempt != 0 && millis() - lastLoginAttempt < 10000UL) {
    return false;
  }

  lastLoginAttempt = millis();
  idToken = "";

  return firebaseLogin();
}

bool firebaseRequest(String path,
                     String method,
                     String payload,
                     String& response) {

  if (!ensureFirebaseLogin()) return false;

  String url =
    String(FIREBASE_URL) + path + ".json?auth=" + idToken;

  int code = 0;

  bool success = httpRequest(
    url, method, payload, response, code);

  if (!success) {
    Serial.printf(
      "[Firebase] %s %s HTTP %d\n",
      method.c_str(), path.c_str(), code);

    if (code == 401) idToken = "";
  }

  return success;
}

// =============================================
// STATUS DATA
// =============================================

void fillStatus(JsonDocument& doc) {
  doc["online"] = true;

  for (int i = 0; i < 3; ++i) {
    String key = "l" + String(i + 1);
    if (pelletValid[i]) doc[key] = pellet[i];
    else doc[key] = nullptr;
  }

  doc["pelletSensor1Valid"] = pelletValid[0];
  doc["pelletSensor2Valid"] = pelletValid[1];
  doc["pelletSensor3Valid"] = pelletValid[2];

  doc["fan"] = fanOn;
  doc["fanMode"] = fanManualMode ? "manual" : "auto";
  doc["fanManual"] = fanManualMode;
  doc["door"] = anyHatchOpen();

  doc["hatch1"] = hatch[0];
  doc["hatch2"] = hatch[1];
  doc["hatch3"] = hatch[2];

  if (freshTemperature()) {
    doc["temp"] = temperature;
  } else {
    doc["temp"] = nullptr;
  }

  doc["tempValid"] = freshTemperature();
  doc["tempSource"] = "Open-Meteo outdoor API";

  addWaterFields(doc);

  doc["lastSeen"] =
    (int64_t)time(nullptr) * 1000LL;

  doc["lastCommandId"] = lastCommandId;
  doc["lastCommandResult"] = lastCommandResult;
}

void publishStatus() {
  if (time(nullptr) < 1700000000) return;

  JsonDocument doc;
  fillStatus(doc);

  String payload;
  String response;

  serializeJson(doc, payload);

  if (firebaseRequest(
        "/rabtech/status",
        "PATCH",
        payload,
        response)) {
    Serial.println("[Firebase] Status updated");
  }
}

// =============================================
// FIREBASE COMMANDS
// =============================================

void pollCommands() {
  String response;

  if (!firebaseRequest(
        "/rabtech/commands",
        "GET", "", response)) return;

  if (response == "null") return;

  JsonDocument doc;

  if (deserializeJson(doc, response)) return;

  String requestId = doc["requestId"] | "";

  if (requestId.isEmpty() || requestId == lastCommandId) return;

  String action = doc["action"] | "";
  String target = doc["target"] | "";
  bool value = doc["value"] | false;

  int64_t issuedAt = doc["issuedAt"].as<int64_t>();
  int64_t nowMs = (int64_t)time(nullptr) * 1000LL;

  lastCommandId = requestId;
  prefs.putString("lastCmd", lastCommandId);

  if (nowMs < 1700000000000LL || issuedAt <= 0 || issuedAt > nowMs + 5000LL || nowMs - issuedAt > 30000LL) {

    lastCommandResult = "expired";
    publishStatus();
    return;
  }

  if (action == "setFan" && target == "fan") {
    // Existing dashboard setFan true/false now works at any temperature.
    setManualFan(value);
    lastCommandResult = "ok";

  } else if ((action == "setFanAuto" || action == "autoFan") &&
             (target == "fan" || target == "auto")) {
    setFanAuto();
    lastCommandResult = "ok";

  } else if (action == "setHatch" && target.startsWith("hatch")) {

    int index = target.substring(5).toInt() - 1;

    if (index < 0 || index > 2) {
      lastCommandResult = "invalid_target";
    } else if (autoFeeding) {
      lastCommandResult = "busy";
    } else {
      setHatch(index, value);
      lastCommandResult = "ok";
    }

  } else if (action == "setAllHatches" && target == "all") {

    if (autoFeeding) {
      lastCommandResult = "busy";
    } else {
      setAllHatches(value);
      lastCommandResult = "ok";
    }

  } else {
    lastCommandResult = "unknown_action";
  }

  publishStatus();
}

// =============================================
// SCHEDULES
// =============================================

bool validTime(String t) {
  if (t.length() != 5 || t[2] != ':') return false;

  for (int i = 0; i < 5; i++) {
    if (i == 2) continue;
    if (!isDigit(t[i])) return false;
  }

  return t.substring(0, 2).toInt() < 24 && t.substring(3, 5).toInt() < 60;
}

void setSchedule(int slot, String t) {
  if (slot < 0 || slot >= 3) return;

  if (t.isEmpty()) {
    scheduleEnabled[slot] = false;
    return;
  }

  if (!validTime(t)) return;

  int newHour = t.substring(0, 2).toInt();
  int newMinute = t.substring(3, 5).toInt();

  // Only reset when the actual schedule changes
  bool changed =
    !scheduleEnabled[slot] ||
    scheduleHour[slot] != newHour ||
    scheduleMinute[slot] != newMinute;

  scheduleHour[slot] = newHour;
  scheduleMinute[slot] = newMinute;
  scheduleEnabled[slot] = true;

  if (changed) {
    // A schedule edit must NOT erase today's completed feeding record.
    logEvent(
      "Schedule " + String(slot + 1) +
      " updated to " + t
    );
  }
}

void pollSchedules() {
  String response;

  if (!firebaseRequest(
        "/rabtech/schedules",
        "GET", "", response)) return;

  JsonDocument doc;

  if (deserializeJson(doc, response)) return;

  for (int i = 0; i < 3; i++) {
    String key = "slot" + String(i + 1);
    JsonVariant v = doc[key];

    if (v.isNull()) { setSchedule(i, ""); continue; }

    String t = "";

    if (v.is<const char*>()) {
      t = v.as<String>();
    } else if (v.is<JsonObject>()) {
      if (v["enabled"] | false) {
        t = v["time"].as<String>();
      }
    }

    if (t.isEmpty() || validTime(t)) {
      setSchedule(i, t);
    }
  }
}

void checkFeedingSchedules() {
  if (autoFeeding || anyHatchOpen()) return;
  struct tm t;
  if (!getLocalTime(&t, 10)) return;
  const int today = (t.tm_year + 1900) * 1000 + t.tm_yday;
  const int64_t minuteKey = (int64_t)today * 1440 + t.tm_hour * 60 + t.tm_min;

  for (int i = 0; i < 3; ++i) {
    if (!scheduleEnabled[i] || t.tm_hour != scheduleHour[i] ||
        t.tm_min != scheduleMinute[i] || lastFedDate[i] == today ||
        lastFedMinuteKey == minuteKey) continue;

    // Persist BEFORE actuating, so a reset cannot feed twice.
    lastFedDate[i] = today;
    lastFedMinuteKey = minuteKey;
    String key = "fed" + String(i);
    if (prefs.putInt(key.c_str(), today) == 0 ||
        prefs.putLong64("fedMinute", minuteKey) == 0) {
      logEvent("Feeding cancelled: failed to persist schedule marker");
      return;
    }

    autoFeeding = true;
    setAllHatches(true);
    feedStarted = millis();
    logEvent("Scheduled feeding slot " + String(i + 1) + " started");
    break;
  }
}

// =============================================
// LOCAL WEB API
// =============================================

void setupWebServer() {
  // LAN API is read-only by default. All remote actuation uses authenticated Firebase.
  // Avoid exposing unauthenticated GET commands on a network.
  server.on("/", HTTP_GET, []() {
    addCORS();
    server.send(
      200, "text/plain",
      "RABTECH ESP32 API Running");
  });

  server.on("/status", HTTP_GET, []() {
    addCORS();

    JsonDocument doc;
    fillStatus(doc);

    JsonArray schedules =
      doc["schedules"].to<JsonArray>();

    for (int i = 0; i < 3; i++) {
      if (scheduleEnabled[i]) {
        char buf[6];

        snprintf(
          buf, sizeof(buf),
          "%02d:%02d",
          scheduleHour[i],
          scheduleMinute[i]);

        schedules.add(buf);
      } else {
        schedules.add(nullptr);
      }
    }

    String result;
    serializeJson(doc, result);

    server.send(
      200, "application/json", result);
  });

  // Legacy unauthenticated local control endpoints intentionally disabled.
  for (const char* path : {"/toggle", "/toggle1", "/toggle2", "/toggle3", "/fan", "/schedule"}) {
    server.on(path, HTTP_GET, []() {
      addCORS();
      server.send(403, "text/plain", "Use authenticated Firebase commands");
    });
  }

  server.on("/logs", HTTP_GET, []() {
    addCORS();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File file =
      LittleFS.open("/logs.txt", FILE_READ);

    if (file) {
      while (file.available()) {
        String line =
          file.readStringUntil('\n');

        line.trim();

        if (!line.isEmpty()) {
          arr.add(line);
        }
      }

      file.close();
    }

    String result;
    serializeJson(doc, result);

    server.send(
      200, "application/json", result);
  });

  server.on("/clearlogs", HTTP_GET, []() {
    addCORS();

    server.send(403, "text/plain", "Disabled");
  });

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCORS();
      server.send(204);
    } else {
      server.send(
        404, "text/plain", "Not Found");
    }
  });

  server.begin();

  Serial.println("[Web] Local API ready");
}

// =============================================
// WIFI
// =============================================

void setupWiFi() {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  wm.setDebugOutput(true);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);

  Serial.println("[WiFi] Starting WiFiManager");

  bool connected =
    wm.autoConnect("Rabtech-Setup");

  if (!connected || WiFi.status() != WL_CONNECTED) {

    Serial.println("[WiFi] Connection failed");
    delay(5000);
    ESP.restart();
  }

  Serial.println("[WiFi] Connected");
  Serial.println(WiFi.localIP());
}


// Dedicated safety task closes scheduled hatches even when HTTPS blocks loop().
// Do not use servo control from two tasks concurrently: only this task closes
// automatic feed; main loop opens it and never closes an active automatic feed.
void feedingSafetyTask(void* arg) {
  for (;;) {
    if (autoFeeding && millis() - feedStarted >= FEED_DURATION_MS) {
      setAllHatches(false);
      autoFeeding = false;
      Serial.println("[Safety] Automatic feed closed");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =============================================
// SETUP
// =============================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("==========================");
  Serial.println(" RABTECH SMART FEEDER ");
  Serial.println("==========================");

  if (!SIMULATION_MODE) {

    // Relay OFF before output initialization
    digitalWrite(
      FAN_PIN,
      RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(FAN_PIN, OUTPUT);

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);

    for (int i = 0; i < 3; i++) {
      servos[i].setPeriodHertz(50);

      servos[i].attach(
        SERVO_PINS[i], 500, 2400);

      servos[i].write(
        SERVO_CLOSED_ANGLE[i]);

      pinMode(TRIG[i], OUTPUT);
      pinMode(ECHO[i], INPUT);

      digitalWrite(TRIG[i], LOW);
    }

    pinMode(WATER_TRIG_PIN, OUTPUT);
    pinMode(WATER_ECHO_PIN, INPUT);

    digitalWrite(WATER_TRIG_PIN, LOW);

    readWaterSensor();
    readPelletSensors();
  }

  LittleFS.begin(true);

  prefs.begin("rabtech-fb", false);
  lastCommandId =
    prefs.getString("lastCmd", "");
  for (int i = 0; i < 3; ++i) {
    String key = "fed" + String(i);
    lastFedDate[i] = prefs.getInt(key.c_str(), -1);
  }
  lastFedMinuteKey = prefs.getLong64("fedMinute", -1);
  fanManualMode = prefs.getBool("fanManual", false);
  fanManualValue = prefs.getBool("fanValue", false);
  applyFanTemperatureRule();

  setupWiFi();

  configTime(
    8 * 3600, 0,
    "pool.ntp.org",
    "time.google.com");

  unsigned long started = millis();

  while (time(nullptr) < 1700000000 && millis() - started < 15000UL) {
    delay(250);
  }

  if (MDNS.begin("rabtech")) {
    Serial.println(
      "[Web] http://rabtech.local");
  }

  setupWebServer();
  pollSchedules();
  xTaskCreatePinnedToCore(feedingSafetyTask, "feedSafety", 4096, nullptr, 3, nullptr, 1);

  Serial.println("[RABTECH] Setup complete");
}

// =============================================
// MAIN LOOP
// =============================================

void loop() {
  server.handleClient();

  // Feeding continues without internet
  checkFeedingSchedules();
  applyFanTemperatureRule();

  unsigned long now = millis();

  // Local sensors continue without Wi-Fi
  if (!SIMULATION_MODE && now - lastWaterRead >= WATER_INTERVAL_MS) {

    lastWaterRead = now;
    readWaterSensor();
  }

  if (!SIMULATION_MODE && now - lastPelletRead >= PELLET_INTERVAL_MS) {

    lastPelletRead = now;
    readPelletSensors();
  }

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;

    if (now - lastReconnect >= 10000UL) {
      lastReconnect = now;
      WiFi.reconnect();
    }

    delay(10);
    return;
  }

  if (now - lastWeatherPoll >= WEATHER_INTERVAL_MS || lastWeatherPoll == 0) {

    lastWeatherPoll = now;
    pollWeatherTemperature();
  }

  if (temperatureValid && now - lastWeatherSuccess > WEATHER_STALE_MS) {

    temperatureValid = false;
  }

  if (now - lastCommandPoll >= 2500UL) {
    lastCommandPoll = now;
    pollCommands();
  }

  if (now - lastSchedulePoll >= 20000UL) {
    lastSchedulePoll = now;
    pollSchedules();
  }

  if (now - lastPublish >= 10000UL) {
    lastPublish = now;
    publishStatus();
  }

  delay(10);
}
