#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "config.h"

// ─── Tipos ───────────────────────────────────────────────────────────────────

enum LedState : uint8_t {
    LED_CONNECTING = 0,  // Azul  — tentando conectar
    LED_ONLINE     = 1,  // Verde — Wi-Fi + MQTT ok
    LED_ALERT      = 2,  // Vermelho — botão de pânico pressionado
};

// ─── Primitivas FreeRTOS ──────────────────────────────────────────────────────

static QueueHandle_t     xAlertQueue;   // Sensor → MQTT (sinaliza alerta)
static SemaphoreHandle_t xLedMutex;     // Proteção de acesso ao LED RGB

// ─── Clientes de rede ────────────────────────────────────────────────────────

static WiFiClient    wifiClient;
static PubSubClient  mqttClient(wifiClient);

// ─── Helpers de LED ──────────────────────────────────────────────────────────

static void setLed(LedState state) {
    if (xSemaphoreTake(xLedMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    digitalWrite(PIN_LED_RED,   state == LED_ALERT      ? HIGH : LOW);
    digitalWrite(PIN_LED_GREEN, state == LED_ONLINE      ? HIGH : LOW);
    digitalWrite(PIN_LED_BLUE,  state == LED_CONNECTING  ? HIGH : LOW);

    xSemaphoreGive(xLedMutex);
}

// ─── Task_Sensors ─────────────────────────────────────────────────────────────
// Polling com debounce de software. Detecta borda de descida (pull-up ativo).

void Task_Sensors(void *pvParameters) {
    bool lastRaw    = HIGH;
    bool lastStable = HIGH;
    TickType_t lastChange = 0;

    while (true) {
        bool raw = (bool)digitalRead(PIN_BUTTON);

        if (raw != lastRaw) {
            lastChange = xTaskGetTickCount();
            lastRaw = raw;
        }

        if ((xTaskGetTickCount() - lastChange) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
            if (raw != lastStable) {
                lastStable = raw;

                if (lastStable == LOW) {   // borda de descida = botão pressionado
                    Serial.println("[Sensors] Panic button pressed!");
                    uint8_t sig = 1;
                    xQueueSend(xAlertQueue, &sig, pdMS_TO_TICKS(100));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));   // poll a cada 10 ms
    }
}

// ─── Helpers de rede ─────────────────────────────────────────────────────────

static bool connectWifi() {
    Serial.printf("[WiFi] Conectando a \"%s\"...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("[WiFi] Falha na conexão.");
    return false;
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

static void publishPayload(const char *status, bool retained = false) {
    StaticJsonDocument<128> doc;
    doc["status"]    = status;
    doc["device_id"] = DEVICE_ID;
    doc["uptime_ms"] = (uint32_t)millis();

    char buf[128];
    serializeJson(doc, buf);

    if (mqttClient.publish(MQTT_TOPIC, buf, retained)) {
        Serial.printf("[MQTT] Publicado: %s\n", buf);
    } else {
        Serial.println("[MQTT] Falha ao publicar!");
    }
}

// ─── Task_MQTT ────────────────────────────────────────────────────────────────
// Gerencia reconexão Wi-Fi/MQTT e despacha alertas e heartbeats.

void Task_MQTT(void *pvParameters) {
    uint8_t    alertSig;
    TickType_t lastHeartbeat = 0;

    while (true) {
        // ── Garante Wi-Fi ──────────────────────────────────────────────────
        if (WiFi.status() != WL_CONNECTED) {
            setLed(LED_CONNECTING);
            if (!connectWifi()) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
                continue;
            }
        }

        // ── Garante MQTT ───────────────────────────────────────────────────
        if (!mqttClient.connected()) {
            setLed(LED_CONNECTING);
            if (!connectMQTT()) {
                vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
                continue;
            }
            setLed(LED_ONLINE);
            publishPayload("online");
        }

        mqttClient.loop();

        // ── Processa alerta do sensor ──────────────────────────────────────
        if (xQueueReceive(xAlertQueue, &alertSig, 0) == pdTRUE) {
            setLed(LED_ALERT);
            publishPayload("alert", /*retained=*/true);
            vTaskDelay(pdMS_TO_TICKS(ALERT_LED_DURATION_MS));
            setLed(LED_ONLINE);
        }

        // ── Heartbeat periódico ────────────────────────────────────────────
        TickType_t now = xTaskGetTickCount();
        if ((now - lastHeartbeat) >= pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS)) {
            publishPayload("online");
            lastHeartbeat = now;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── Setup / Loop ─────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n[System] Pulseira de Assistência ao Idoso — inicializando...");

    pinMode(PIN_BUTTON,    INPUT_PULLUP);
    pinMode(PIN_LED_RED,   OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE,  OUTPUT);

    xAlertQueue = xQueueCreate(5, sizeof(uint8_t));
    xLedMutex   = xSemaphoreCreateMutex();

    setLed(LED_CONNECTING);

    // Task_Sensors no Core 0 — prioridade alta para não perder o botão
    xTaskCreatePinnedToCore(Task_Sensors, "Task_Sensors",
                            2048, nullptr, 2, nullptr, 0);

    // Task_MQTT no Core 1 — operações de rede são bloqueantes
    xTaskCreatePinnedToCore(Task_MQTT, "Task_MQTT",
                            8192, nullptr, 1, nullptr, 1);

    Serial.println("[System] Tasks iniciadas.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);   // FreeRTOS assume o controle
}
