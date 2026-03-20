/*
 * ESP32 Pin Simulator — Firmware
 * ================================
 * Connects to the FastAPI backend and:
 *   - Reads all INPUT pins configured via the simulator frontend
 *   - Pushes readings to backend (POST /api/esp32/readings/batch)
 *   - Polls backend for OUTPUT commands (GET /api/esp32/outputs)
 *   - Applies output commands to actual GPIO pins
 *
 * Required Arduino Libraries:
 *   - WiFi.h            (built-in ESP32)
 *   - HTTPClient.h      (built-in ESP32)
 *   - ArduinoJson       (install via Library Manager → ArduinoJson by Benoit Blanchon)
 *
 * Board: ESP32 Dev Module
 * Flash: 4MB, CPU: 240MHz
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── CONFIGURATION ────────────────────────────────────────────
#define WIFI_SSID        "DESKTOP-lol"
#define WIFI_PASSWORD    "12345678"
#define BACKEND_HOST     "https://bms-production-2a2c.up.railway.app:443"   // ← your PC's local IP

// How often to push readings and pull output commands (milliseconds)
#define PUSH_INTERVAL_MS   200     // push sensor readings every 200ms
#define PULL_INTERVAL_MS   300     // pull output commands every 300ms
#define CONFIG_SYNC_MS    5000     // re-sync pin config from backend every 5s

// DAC resolution (8-bit for ESP32)
#define DAC_MAX  255
// ADC resolution (12-bit for ESP32)
#define ADC_MAX  4095
// Touch threshold — readings below this = touched
#define TOUCH_THRESHOLD  40

// Activity LED (optional, use LED_BUILTIN or set to -1 to disable)
#define ACTIVITY_LED  2


// ─── PIN TYPE DEFINITIONS ─────────────────────────────────────
enum SignalType {
  SIG_NONE,
  SIG_ADC,
  SIG_DIGITAL_IN,
  SIG_TOUCH,
  SIG_PWM_READ,
  SIG_DAC,
  SIG_PWM_OUT,
  SIG_DIGITAL_OUT,
};

struct PinInfo {
  int     gpio;
  SignalType signalType;
  String  pullMode;      // "none" | "up" | "down"
  bool    configured;
  // output values
  float   outputValue;   // normalized 0.0–1.0
  bool    digitalState;
  int     frequency;
  int     pwmChannel;    // LEDC channel assigned
};

// Max 38 GPIO pins on ESP32 but only ~30 usable
#define MAX_PINS 30
PinInfo pins[MAX_PINS];
int     pinCount = 0;

// Timing
unsigned long lastPush   = 0;
unsigned long lastPull   = 0;
unsigned long lastSync   = 0;
bool          wifiOk     = false;


// ─── UTILITY ──────────────────────────────────────────────────
SignalType parseSignalType(const char* s) {
  if (strcmp(s, "ADC")       == 0) return SIG_ADC;
  if (strcmp(s, "DIGITAL")   == 0) return SIG_DIGITAL_IN;
  if (strcmp(s, "TOUCH")     == 0) return SIG_TOUCH;
  if (strcmp(s, "PWM_READ")  == 0) return SIG_PWM_READ;
  if (strcmp(s, "DAC")       == 0) return SIG_DAC;
  if (strcmp(s, "PWM")       == 0) return SIG_PWM_OUT;
  return SIG_NONE;
}

// Check if GPIO is safe to use
bool isGpioSafe(int gpio) {
  // Internal flash — never touch
  if (gpio >= 6 && gpio <= 11) return false;
  // Must exist
  if (gpio < 0 || gpio > 39)   return false;
  return true;
}

// Check if GPIO supports output
bool isOutputCapable(int gpio) {
  if (!isGpioSafe(gpio)) return false;
  // Input-only pins
  if (gpio == 34 || gpio == 35 || gpio == 36 || gpio == 39) return false;
  return true;
}

bool isDacPin(int gpio) {
  return (gpio == 25 || gpio == 26);
}

bool isTouchPin(int gpio) {
  int touchPins[] = {0, 2, 4, 12, 13, 14, 15, 27, 32, 33};
  for (int i = 0; i < 10; i++) if (touchPins[i] == gpio) return true;
  return false;
}


// ─── WIFI ─────────────────────────────────────────────────────
void connectWiFi() {
  Serial.println("[WIFI] Connecting to " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  } else {
    wifiOk = false;
    Serial.println("\n[WIFI] Failed to connect — will retry");
  }
}


// ─── HTTP HELPERS ─────────────────────────────────────────────
String httpGET(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(String(BACKEND_HOST) + path);
  http.setTimeout(2000);
  int code = http.GET();
  String body = "";
  if (code == 200) body = http.getString();
  else Serial.printf("[HTTP] GET %s → %d\n", path.c_str(), code);
  http.end();
  return body;
}

int httpPOST(const String& path, const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.begin(String(BACKEND_HOST) + path);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);
  int code = http.POST(payload);
  if (code != 200) Serial.printf("[HTTP] POST %s → %d\n", path.c_str(), code);
  http.end();
  return code;
}


// ─── SYNC CONFIG FROM BACKEND ─────────────────────────────────
void syncPinConfig() {
  Serial.println("[SYNC] Fetching pin config...");
  String inputsBody  = httpGET("/api/esp32/inputs");
  String outputsBody = httpGET("/api/esp32/outputs");

  if (inputsBody.isEmpty() && outputsBody.isEmpty()) {
    Serial.println("[SYNC] Backend unreachable");
    return;
  }

  // Reset existing pin list
  pinCount = 0;

  // Parse INPUT pins
  if (!inputsBody.isEmpty()) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, inputsBody) == DeserializationError::Ok) {
      JsonObject inputs = doc["inputs"].as<JsonObject>();
      for (JsonPair kv : inputs) {
        int gpio = atoi(kv.key().c_str());
        if (!isGpioSafe(gpio)) continue;

        const char* sigTypeStr = kv.value()["signal_type"] | "DIGITAL";
        const char* pullStr    = kv.value()["pull_mode"]   | "none";
        SignalType st = parseSignalType(sigTypeStr);

        // Skip output-only types in input list
        if (st == SIG_DAC || st == SIG_PWM_OUT || st == SIG_DIGITAL_OUT) continue;
        // Remap DIGITAL signal type for input
        if (st == SIG_DIGITAL_IN || (st == SIG_NONE && strcmp(sigTypeStr,"DIGITAL")==0)) st = SIG_DIGITAL_IN;

        PinInfo& p = pins[pinCount++];
        p.gpio        = gpio;
        p.signalType  = st;
        p.pullMode    = String(pullStr);
        p.configured  = true;
        p.outputValue = 0.0;
        p.digitalState= false;
        p.frequency   = 1000;
        p.pwmChannel  = -1;

        // Configure GPIO
        if (st == SIG_ADC) {
          pinMode(gpio, INPUT);
          Serial.printf("[PIN] GPIO %d → ADC input  pull=%s\n", gpio, pullStr);
        } else if (st == SIG_DIGITAL_IN) {
          if (strcmp(pullStr, "up") == 0)        pinMode(gpio, INPUT_PULLUP);
          else if (strcmp(pullStr, "down") == 0) pinMode(gpio, INPUT_PULLDOWN);
          else                                   pinMode(gpio, INPUT);
          Serial.printf("[PIN] GPIO %d → DIGITAL input  pull=%s\n", gpio, pullStr);
        } else if (st == SIG_TOUCH) {
          if (!isTouchPin(gpio)) {
            Serial.printf("[PIN] GPIO %d → TOUCH not supported, skipping\n", gpio);
            pinCount--;
            continue;
          }
          // Touch pins don't need pinMode
          Serial.printf("[PIN] GPIO %d → TOUCH input\n", gpio);
        } else if (st == SIG_PWM_READ) {
          pinMode(gpio, INPUT);
          Serial.printf("[PIN] GPIO %d → PWM_READ input\n", gpio);
        }

        if (pinCount >= MAX_PINS) break;
      }
    }
  }

  // Parse OUTPUT pins
  if (!outputsBody.isEmpty()) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, outputsBody) == DeserializationError::Ok) {
      JsonObject outputs = doc["outputs"].as<JsonObject>();
      for (JsonPair kv : outputs) {
        int gpio = atoi(kv.key().c_str());
        if (!isGpioSafe(gpio) || !isOutputCapable(gpio)) continue;

        const char* sigTypeStr = kv.value()["signal_type"] | "DIGITAL";
        float value            = kv.value()["value"]       | 0.0f;
        bool  digState         = kv.value()["digital_state"]| false;
        int   freq             = kv.value()["frequency"]   | 1000;
        SignalType st = parseSignalType(sigTypeStr);
        if (st == SIG_NONE || st == SIG_ADC || st == SIG_TOUCH || st == SIG_PWM_READ) continue;
        // Remap output DIGITAL
        if (st == SIG_DIGITAL_IN) st = SIG_DIGITAL_OUT;

        PinInfo& p = pins[pinCount++];
        p.gpio        = gpio;
        p.signalType  = st;
        p.pullMode    = "none";
        p.configured  = true;
        p.outputValue = value;
        p.digitalState= digState;
        p.frequency   = freq;
        p.pwmChannel  = -1;

        // Configure GPIO
        if (st == SIG_DAC) {
          // DAC pins 25/26 — no pinMode needed
          Serial.printf("[PIN] GPIO %d → DAC output\n", gpio);
        } else if (st == SIG_PWM_OUT) {
          // ESP32 Arduino core v3.x API — no channels, attach directly to pin
          ledcAttach(gpio, freq, 8);  // pin, frequency, 8-bit resolution
          p.pwmChannel = gpio;        // store gpio as the "channel" (v3 uses pin directly)
          Serial.printf("[PIN] GPIO %d → PWM output  freq=%d\n", gpio, freq);
        } else if (st == SIG_DIGITAL_OUT) {
          pinMode(gpio, OUTPUT);
          Serial.printf("[PIN] GPIO %d → DIGITAL output\n", gpio);
        }

        if (pinCount >= MAX_PINS) break;
      }
    }
  }

  Serial.printf("[SYNC] Done — %d pins configured\n", pinCount);
}


// ─── READ SENSORS ─────────────────────────────────────────────
float readPin(const PinInfo& p) {
  switch (p.signalType) {

    case SIG_ADC: {
      // Average 4 samples to reduce noise
      int sum = 0;
      for (int i = 0; i < 4; i++) sum += analogRead(p.gpio);
      int raw = sum / 4;
      return (float)raw / ADC_MAX;   // normalized 0.0–1.0
    }

    case SIG_DIGITAL_IN: {
      return digitalRead(p.gpio) == HIGH ? 1.0f : 0.0f;
    }

    case SIG_TOUCH: {
      uint16_t raw = touchRead(p.gpio);
      // Normalize: low raw = touched, high raw = released
      // Typical: touched ~10–30, released ~60–100
      float normalized = (raw < TOUCH_THRESHOLD) ? 1.0f : 0.0f;
      return normalized;
    }

    case SIG_PWM_READ: {
      // Measure duty cycle using pulseIn
      unsigned long highTime = pulseIn(p.gpio, HIGH, 50000UL);  // 50ms timeout
      unsigned long lowTime  = pulseIn(p.gpio, LOW,  50000UL);
      if (highTime == 0 && lowTime == 0) return 0.0f;
      float period = highTime + lowTime;
      if (period == 0) return 0.0f;
      return (float)highTime / period;  // duty cycle 0.0–1.0
    }

    default:
      return 0.0f;
  }
}

float getRawValue(const PinInfo& p, float normalized) {
  switch (p.signalType) {
    case SIG_ADC:      return normalized * ADC_MAX;   // 0–4095
    case SIG_TOUCH:    return touchRead(p.gpio);       // raw capacitance
    default:           return normalized;
  }
}

const char* getUnit(SignalType st) {
  switch (st) {
    case SIG_ADC:      return "V";
    case SIG_DIGITAL_IN: return "";
    case SIG_TOUCH:    return "raw";
    case SIG_PWM_READ: return "%";
    default:           return "";
  }
}


// ─── PUSH READINGS TO BACKEND ─────────────────────────────────
void pushReadings() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  bool hasData = false;

  for (int i = 0; i < pinCount; i++) {
    PinInfo& p = pins[i];
    // Only push INPUT type pins
    if (p.signalType == SIG_DAC || p.signalType == SIG_PWM_OUT || p.signalType == SIG_DIGITAL_OUT) continue;

    float norm = readPin(p);
    float raw  = getRawValue(p, norm);

    // Build signal type string
    const char* stStr = "DIGITAL";
    if (p.signalType == SIG_ADC)      stStr = "ADC";
    else if (p.signalType == SIG_TOUCH)    stStr = "TOUCH";
    else if (p.signalType == SIG_PWM_READ) stStr = "PWM_READ";

    JsonObject entry = arr.createNestedObject();
    entry["gpio"]        = String(p.gpio);
    entry["signal_type"] = stStr;
    entry["value"]       = norm;
    entry["raw"]         = raw;
    entry["unit"]        = getUnit(p.signalType);
    entry["timestamp"]   = (long long)(millis());

    hasData = true;
  }

  if (!hasData) return;

  String payload;
  serializeJson(doc, payload);
  int code = httpPOST("/api/esp32/readings/batch", payload);

  // Blink activity LED
  if (ACTIVITY_LED >= 0) {
    digitalWrite(ACTIVITY_LED, HIGH);
    delay(10);
    digitalWrite(ACTIVITY_LED, LOW);
  }
}


// ─── PULL OUTPUT COMMANDS FROM BACKEND ────────────────────────
void pullOutputs() {
  String body = httpGET("/api/esp32/outputs");
  if (body.isEmpty()) return;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  JsonObject outputs = doc["outputs"].as<JsonObject>();

  for (int i = 0; i < pinCount; i++) {
    PinInfo& p = pins[i];
    if (p.signalType != SIG_DAC && p.signalType != SIG_PWM_OUT && p.signalType != SIG_DIGITAL_OUT) continue;

    String gpioStr = String(p.gpio);
    if (!outputs.containsKey(gpioStr)) continue;

    JsonObject cmd = outputs[gpioStr];
    float newValue = cmd["value"] | p.outputValue;
    bool  newDig   = cmd["digital_state"] | p.digitalState;
    int   newFreq  = cmd["frequency"] | p.frequency;

    // Apply output
    switch (p.signalType) {

      case SIG_DAC: {
        uint8_t dacVal = (uint8_t)(newValue * DAC_MAX);
        if (p.gpio == 25) dacWrite(25, dacVal);
        if (p.gpio == 26) dacWrite(26, dacVal);
        p.outputValue = newValue;
        break;
      }

      case SIG_PWM_OUT: {
        // Update frequency if changed (v3.x: re-attach with new freq)
        if (newFreq != p.frequency) {
          ledcAttach(p.gpio, newFreq, 8);
          p.frequency = newFreq;
        }
        uint8_t duty = (uint8_t)(newValue * 255);
        ledcWrite(p.gpio, duty);   // v3.x: write directly to pin, not channel
        p.outputValue = newValue;
        break;
      }

      case SIG_DIGITAL_OUT: {
        digitalWrite(p.gpio, newDig ? HIGH : LOW);
        p.digitalState = newDig;
        break;
      }

      default: break;
    }
  }
}


// ─── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n╔════════════════════════════════╗");
  Serial.println("║   ESP32 Pin Simulator v1.0    ║");
  Serial.println("╚════════════════════════════════╝");

  // Activity LED
  if (ACTIVITY_LED >= 0) {
    pinMode(ACTIVITY_LED, OUTPUT);
    digitalWrite(ACTIVITY_LED, LOW);
  }

  // Connect to WiFi
  connectWiFi();

  // Initial config sync from backend
  if (wifiOk) {
    delay(500);
    syncPinConfig();
    lastSync = millis();
  }
}


// ─── LOOP ─────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    Serial.println("[WIFI] Disconnected — reconnecting...");
    connectWiFi();
    return;
  }
  wifiOk = true;

  // Re-sync config every CONFIG_SYNC_MS
  // This picks up any new pins the user configured in the simulator
  if (now - lastSync >= CONFIG_SYNC_MS) {
    syncPinConfig();
    lastSync = now;
  }

  // Push sensor readings (input pins) to backend
  if (now - lastPush >= PUSH_INTERVAL_MS) {
    pushReadings();
    lastPush = now;
  }

  // Pull output commands from backend (output pins)
  if (now - lastPull >= PULL_INTERVAL_MS) {
    pullOutputs();
    lastPull = now;
  }
}
