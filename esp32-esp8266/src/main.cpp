#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
// TIPOS
// ═══════════════════════════════════════════════════════════════════════════════

enum LedState : uint8_t {
    LED_CONNECTING = 0,   // Azul              — conectando Wi-Fi / MQTT
    LED_ONLINE     = 1,   // Verde             — tudo operacional
    LED_ALERT      = 2,   // Vermelho fixo     — alerta confirmado e enviado
    LED_PRE_ALERT  = 3,   // Amarelo piscando  — janela de cancelamento ativa
    LED_OFF        = 4,   // Apagado           — ciclo do piscar no pré-alerta
};

// Causa do alerta — transportada pela Queue entre Tasks
#define ALERT_BUTTON  0   // Botão de pânico pressionado manualmente
#define ALERT_FALL    1   // Queda detectada pelo acelerômetro

// Struct publicada na Queue xAlertQueue (Sensor → MQTT).
// Usar struct em vez de uint8_t permite carregar metadados sem variáveis globais.
typedef struct {
    uint8_t type;       // ALERT_BUTTON ou ALERT_FALL
    float   accel_g;    // magnitude do impacto em 'g' (0.0 para botão de pânico)
} AlertEvent_t;

// ═══════════════════════════════════════════════════════════════════════════════
// PRIMITIVAS FREERTOS
// ═══════════════════════════════════════════════════════════════════════════════

// Queue de capacidade 10: Task_Sensors produz, Task_MQTT consome.
// Tamanho 10 garante que rajadas de eventos não bloqueiem Task_Sensors.
static QueueHandle_t     xAlertQueue;

// Mutex protege o barramento de pinos do LED RGB contra acesso simultâneo
// das duas Tasks (ex: Task_MQTT altera LED enquanto Task_Sensors também tentaria).
static SemaphoreHandle_t xLedMutex;

// ═══════════════════════════════════════════════════════════════════════════════
// CLIENTES DE REDE
// ═══════════════════════════════════════════════════════════════════════════════

static WiFiClient    wifiClient;
static PubSubClient  mqttClient(wifiClient);

// ═══════════════════════════════════════════════════════════════════════════════
// LED RGB
// ═══════════════════════════════════════════════════════════════════════════════

static void setLed(LedState state) {
    if (xSemaphoreTake(xLedMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // LED_PRE_ALERT = Amarelo → Vermelho + Verde acesos simultaneamente
    // LED_OFF       = Apagado → todos os pinos em LOW
    digitalWrite(PIN_LED_RED,   (state == LED_ALERT || state == LED_PRE_ALERT) ? HIGH : LOW);
    digitalWrite(PIN_LED_GREEN, (state == LED_ONLINE  || state == LED_PRE_ALERT) ? HIGH : LOW);
    digitalWrite(PIN_LED_BLUE,   state == LED_CONNECTING ? HIGH : LOW);

    xSemaphoreGive(xLedMutex);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MPU-6500 — COMUNICAÇÃO I2C RAW
// Usamos Wire diretamente (sem biblioteca externa) para ter controle total
// sobre os registradores e facilitar a adaptação para MPU-6050 / MPU-9250.
// ═══════════════════════════════════════════════════════════════════════════════

#define MPU_REG_PWR_MGMT_1   0x6B   // Controle de energia; bit 6 = SLEEP
#define MPU_REG_ACCEL_XOUT_H 0x3B   // Primeiro byte dos dados de aceleração
#define MPU_REG_WHO_AM_I     0x75   // Registro de identidade do chip

// Escala padrão ±2g → 16384 LSB/g.
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ COMO MUDAR A FAIXA DE MEDIÇÃO:                                          │
// │ Escreva no registro ACCEL_CONFIG (0x1C) durante mpuInit():              │
// │   0x00 → ±2g  → divisor 16384.0f  ← padrão, mais preciso em repouso   │
// │   0x08 → ±4g  → divisor  8192.0f                                       │
// │   0x10 → ±8g  → divisor  4096.0f                                       │
// │   0x18 → ±16g → divisor  2048.0f  ← melhor para impactos fortes        │
// │                                                                         │
// │ Para quedas de altura > 1m, considere mudar para ±16g e ajustar        │
// │ IMPACT_THRESHOLD_G em config.h proporcionalmente.                       │
// └─────────────────────────────────────────────────────────────────────────┘
static const float MPU_ACCEL_SCALE = 16384.0f;

static bool mpuInit() {
    // Acorda o sensor: limpa o bit SLEEP no registro PWR_MGMT_1
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_PWR_MGMT_1);
    Wire.write(0x00);
    if (Wire.endTransmission(true) != 0) {
        Serial.println("[MPU] ERRO: sem resposta no barramento I2C.");
        Serial.printf("[MPU] Verifique: SDA=GPIO%d  SCL=GPIO%d  Addr=0x%02X\n",
                      MPU_SDA_PIN, MPU_SCL_PIN, MPU_I2C_ADDR);
        return false;
    }

    // Lê WHO_AM_I para confirmar que o chip é um MPU compatível
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_WHO_AM_I);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)1);
    uint8_t id = Wire.read();
    // Resposta esperada: MPU-6500 → 0x70 | MPU-6050 → 0x68 | MPU-9250 → 0x71
    Serial.printf("[MPU] WHO_AM_I = 0x%02X %s\n", id,
        id == 0x70 ? "(MPU-6500 ✓)" :
        id == 0x68 ? "(MPU-6050 — compatível)" :
        id == 0x71 ? "(MPU-9250 — compatível)" : "(chip desconhecido)");
    return true;
}

