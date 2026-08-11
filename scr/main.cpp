/*
 * Professional Mobile Phone Hardware Diagnostic Tool
 * ESP32-S3 N16R8, Arduino Core 3.x, LittleFS, AsyncWebServer
 */
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <driver/pulse_cnt.h>
#include <driver/gpio.h>
#include <Wire.h>

// ------------------------- PIN DEFINITIONS -------------------------
#define PIN_UART_RX     18
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define PIN_PWM_OUT     10
#define PIN_CLOCK_IN    11
#define PIN_ADC_VOLT    1
#define PIN_BOOT_MON    4
#define PIN_RESET_MON   5
#define PIN_EN_MON      6
#define PIN_USB_DP      19
#define PIN_USB_DN      20

// ------------------------- CONFIGURATION -------------------------
#define AP_SSID_DEFAULT  "JCID_Diag_S3"
#define AP_PASS_DEFAULT  "admin1234"
#define DNS_PORT         53
#define PWM_FREQ_MIN     1
#define PWM_FREQ_MAX     40000
const int PCNT_LIMIT = 30000;

// ------------------------- GLOBALS & INSTANCES -------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
Preferences preferences;

bool pwmEnabled = false;
uint8_t uartMode = 0; // 0=ASCII, 1=HEX
char ssid[33] = AP_SSID_DEFAULT;
char password[65] = AP_PASS_DEFAULT;
unsigned long last100ms = 0;
unsigned long last1000ms = 0;

pcnt_unit_handle_t pcnt_unit = NULL;
pcnt_channel_handle_t pcnt_chan = NULL;
volatile uint32_t pcnt_overflow_count = 0;

float adcSamples[10] = {0};
int adcSampleIdx = 0;

// FreeRTOS Queue for Thread-Safe ISR Events
struct SeqEvent {
    uint32_t timestamp;
    uint8_t pin;
    bool state;
};
QueueHandle_t eventQueue;

// ------------------------- ISRs -------------------------
bool IRAM_ATTR pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx) {
    pcnt_overflow_count++;
    return false;
}

void IRAM_ATTR handleSeqInterrupt(uint8_t pin) {
    SeqEvent ev;
    ev.timestamp = millis();
    ev.pin = pin;
    ev.state = digitalRead(pin);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(eventQueue, &ev, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

void IRAM_ATTR isrBoot()  { handleSeqInterrupt(PIN_BOOT_MON); }
void IRAM_ATTR isrReset() { handleSeqInterrupt(PIN_RESET_MON); }
void IRAM_ATTR isrEn()    { handleSeqInterrupt(PIN_EN_MON); }

// ------------------------- UTILITIES -------------------------
void htmlEscape(const char* src, char* dst, size_t dstSize) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstSize - 6; i++) {
        switch (src[i]) {
            case '<': strcpy(&dst[j], "&lt;"); j += 4; break;
            case '>': strcpy(&dst[j], "&gt;"); j += 4; break;
            case '&': strcpy(&dst[j], "&amp;"); j += 5; break;
            case '"': strcpy(&dst[j], "&quot;"); j += 6; break;
            case '\'': strcpy(&dst[j], "&#39;"); j += 5; break;
            default: dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

void broadcastTXT(const char* msg) {
    if (ws.count() > 0) {
        ws.textAll(msg);
    }
}

// ------------------------- WEBSOCKET HANDLER -------------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)data, len); // Safe cast, no null-terminator hack
            if (err) return;
            
            const char* cmd = doc["cmd"];
            if (!cmd) return;
            
            if (strcmp(cmd, "baud") == 0) {
                long b = doc["val"] | 115200;
                Serial1.end();
                Serial1.begin(b, SERIAL_8N1, PIN_UART_RX, -1);
            }
            else if (strcmp(cmd, "uart_mode") == 0) {
                uartMode = doc["val"].as<uint8_t>();
            }
            else if (strcmp(cmd, "i2c") == 0) {
                Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
                gpio_pullup_dis((gpio_num_t)PIN_I2C_SDA);
                gpio_pullup_dis((gpio_num_t)PIN_I2C_SCL);
                
                char buf[1024];
                int offset = snprintf(buf, sizeof(buf), "Scanned bus (pull-ups disabled):<br>");
                int count = 0;
                for (byte addr = 1; addr < 127; addr++) {
                    Wire.beginTransmission(addr);
                    if (Wire.endTransmission() == 0) {
                        count++;
                        const char* name = "";
                        switch (addr) {
                            case 0x1A: name = " (LSM6DS3 Accel/Gyro)"; break;
                            case 0x68: name = " (MPU6050/RTC)"; break;
                            case 0x0F: name = " (BQ25890 Charger)"; break;
                            case 0x55: name = " (MAX17047 Fuel)"; break;
                            case 0x36: name = " (MAX17048 Fuel)"; break;
                            case 0x5A: name = " (MLX90614 Temp)"; break;
                            case 0x3C: name = " (OLED Display)"; break;
                            case 0x50: name = " (EEPROM)"; break;
                        }
                        if (offset < sizeof(buf) - 50) {
                            offset += snprintf(buf + offset, sizeof(buf) - offset, "0x%02X%s<br>", addr, name);
                        }
                    }
                }
                if (count == 0) snprintf(buf + offset, sizeof(buf) - offset, "No devices found. Check power & pull-ups.");
                
                pinMode(PIN_I2C_SDA, INPUT); // SAFETY: Restore High-Z
                pinMode(PIN_I2C_SCL, INPUT);
                
                JsonDocument outDoc;
                outDoc["type"] = "i2c";
                outDoc["val"] = buf;
                char outBuf[1500];
                serializeJson(outDoc, outBuf);
                client->text(outBuf);
            }
            else if (strcmp(cmd, "pwm") == 0) {
                int en = doc["en"];
                if (en == 1) {
                    long f = doc["f"];
                    int d = doc["d"];
                    if (f >= PWM_FREQ_MIN && f <= PWM_FREQ_MAX && d >= 0 && d <= 255) {
                        ledcAttach(PIN_PWM_OUT, f, 8);
                        ledcWrite(PIN_PWM_OUT, d);
                        pwmEnabled = true;
                    }
                } else {
                    ledcDetach(PIN_PWM_OUT);
                    pinMode(PIN_PWM_OUT, INPUT); // SAFETY: Instant High-Z
                    pwmEnabled = false;
                }
            }
            else if (strcmp(cmd, "restart") == 0) ESP.restart();
            else if (strcmp(cmd, "factory") == 0) {
                preferences.begin("config", false);
                preferences.clear();
                preferences.end();
                LittleFS.format();
                ESP.restart();
            }
            else if (strcmp(cmd, "settings_save") == 0) {
                const char* newssid = doc["ssid"];
                const char* newpass = doc["pass"];
                if (newssid && newpass) {
                    preferences.begin("config", false);
                    preferences.putString("ssid", newssid);
                    preferences.putString("pass", newpass);
                    preferences.end();
                    ESP.restart();
                }
            }
        }
    }
}

