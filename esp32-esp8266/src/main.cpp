#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "CircularBuffer.h"

enum LedState : uint8_t {
    LED_CONNECTING = 0,
    LED_ONLINE     = 1,
    LED_ALERT      = 2,
    LED_PRE_ALERT  = 3,
    LED_OFF        = 4,
};

#define ALERT_BUTTON  0
#define ALERT_FALL    1

typedef struct {
    uint8_t type;
    float   accel_g;
} AlertEvent_t;

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static CircularBuffer<float, 64> sampleBuf;
static uint32_t samplesProduced   = 0;
static uint32_t samplesPublished  = 0;
static uint32_t lastSamplePublish = 0;

static bool         alertPending  = false;
static AlertEvent_t pendingAlert  = { 0, 0.0f };

static LedState     currentLed    = LED_CONNECTING;

static void setLed(LedState state) {
    currentLed = state;
    digitalWrite(PIN_LED_RED,   (state == LED_ALERT || state == LED_PRE_ALERT) ? HIGH : LOW);
    digitalWrite(PIN_LED_GREEN, (state == LED_ONLINE  || state == LED_PRE_ALERT) ? HIGH : LOW);
    digitalWrite(PIN_LED_BLUE,   state == LED_CONNECTING ? HIGH : LOW);
}

#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_WHO_AM_I     0x75

static const float MPU_ACCEL_SCALE = 16384.0f;
static bool mpuAvailable = false;

static bool mpuInit() {
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_PWR_MGMT_1);
    Wire.write(0x00);
    if (Wire.endTransmission(true) != 0) {
        Serial.println("[MPU] ERRO: sem resposta no barramento I2C.");
        Serial.printf("[MPU] Verifique: SDA=GPIO%d  SCL=GPIO%d  Addr=0x%02X\n",
                      MPU_SDA_PIN, MPU_SCL_PIN, MPU_I2C_ADDR);
        return false;
    }
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_WHO_AM_I);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)1);
    uint8_t id = Wire.read();
    Serial.printf("[MPU] WHO_AM_I = 0x%02X %s\n", id,
        id == 0x70 ? "(MPU-6500 OK)" :
        id == 0x68 ? "(MPU-6050 — compativel)" :
        id == 0x71 ? "(MPU-9250 — compativel)" : "(chip desconhecido)");
    return true;
}

static bool mpuReadAccel(float &ax, float &ay, float &az) {
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_ACCEL_XOUT_H);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return false;
    int16_t rx = ((int16_t)Wire.read() << 8) | Wire.read();
    int16_t ry = ((int16_t)Wire.read() << 8) | Wire.read();
    int16_t rz = ((int16_t)Wire.read() << 8) | Wire.read();
    ax = rx / MPU_ACCEL_SCALE;
    ay = ry / MPU_ACCEL_SCALE;
    az = rz / MPU_ACCEL_SCALE;
    return true;
}

static inline float vecMag(float ax, float ay, float az) {
    return sqrtf(ax*ax + ay*ay + az*az);
}

static enum FallState : uint8_t {
    IDLE, FREEFALL_DETECTED, IMPACT_DETECTED, VERIFYING, PRE_ALERT
} fallState = IDLE;

static uint16_t  ffCount      = 0;
static uint32_t  stateTimer   = 0;
static float     impactG      = 0.0f;

static float     stable_ax = 0, stable_ay = 0, stable_az = 1;
static bool      stableInited = false;
static const float EMA_ALPHA  = 0.05f;

static float     ver_ax = 0, ver_ay = 0, ver_az = 0;
static float     ver_magMin = 0, ver_magMax = 0;
static uint8_t   verSamples   = 0;
static const uint8_t VER_SAMPLE_COUNT = 10;

static bool      btnLastRaw    = HIGH;
static bool      btnLastStable = HIGH;
static uint32_t  btnLastChange = 0;

static uint32_t  preAlertStart = 0;
static uint32_t  lastBlink     = 0;
static bool      blinkOn       = true;

static void publishOnline();