// Lê os três eixos do acelerômetro e converte para unidades 'g'.
// Retorna false se o barramento I2C não responder.
static bool mpuReadAccel(float &ax, float &ay, float &az) {
    Wire.beginTransmission(MPU_I2C_ADDR);
    Wire.write(MPU_REG_ACCEL_XOUT_H);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)6);

    if (Wire.available() < 6) return false;

    // Cada eixo ocupa 2 bytes (big-endian): byte alto primeiro
    int16_t rx = ((int16_t)Wire.read() << 8) | Wire.read();
    int16_t ry = ((int16_t)Wire.read() << 8) | Wire.read();
    int16_t rz = ((int16_t)Wire.read() << 8) | Wire.read();

    ax = rx / MPU_ACCEL_SCALE;
    ay = ry / MPU_ACCEL_SCALE;
    az = rz / MPU_ACCEL_SCALE;
    return true;
}

// Magnitude euclidiana do vetor de aceleração.
// Valores de referência com faixa ±2g:
//   ~1.0 g → sensor em repouso (componente da gravidade)
//   ~0.0 g → queda livre ideal (sem contato com superfície)
//   >2.5 g → impacto físico significativo
static inline float vecMag(float ax, float ay, float az) {
    return sqrtf(ax*ax + ay*ay + az*az);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK_SENSORS (Core 0, prioridade 2)
// Monitora o botão de pânico e o acelerômetro MPU-6500.
// Quando detecta um evento validado, empurra AlertEvent_t na xAlertQueue.
//
// MÁQUINA DE ESTADOS — 5 FASES:
//
//   IDLE ──(mag < FREEFALL, N amostras)──▶ FREEFALL_DETECTED
//     ▲                                          │
//     │                              (mag > IMPACT em FALL_WINDOW_MS)
//     │                                          ▼
//     │                                   IMPACT_DETECTED
//     │                                          │
//     │                              (aguarda IMMOBILITY_DELAY_MS)
//     │                                          ▼
//     │                                      VERIFYING
//     │                              ┌─────────────────────┐
//     │◀── postura OK ou em movimento ┤                     ├──▶ PRE_ALERT
//     │    (descartado)               └─────────────────────┘        │
//     │                                                         botão │ timer
//     │◀─────────────────── cancelado ──────────────────────────────┤ │
//     │◀─────── IDLE (sem alerta) ◀─────── cancelado                  │
//                                                                      ▼
//                                                             xAlertQueue → MQTT
// ═══════════════════════════════════════════════════════════════════════════════

void Task_Sensors(void *pvParameters) {

    // ── Debounce do botão ─────────────────────────────────────────────────
    bool       btnLastRaw    = HIGH;
    bool       btnLastStable = HIGH;
    TickType_t btnLastChange = 0;

    // ── Máquina de estados de queda ───────────────────────────────────────
    enum FallState : uint8_t {
        IDLE, FREEFALL_DETECTED, IMPACT_DETECTED, VERIFYING, PRE_ALERT
    };
    FallState  fallState  = IDLE;
    uint16_t   ffCount    = 0;       // amostras consecutivas abaixo do threshold de freefall
    TickType_t stateTimer = 0;       // timestamp da última transição de estado
    float      impactG    = 0.0f;   // magnitude do impacto — carregada até o PRE_ALERT

    // ── Vetor de postura estável (EMA, atualizado só em IDLE) ─────────────
    // Representa a orientação normal do idoso. Congelado durante a queda para
    // servir como referência no produto escalar da fase VERIFYING.
    float stable_ax = 0.0f, stable_ay = 0.0f, stable_az = 1.0f;
    bool  stableInited = false;
    // τ ≈ 390 ms a 50 Hz — acompanha movimentos lentos, ignora choques rápidos
    static const float EMA_ALPHA = 0.05f;

    // ── Acumulador para a fase VERIFYING ──────────────────────────────────
    float   ver_ax = 0, ver_ay = 0, ver_az = 0;
    float   ver_magMin = 0, ver_magMax = 0;
    uint8_t verSamples = 0;
    // 10 amostras * 20 ms = 200 ms de observação após IMMOBILITY_DELAY_MS
    static const uint8_t VER_SAMPLE_COUNT = 10;

    bool mpuOk = mpuInit();
    if (!mpuOk)
        Serial.println("[Sensors] MPU-6500 ausente — detecção de queda DESATIVADA.");
    else
        Serial.println("[Sensors] MPU-6500 OK — monitoramento de queda ativado.");

    while (true) {
        TickType_t now = xTaskGetTickCount();

        // ── 1. Botão de pânico ────────────────────────────────────────────
        // Durante PRE_ALERT o botão é tratado no loop interno — ignora aqui.
        if (fallState != PRE_ALERT) {
            bool raw = (bool)digitalRead(PIN_BUTTON);
            if (raw != btnLastRaw) { btnLastChange = now; btnLastRaw = raw; }
            if ((now - btnLastChange) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
                if (raw != btnLastStable) {
                    btnLastStable = raw;
                    if (btnLastStable == LOW) {
                        Serial.println("[Sensors] BOTAO DE PANICO acionado!");
                        AlertEvent_t ev = { ALERT_BUTTON, 0.0f };
                        xQueueSend(xAlertQueue, &ev, 0);
                    }
                }
            }
        }

        // ── 2. Acelerômetro ───────────────────────────────────────────────
        if (mpuOk) {
            float ax, ay, az;
            if (mpuReadAccel(ax, ay, az)) {
                float mag = vecMag(ax, ay, az);

                // Debug: descomente para calibrar. Em repouso mag deve ser ~1.0 g.
                // Serial.printf("[MPU] ax=%.3f ay=%.3f az=%.3f mag=%.3f g\n",
                //               ax, ay, az, mag);

                switch (fallState) {

                    // ── IDLE ─────────────────────────────────────────────
                    case IDLE:
                        // Atualiza EMA do vetor de postura. Segue movimentos
                        // lentos (caminhar, sentar) mas não reage a choques.
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
                                Serial.printf("[Sensors] Freefall confirmado (mag=%.2fg). "
                                              "Aguardando impacto...\n", mag);
                                stateTimer = now;
                                fallState  = FREEFALL_DETECTED;
                            }
                        } else {
                            ffCount = 0;
                        }
                        break;

                    // ── FREEFALL_DETECTED ─────────────────────────────────
                    case FREEFALL_DETECTED:
                        if (mag > IMPACT_THRESHOLD_G) {
                            impactG = mag;
                            Serial.printf("[Sensors] Impacto detectado (%.2fg). "
                                          "Aguardando imobilidade (%d ms)...\n",
                                          mag, IMMOBILITY_DELAY_MS);
                            stateTimer = now;
                            fallState  = IMPACT_DETECTED;
                        } else if ((now - stateTimer) > pdMS_TO_TICKS(FALL_WINDOW_MS)) {
                            Serial.println("[Sensors] Janela expirada sem impacto — descartado.");
                            ffCount   = 0;
                            fallState = IDLE;
                        }
                        break;

                    // ── IMPACT_DETECTED ───────────────────────────────────
                    // Aguarda o corpo sossegar antes de checar postura.
                    case IMPACT_DETECTED:
                        if ((now - stateTimer) >= pdMS_TO_TICKS(IMMOBILITY_DELAY_MS)) {
                            ver_ax = ax; ver_ay = ay; ver_az = az;
                            ver_magMin = ver_magMax = mag;
                            verSamples = 1;
                            fallState  = VERIFYING;
                        }
                        break;

                    // ── VERIFYING ─────────────────────────────────────────
                    // Dois critérios devem ser atendidos para validar a queda:
                    //  1. Imobilidade: variação de magnitude < IMMOBILITY_THRESHOLD
                    //  2. Postura: cosθ < 0.707 (ângulo > 45° vs. vetor estável)
                    case VERIFYING: {
                        ver_ax += ax; ver_ay += ay; ver_az += az;
                        if (mag < ver_magMin) ver_magMin = mag;
                        if (mag > ver_magMax) ver_magMax = mag;
                        verSamples++;

                        if (verSamples < VER_SAMPLE_COUNT) break;

                        float variation = ver_magMax - ver_magMin;
                        if (variation > IMMOBILITY_THRESHOLD) {
                            Serial.printf("[Sensors] Pessoa em movimento (var=%.2fg) — "
                                          "provavelmente se levantou. Descartado.\n", variation);
                            ffCount   = 0;
                            fallState = IDLE;
                            break;
                        }

                        // Produto escalar: vetor estável vs. vetor atual médio
                        float n     = (float)verSamples;
                        float cx    = ver_ax / n, cy = ver_ay / n, cz = ver_az / n;
                        float dot   = stable_ax*cx + stable_ay*cy + stable_az*cz;
                        float mag_s = vecMag(stable_ax, stable_ay, stable_az);
                        float mag_c = vecMag(cx, cy, cz);
                        float cosine = (mag_s > 0.01f && mag_c > 0.01f)
                                       ? (dot / (mag_s * mag_c)) : 1.0f;

                        if (cosine < 0.707f) {
                            // Ângulo > 45°: corpo provavelmente horizontal → QUEDA VALIDADA
                            Serial.printf("[Sensors] Queda validada (cosθ=%.3f, var=%.2fg). "
                                          "Entrando em PRE-ALERTA (%d s).\n",
                                          cosine, variation, CANCEL_WINDOW_MS / 1000);
                            fallState = PRE_ALERT;
                        } else {
                            Serial.printf("[Sensors] Postura sem mudanca significativa "
                                          "(cosθ=%.3f) — provavelmente sentou. Descartado.\n",
                                          cosine);
                            ffCount   = 0;
                            fallState = IDLE;
                        }
                        break;
                    }

                    // ── PRE_ALERT ─────────────────────────────────────────
                    // Loop interno com LED amarelo piscando por CANCEL_WINDOW_MS.
                    // Botão pressionado → cancela (sem enviar ao MQTT).
                    // Tempo esgotado   → envia alerta de queda ao MQTT.
                    case PRE_ALERT: {
                        TickType_t preStart  = xTaskGetTickCount();
                        bool       cancelled = false;
                        bool       ledOn     = true;
                        TickType_t lastBlink = preStart;

                        // Reinicia debounce — estado do botão pode ter mudado
                        btnLastRaw    = (bool)digitalRead(PIN_BUTTON);
                        btnLastStable = btnLastRaw;
                        btnLastChange = preStart;

                        setLed(LED_PRE_ALERT);

                        while ((xTaskGetTickCount() - preStart) < pdMS_TO_TICKS(CANCEL_WINDOW_MS)) {
                            TickType_t t = xTaskGetTickCount();

                            // Piscar amarelo a cada PRE_ALERT_BLINK_MS
                            if ((t - lastBlink) >= pdMS_TO_TICKS(PRE_ALERT_BLINK_MS)) {
                                ledOn = !ledOn;
                                setLed(ledOn ? LED_PRE_ALERT : LED_OFF);
                                lastBlink = t;
                            }

                            // Polling do botão com debounce
                            bool raw = (bool)digitalRead(PIN_BUTTON);
                            if (raw != btnLastRaw) { btnLastChange = t; btnLastRaw = raw; }
                            if ((t - btnLastChange) > pdMS_TO_TICKS(DEBOUNCE_MS) &&
                                raw != btnLastStable)
                            {
                                btnLastStable = raw;
                                if (btnLastStable == LOW) {
                                    cancelled = true;
                                    break;
                                }
                            }

                            vTaskDelay(pdMS_TO_TICKS(MPU_SAMPLE_MS));
                        }

                        if (cancelled) {
                            Serial.println("[Sensors] Alarme CANCELADO — falso positivo "
                                           "confirmado pelo usuario.");
                        } else {
                            Serial.printf("[Sensors] Janela expirada. QUEDA CONFIRMADA "
                                          "(%.2fg). Enviando alerta MQTT.\n", impactG);
                            AlertEvent_t ev = { ALERT_FALL, impactG };
                            xQueueSend(xAlertQueue, &ev, 0);
                        }

                        // Volta ao verde — Task_MQTT sobrescreverá com vermelho
                        // por ALERT_LED_DURATION_MS se o alerta foi confirmado.
                        setLed(LED_ONLINE);
                        ffCount   = 0;
                        fallState = IDLE;
                        break;
                    }

                    default: break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MPU_SAMPLE_MS));   // 50 Hz
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPERS DE REDE
// ═══════════════════════════════════════════════════════════════════════════════

static bool connectWifi() {
    Serial.printf("[WiFi] Conectando a \"%s\"...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Falha na conexão.");
        return false;
    }

    Serial.printf("[WiFi] IPv4: %s\n", WiFi.localIP().toString().c_str());

#if ENABLE_IPV6
    // Ativa SLAAC (Stateless Address Autoconfiguration) para obter endereço IPv6.
    // O roteador anuncia o prefixo via Router Advertisement; o ESP32 gera o
    // sufixo EUI-64 a partir do endereço MAC → endereço global único sem DHCP6.
    WiFi.enableIPv6();
    vTaskDelay(pdMS_TO_TICKS(1000));   // aguarda o SO configurar o endereço
    Serial.printf("[WiFi] IPv6: %s\n", WiFi.localIPv6().toString().c_str());
#endif

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

// Publica um alerta com os metadados da causa (botão ou queda).
// JSON resultante: {"status":"alert","cause":"fall","accel_g":3.42,"device_id":"...","uptime_ms":...}
static void publishAlert(const AlertEvent_t &ev) {
    StaticJsonDocument<192> doc;
    doc["status"]    = "alert";
    doc["cause"]     = (ev.type == ALERT_FALL) ? "fall" : "manual";
    doc["accel_g"]   = serialized(String(ev.accel_g, 2));
    doc["device_id"] = DEVICE_ID;
    doc["uptime_ms"] = (uint32_t)millis();

    char buf[192];
    serializeJson(doc, buf);

    if (mqttClient.publish(MQTT_TOPIC, buf, /*retained=*/true)) {
        Serial.printf("[MQTT] Alerta publicado: %s\n", buf);
    } else {
        Serial.println("[MQTT] Falha ao publicar alerta!");
    }
}

static void publishOnline() {
    StaticJsonDocument<128> doc;
    doc["status"]    = "online";
    doc["cause"]     = "online";
    doc["device_id"] = DEVICE_ID;
    doc["uptime_ms"] = (uint32_t)millis();

    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC, buf, false);
    Serial.printf("[MQTT] Online: %s\n", buf);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK_MQTT (Core 1, prioridade 1)
// Gerencia reconexão Wi-Fi/MQTT e despacha eventos recebidos da xAlertQueue.
// Roda no Core 1 para não disputar CPU com Task_Sensors durante leituras I2C.
// ═══════════════════════════════════════════════════════════════════════════════

void Task_MQTT(void *pvParameters) {
    AlertEvent_t ev;

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
            publishOnline();   // anuncia presença ao (re)conectar
        }

        mqttClient.loop();

        // ── Consome alertas da Queue ───────────────────────────────────────
        // xQueueReceive com timeout 0 é não-bloqueante: retorna imediatamente
        // se a fila estiver vazia, evitando travar o loop de reconexão MQTT.
        if (xQueueReceive(xAlertQueue, &ev, 0) == pdTRUE) {
            setLed(LED_ALERT);
            publishAlert(ev);
            vTaskDelay(pdMS_TO_TICKS(ALERT_LED_DURATION_MS));
            setLed(LED_ONLINE);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("\n[System] VitaLink — inicializando...");

    // GPIO
    pinMode(PIN_BUTTON,    INPUT_PULLUP);
    pinMode(PIN_LED_RED,   OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE,  OUTPUT);

    // I2C — inicializa antes de criar as Tasks para que mpuInit() funcione
    Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
    Wire.setClock(400000);   // 400 kHz (Fast Mode) — reduz latência da leitura

    // FreeRTOS — Queue agora transporta AlertEvent_t (não mais uint8_t)
    xAlertQueue = xQueueCreate(10, sizeof(AlertEvent_t));
    xLedMutex   = xSemaphoreCreateMutex();

    setLed(LED_CONNECTING);

    // Task_Sensors: Core 0, prioridade 2 (maior), stack 4096
    // Stack maior que antes por causa do sqrtf e das variáveis de estado da queda
    xTaskCreatePinnedToCore(Task_Sensors, "Task_Sensors",
                            4096, nullptr, 2, nullptr, 0);

    // Task_MQTT: Core 1, prioridade 1, stack 8192 (operações de rede são pesadas)
    xTaskCreatePinnedToCore(Task_MQTT, "Task_MQTT",
                            8192, nullptr, 1, nullptr, 1);

    Serial.println("[System] Tasks iniciadas. FreeRTOS assumindo controle.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);   // nada acontece aqui — FreeRTOS gerencia tudo
}
