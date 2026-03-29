#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <driver/pcnt.h>

// --- CONFIGURATION ---
#define WIFI_SSID         "DESKTOP-lol"
#define WIFI_PASSWORD     "12345678"
#define BACKEND_HOST      "https://esp32simulator-production.up.railway.app:8080"

#define MAX_PINS          25U
#define DMS_TIMEOUT_MS    10000U
#define ADC_SAMPLES       8U
#define HW_TICK_MS        20U
#define NET_SYNC_MS       5000U

// --- MISRA-COMPLIANT TYPES ---
enum SignalType_t { 
    SIG_NONE = 0, SIG_ADC, SIG_DIG_IN, SIG_TOUCH, 
    SIG_DAC, SIG_PWM_OUT, SIG_DIG_OUT, SIG_FREQ_IN 
};

typedef struct {
    uint8_t       gpio;
    SignalType_t  type;
    float32_t     current_val;
    float32_t     target_val;
    uint32_t      freq;
    uint8_t       safe_state_val;
    uint8_t       pull_mode;     // 0=none, 1=up, 2=down
    uint8_t       ledc_attached; 
    uint8_t       is_active;
} PinDevice_t;

// --- STATIC MEMORY ---
static PinDevice_t g_registry[MAX_PINS];
static volatile uint32_t g_net_heartbeat = 0U;
static volatile uint8_t  g_pin_count = 0U;
static StaticJsonDocument<10240> g_json_doc; // Larger for rich metadata

// --- GPIO VALIDATION (GAP 3) ---
static uint8_t isGpioSafe(uint8_t g) { return (g >= 6U && g <= 11U) ? 0U : (g <= 39U); }
static uint8_t isOutputCapable(uint8_t g) { return (isGpioSafe(g) && g != 34 && g != 35 && g != 36 && g != 39); }
static uint8_t isTouchPin(uint8_t g) {
    const uint8_t t[] = {0, 2, 4, 12, 13, 14, 15, 27, 32, 33};
    for(uint8_t i=0; i<10; i++) if(t[i] == g) return 1U;
    return 0U;
}

// --- METADATA HELPERS (GAP 1) ---
static const char* getTypeStr(SignalType_t t) {
    const char* s[] = {"NONE","ADC","DIGITAL","TOUCH","DAC","PWM","DIGITAL_OUT","FREQ_IN"};
    return s[(uint8_t)t];
}

// --- HARDWARE ABSTRACTION LAYER (HAL) ---
static void hal_configure_pin(PinDevice_t* p) {
    if (p->type == SIG_DIG_IN || p->type == SIG_ADC) {
        if (p->pull_mode == 1U) pinMode(p->gpio, INPUT_PULLUP);
        else if (p->pull_mode == 2U) pinMode(p->gpio, INPUT_PULLDOWN);
        else pinMode(p->gpio, INPUT);
    }
}

static void hal_write_output(const PinDevice_t* p, float32_t val) {
    const uint8_t duty = (uint8_t)(val * 255.0f);
    switch(p->type) {
        case SIG_DIG_OUT: digitalWrite(p->gpio, (val > 0.5f) ? HIGH : LOW); break;
        case SIG_PWM_OUT: ledcWrite(p->gpio, duty); break;
        case SIG_DAC:     if(p->gpio == 25 || p->gpio == 26) dacWrite(p->gpio, duty); break;
        default: break;
    }
}

static float32_t hal_read_input(const PinDevice_t* p) {
    float32_t res = 0.0f;
    switch(p->type) {
        case SIG_ADC: {
            uint32_t acc = 0U;
            for(uint8_t s=0U; s < ADC_SAMPLES; ++s) acc += analogRead(p->gpio);
            res = (float32_t)(acc / ADC_SAMPLES) / 4095.0f;
            break;
        }
        case SIG_DIG_IN: res = (float32_t)digitalRead(p->gpio); break;
        case SIG_TOUCH:  res = (float32_t)touchRead(p->gpio) / 100.0f; break;
        case SIG_FREQ_IN: { int16_t c; pcnt_get_counter_value(PCNT_UNIT_0, &c); pcnt_counter_clear(PCNT_UNIT_0); res = (float32_t)c; break; }
        default: break;
    }
    return res;
}