// ------------------------- SETUP -------------------------
void setupHardware() {
    // SAFETY FIRST: Force ALL relevant pins to INPUT (High-Z) immediately
    const uint8_t safePins[] = {PIN_UART_RX, PIN_I2C_SDA, PIN_I2C_SCL, PIN_PWM_OUT, PIN_CLOCK_IN, PIN_ADC_VOLT, PIN_BOOT_MON, PIN_RESET_MON, PIN_EN_MON, PIN_USB_DP, PIN_USB_DN};
    for(int i=0; i<11; i++) {
        pinMode(safePins[i], INPUT);
        gpio_pullup_dis((gpio_num_t)safePins[i]);
        gpio_pulldown_dis((gpio_num_t)safePins[i]);
    }

    eventQueue = xQueueCreate(64, sizeof(SeqEvent));

    attachInterrupt(digitalPinToInterrupt(PIN_BOOT_MON), isrBoot, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_RESET_MON), isrReset, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_EN_MON), isrEn, CHANGE);
    
    analogSetAttenuation(ADC_11db);
    Serial1.begin(115200, SERIAL_8N1, PIN_UART_RX, -1); // TX explicitly disabled (-1)

    // PCNT Setup
    pcnt_unit_config_t unit_config = { .low_limit = -1, .high_limit = PCNT_LIMIT };
    if (pcnt_new_unit(&unit_config, &pcnt_unit) == ESP_OK) {
        pcnt_chan_config_t chan_config = { .edge_gpio_num = PIN_CLOCK_IN, .level_gpio_num = -1 };
        pcnt_new_channel(pcnt_unit, &chan_config, &pcnt_chan);
        pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
        pcnt_event_callbacks_t cbs = { .on_reach = pcnt_on_reach };
        pcnt_unit_register_event_callbacks(pcnt_unit, &cbs, NULL);
        pcnt_unit_add_watch_point(pcnt_unit, PCNT_LIMIT);
        pcnt_unit_enable(pcnt_unit);
        pcnt_unit_clear_count(pcnt_unit);
        pcnt_unit_start(pcnt_unit);
    }
}