static void processSensors() {
    uint32_t now = millis();

    if (fallState != PRE_ALERT) {
        bool raw = (bool)digitalRead(PIN_BUTTON);
        if (raw != btnLastRaw) { btnLastChange = now; btnLastRaw = raw; }
        if ((now - btnLastChange) > DEBOUNCE_MS) {
            if (raw != btnLastStable) {
                btnLastStable = raw;
                if (btnLastStable == LOW) {
                    Serial.println("[Sensors] BOTAO DE PANICO acionado!");
                    pendingAlert = { ALERT_BUTTON, 0.0f };
                    alertPending = true;
                }
            }
        }
    }

    if (!mpuAvailable) return;

    float ax, ay, az;
    if (!mpuReadAccel(ax, ay, az)) return;
    float mag = vecMag(ax, ay, az);

    sampleBuf.push(mag);
    samplesProduced++;

    switch (fallState) {

        case IDLE:
            if (!stableInited) {
                stable_ax = ax; stable_ay = ay; stable_az = az;
                stableInited = true;
            } else {
                stable_ax += EMA_ALPHA * (ax - stable_ax);
                stable_ay += EMA_ALPHA * (ay - stable_ay);
                stable_az += EMA_ALPHA * (az - stable_az);
            }
            if (mag < FREEFALL_THRESHOLD_G) {
                if (++ffCount >= FREEFALL_SAMPLES) {
                    Serial.printf("[Sensors] Freefall confirmado (mag=%.2fg).\n", mag);
                    stateTimer = now;
                    fallState  = FREEFALL_DETECTED;
                }
            } else {
                ffCount = 0;
            }
            break;

        case FREEFALL_DETECTED:
            if (mag > IMPACT_THRESHOLD_G) {
                impactG    = mag;
                Serial.printf("[Sensors] Impacto detectado (%.2fg).\n", mag);
                stateTimer = now;
                fallState  = IMPACT_DETECTED;
            } else if ((now - stateTimer) > (uint32_t)FALL_WINDOW_MS) {
                Serial.println("[Sensors] Janela expirada sem impacto — descartado.");
                ffCount   = 0;
                fallState = IDLE;
            }
            break;

        case IMPACT_DETECTED:
            if ((now - stateTimer) >= (uint32_t)IMMOBILITY_DELAY_MS) {
                ver_ax = ax; ver_ay = ay; ver_az = az;
                ver_magMin = ver_magMax = mag;
                verSamples = 1;
                fallState  = VERIFYING;
            }
            break;

        case VERIFYING: {
            ver_ax += ax; ver_ay += ay; ver_az += az;
            if (mag < ver_magMin) ver_magMin = mag;
            if (mag > ver_magMax) ver_magMax = mag;
            verSamples++;

            if (verSamples < VER_SAMPLE_COUNT) break;

            float variation = ver_magMax - ver_magMin;
            if (variation > IMMOBILITY_THRESHOLD) {
                Serial.printf("[Sensors] Pessoa em movimento (var=%.2fg) — descartado.\n", variation);
                ffCount   = 0;
                fallState = IDLE;
                break;
            }

            float n  = (float)verSamples;
            float cx = ver_ax/n, cy = ver_ay/n, cz = ver_az/n;
            float dot    = stable_ax*cx + stable_ay*cy + stable_az*cz;
            float mag_s  = vecMag(stable_ax, stable_ay, stable_az);
            float mag_c  = vecMag(cx, cy, cz);
            float cosine = (mag_s > 0.01f && mag_c > 0.01f) ? (dot/(mag_s*mag_c)) : 1.0f;

            if (cosine < 0.707f) {
                Serial.printf("[Sensors] Queda validada (cosT=%.3f, var=%.2fg). PRE-ALERTA.\n",
                              cosine, variation);
                fallState     = PRE_ALERT;
                preAlertStart = now;
                lastBlink     = now;
                blinkOn       = true;
                btnLastRaw    = (bool)digitalRead(PIN_BUTTON);
                btnLastStable = btnLastRaw;
                btnLastChange = now;
                setLed(LED_PRE_ALERT);
            } else {
                Serial.printf("[Sensors] Postura sem mudanca (cosT=%.3f) — descartado.\n", cosine);
                ffCount   = 0;
                fallState = IDLE;
            }
            break;
        }

        case PRE_ALERT: {
            if ((now - lastBlink) >= (uint32_t)PRE_ALERT_BLINK_MS) {
                blinkOn = !blinkOn;
                setLed(blinkOn ? LED_PRE_ALERT : LED_OFF);
                lastBlink = now;
            }

            bool raw = (bool)digitalRead(PIN_BUTTON);
            if (raw != btnLastRaw) { btnLastChange = now; btnLastRaw = raw; }
            if ((now - btnLastChange) > DEBOUNCE_MS && raw != btnLastStable) {
                btnLastStable = raw;
                if (btnLastStable == LOW) {
                    Serial.println("[Sensors] Alarme CANCELADO pelo usuario.");
                    setLed(LED_ONLINE);
                    ffCount   = 0;
                    fallState = IDLE;
                    return;
                }
            }

            if ((now - preAlertStart) >= (uint32_t)CANCEL_WINDOW_MS) {
                Serial.printf("[Sensors] QUEDA CONFIRMADA (%.2fg). Enviando alerta.\n", impactG);
                pendingAlert = { ALERT_FALL, impactG };
                alertPending = true;
                setLed(LED_ONLINE);
                ffCount   = 0;
                fallState = IDLE;
            }
            break;
        }

        default: break;
    }
}