// --- CORE 1: SAFETY & IO ---
void IRAM_ATTR safety_engine_task(void * pvParameters) {
    esp_task_wdt_add(NULL);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for(;;) {
        esp_task_wdt_reset();
        bool healthy = (millis() - g_net_heartbeat) < DMS_TIMEOUT_MS;
        for (uint8_t i = 0U; i < g_pin_count; ++i) { // Optimized Loop (GAP 6)
            if (g_registry[i].is_active) {
                if (healthy) {
                    g_registry[i].current_val = hal_read_input(&g_registry[i]);
                    hal_write_output(&g_registry[i], g_registry[i].target_val);
                } else {
                    hal_write_output(&g_registry[i], (float32_t)g_registry[i].safe_state_val / 255.0f);
                }
            }
        }
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(HW_TICK_MS));
    }
}

// --- CORE 0: NETWORK ---
void network_engine_task(void * pvParameters) {
    esp_task_wdt_add(NULL);
    WiFiClientSecure sClient; sClient.setInsecure();
    HTTPClient http; http.setReuse(true);
    uint32_t last_sync = 0U;

    for(;;) {
        esp_task_wdt_reset();
        if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        g_net_heartbeat = millis();

        // 1. SYNC CONFIG (GAPS 2, 3, 4, 5)
        if (millis() - last_sync > NET_SYNC_MS) {
            http.begin(sClient, String(BACKEND_HOST) + "/api/esp32/sync");
            if (http.GET() == 200) {
                g_json_doc.clear();
                if (deserializeJson(g_json_doc, http.getStream()) == DeserializationError::Ok) {
                    JsonArray pins = g_json_doc["pins"];
                    uint8_t idx = 0U;
                    for (JsonObject pObj : pins) {
                        uint8_t pin = pObj["gpio"];
                        SignalType_t type = (SignalType_t)pObj["type"];
                        if (!isGpioSafe(pin) || idx >= MAX_PINS) continue;

                        g_registry[idx].gpio = pin;
                        g_registry[idx].type = type;
                        g_registry[idx].pull_mode = pObj["pull"] | 0U;
                        g_registry[idx].target_val = pObj["target"] | 0.0f;
                        g_registry[idx].safe_state_val = pObj["safe"] | 0U;
                        
                        hal_configure_pin(&g_registry[idx]);
                        if (type == SIG_PWM_OUT) { ledcAttach(pin, pObj["freq"] | 5000, 8); g_registry[idx].ledc_attached = 1U; }
                        g_registry[idx].is_active = 1U;
                        idx++;
                    }
                    g_pin_count = idx;
                }
            }
            http.end(); last_sync = millis();
        }

        // 2. RICH METADATA PUSH (GAP 1)
        g_json_doc.clear();
        JsonArray pushArr = g_json_doc.to<JsonArray>();
        for(uint8_t i=0; i<g_pin_count; i++) {
            JsonObject o = pushArr.createNestedObject();
            o["gpio"] = g_registry[i].gpio;
            o["type"] = getTypeStr(g_registry[i].type);
            o["value"] = g_registry[i].current_val;
            o["timestamp"] = millis();
        }
        String payload; serializeJson(g_json_doc, payload);
        http.begin(sClient, String(BACKEND_HOST) + "/api/esp32/readings");
        http.POST(payload);
        http.end();

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void setup() {
    Serial.begin(115200);
    esp_task_wdt_init(30, true);
    memset(g_registry, 0, sizeof(g_registry));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreatePinnedToCore(network_engine_task, "Net", 12288, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(safety_engine_task, "SafeIO", 6144, NULL, 3, NULL, 1);
}

void loop() { vTaskDelete(NULL); }