void setup() {
    preferences.begin("config", true);
    String savedSsid = preferences.getString("ssid", AP_SSID_DEFAULT);
    String savedPass = preferences.getString("pass", AP_PASS_DEFAULT);
    preferences.end();
    strlcpy(ssid, savedSsid.c_str(), sizeof(ssid));
    strlcpy(password, savedPass.c_str(), sizeof(password));
    
    setupHardware();
    
    if (!LittleFS.begin(true)) {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
            request->send(200, "text/plain", "FS Error - Flash LittleFS Image");
        });
    } else {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    
    server.onNotFound([](AsyncWebServerRequest *request){
        request->redirect("http://" + WiFi.softAPIP().toString() + "/");
    });
    
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
}

// ------------------------- MAIN LOOP -------------------------
void loop() {
    dnsServer.processNextRequest();
    char jsonBuf[512];
    
    // Process Event Queue (Logic Analyzer)
    SeqEvent ev;
    while (xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
        if(ws.count() == 0) continue;
        const char* pinName = (ev.pin == PIN_BOOT_MON) ? "BOOT" : (ev.pin == PIN_RESET_MON) ? "RST" : "EN";
        snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"seq\",\"time\":%u,\"pin\":\"%s\",\"state\":%s}",
                 ev.timestamp, pinName, ev.state ? "true" : "false");
        broadcastTXT(jsonBuf);
    }
    
    // Process UART
    if (Serial1.available()) {
        char uartBuf[256];
        int len = Serial1.readBytesUntil('\n', uartBuf, 255);
        if(ws.count() > 0 && len > 0) {
            uartBuf[len] = 0;
            if (uartMode == 0) {
                char clean[256]; int j = 0;
                for (int i=0; i<len; i++) {
                    if (uartBuf[i] >= 32 && uartBuf[i] <= 126) clean[j++] = uartBuf[i];
                }
                clean[j] = 0;
                char escaped[1024];
                htmlEscape(clean, escaped, sizeof(escaped));
                snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"uart\",\"val\":\"%s\"}", escaped);
            } else {
                char hexStr[768] = "";
                for (int i=0; i<len; i++) {
                    char hx[4]; snprintf(hx, sizeof(hx), "%02X ", (uint8_t)uartBuf[i]);
                    strlcat(hexStr, hx, sizeof(hexStr));
                }
                snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"uart\",\"val\":\"%s\"}", hexStr);
            }
            broadcastTXT(jsonBuf);
        }
    }
    
    // 100ms Tasks (ADC, USB Monitor)
    unsigned long now = millis();
    if (now - last100ms >= 100) {
        last100ms = now;
        
        // Calibrated ADC smoothing
        float voltage = analogReadMilliVolts(PIN_ADC_VOLT) / 1000.0f;
        adcSamples[adcSampleIdx] = voltage;
        adcSampleIdx = (adcSampleIdx + 1) % 10;
        float avg = 0; for (int i=0; i<10; i++) avg += adcSamples[i];
        avg /= 10.0f;
        
        bool dp = digitalRead(PIN_USB_DP);
        bool dn = digitalRead(PIN_USB_DN);
        
        if(ws.count() > 0) {
            snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"v\",\"val\":%.2f,\"avg\":%.2f}", voltage, avg);
            broadcastTXT(jsonBuf);
            snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"usb\",\"dp\":%s,\"dn\":%s}", dp?"true":"false", dn?"true":"false");
            broadcastTXT(jsonBuf);
        }
    }
    
    // 1000ms Tasks (PCNT, System Info)
    if (now - last1000ms >= 1000) {
        last1000ms = now;
        
        if (pcnt_unit != NULL && ws.count() > 0) {
            int count = 0;
            pcnt_unit_get_count(pcnt_unit, &count);
            pcnt_unit_stop(pcnt_unit);
            pcnt_unit_clear_count(pcnt_unit);
            uint32_t overflows = pcnt_overflow_count;
            pcnt_overflow_count = 0;
            pcnt_unit_start(pcnt_unit);
            
            uint32_t hz = (overflows * PCNT_LIMIT) + count;
            snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"clk\",\"val\":%u}", hz);
            broadcastTXT(jsonBuf);
        }
        
        if(ws.count() > 0) {
            snprintf(jsonBuf, sizeof(jsonBuf), "{\"type\":\"sys\",\"heap\":%u,\"psram\":%u,\"temp\":%.1f,\"clients\":%d}",
                     ESP.getFreeHeap(), ESP.getFreePsram(), temperatureRead(), WiFi.softAPgetStationNum());
            broadcastTXT(jsonBuf);
        }
    }
}