static uint32_t lastWifiRetry = 0;
static uint32_t lastMqttRetry = 0;
static uint32_t alertLedUntil = 0;

static bool connectWifi() {
    Serial.printf("[WiFi] Conectando a \"%s\"...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t) < 10000) {
        delay(500);
        Serial.print('.');
        yield();
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Falha na conexão.");
        return false;
    }
    Serial.printf("[WiFi] IPv4: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

static bool connectMQTT() {
    Serial.printf("[MQTT] Conectando ao broker %s:%d...\n", MQTT_BROKER, MQTT_PORT);
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
        Serial.println("[MQTT] Broker conectado!");
        return true;
    }
    Serial.printf("[MQTT] Falha. rc=%d\n", mqttClient.state());
    return false;
}

static void publishAlert(const AlertEvent_t &ev) {
    JsonDocument doc;
    doc["status"]    = "alert";
    doc["cause"]     = (ev.type == ALERT_FALL) ? "fall" : "manual";
    doc["accel_g"]   = serialized(String(ev.accel_g, 2));
    doc["device_id"] = DEVICE_ID;
    doc["uptime_ms"] = (uint32_t)millis();
    char buf[192];
    serializeJson(doc, buf);
    if (mqttClient.publish(MQTT_TOPIC, buf, /*retained=*/true))
        Serial.printf("[MQTT] Alerta publicado: %s\n", buf);
    else
        Serial.println("[MQTT] Falha ao publicar!");
}

static void publishOnline() {
    JsonDocument doc;
    doc["status"]    = "online";
    doc["cause"]     = "online";
    doc["device_id"] = DEVICE_ID;
    doc["uptime_ms"] = (uint32_t)millis();
    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC, buf, /*retained=*/true);
    Serial.printf("[MQTT] Online: %s\n", buf);
}

static void publishSample() {
    float val;
    if (!sampleBuf.pop(val)) return;
    JsonDocument doc;
    doc["seq"]      = samplesPublished;
    doc["value"]    = roundf(val * 100.0f) / 100.0f;
    doc["buf_size"] = (int)sampleBuf.size();
    doc["buf_cap"]  = 64;
    doc["produced"] = samplesProduced;
    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish("elderly/samples", buf, false);
    samplesPublished++;
}

static void handleNetwork() {
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
        if ((now - lastWifiRetry) >= (uint32_t)WIFI_RECONNECT_DELAY_MS) {
            setLed(LED_CONNECTING);
            connectWifi();
            lastWifiRetry = now;
        }
        return;
    }

    if (!mqttClient.connected()) {
        if ((now - lastMqttRetry) >= (uint32_t)MQTT_RECONNECT_DELAY_MS) {
            setLed(LED_CONNECTING);
            if (connectMQTT()) {
                setLed(LED_ONLINE);
                publishOnline();
            }
            lastMqttRetry = now;
        }
        return;
    }

    mqttClient.loop();

    if (!sampleBuf.isEmpty() && (now - lastSamplePublish) >= 200) {
        lastSamplePublish = now;
        publishSample();
    }

    if (alertPending) {
        alertPending  = false;
        setLed(LED_ALERT);
        alertLedUntil = now + ALERT_LED_DURATION_MS;
        publishAlert(pendingAlert);
    }

    if (alertLedUntil > 0 && now >= alertLedUntil) {
        alertLedUntil = 0;
        setLed(LED_ONLINE);
        publishOnline();
    }
}

static uint32_t lastSensorTick = 0;

void setup() {
    Serial.begin(115200);
    Serial.println("\n[System] VitaLink ESP8266 — inicializando...");

    pinMode(PIN_BUTTON,    INPUT_PULLUP);
    pinMode(PIN_LED_RED,   OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE,  OUTPUT);
    setLed(LED_CONNECTING);

    Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
    Wire.setClock(400000);

    mpuAvailable = mpuInit();
    if (!mpuAvailable)
        Serial.println("[Sensors] MPU ausente — deteccao de queda DESATIVADA.");
    else
        Serial.println("[Sensors] MPU OK — monitoramento de queda ativado.");

    if (connectWifi()) {
        if (connectMQTT()) {
            setLed(LED_ONLINE);
            publishOnline();
        }
    }
}

void loop() {
    uint32_t now = millis();

    if ((now - lastSensorTick) >= (uint32_t)MPU_SAMPLE_MS) {
        lastSensorTick = now;
        processSensors();
    }

    handleNetwork();

    yield();
}
