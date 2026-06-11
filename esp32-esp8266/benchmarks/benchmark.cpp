#include <Arduino.h>
#include <cstring>
#include <cmath>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "CircularBuffer.h"
#include "config.h"

static WiFiClient        g_wifiClient;
static PubSubClient      g_mqttClient(g_wifiClient);
static SemaphoreHandle_t g_mqttMutex   = nullptr;
static volatile bool     g_mqttReady   = false;
static const char* const SAMPLES_TOPIC = "elderly/samples";

static const uint32_t BENCH_REPS      = 100;
static const uint32_t MQTT_LATENCY_MS = 80;
static const uint32_t SENSOR_PERIOD_MS = 2;

class SlidingWindow {
public:

    explicit SlidingWindow(uint32_t capacity)
        : _cap(capacity), _count(0), _data(nullptr)
    {
        _data = static_cast<float*>(malloc(_cap * sizeof(float)));
    }

    ~SlidingWindow() {
        free(_data);
        _data = nullptr;
    }

    void push(float value) {
        if (!_data) return;

        if (_count < _cap) {
            _data[_count++] = value;
        } else {
            memmove(_data, _data + 1, (_cap - 1) * sizeof(float));
            _data[_cap - 1] = value;
        }
    }

    float    get(uint32_t i) const { return (i < _count) ? _data[i] : 0.0f; }
    uint32_t size()          const { return _count; }
    uint32_t capacity()      const { return _cap;   }
    bool     valid()         const { return _data != nullptr; }

    SlidingWindow(const SlidingWindow&)            = delete;
    SlidingWindow& operator=(const SlidingWindow&) = delete;

private:
    uint32_t  _cap;
    uint32_t  _count;
    float*    _data;
};

static CircularBuffer<float, 100>   g_buf100;
static CircularBuffer<float, 5000>  g_buf5k;
static CircularBuffer<float, 20000> g_buf20k;
static CircularBuffer<float, 512>   g_demoBuf;

static SemaphoreHandle_t g_demoBufMutex = nullptr;

static volatile uint32_t g_produzidas  = 0;
static volatile uint32_t g_consumidas  = 0;
static volatile uint32_t g_descartadas = 0;

static float next_sample() {
    static uint32_t idx = 0;
    return sinf(idx++ * 0.01f) * 100.0f;
}

static void run_benchmark_v1(uint32_t N) {
    uint32_t heapAntes = ESP.getFreeHeap();

    if (heapAntes < N * sizeof(float) + 4096) {
        Serial.printf("BENCHMARK;V1;%u;ERRO_HEAP_INSUFICIENTE;%u;0\n", N, heapAntes);
        return;
    }

    SlidingWindow sw(N);
    uint32_t heapAposAlloc = ESP.getFreeHeap();
    uint32_t heapDelta     = heapAntes - heapAposAlloc;

    if (!sw.valid()) {
        Serial.printf("BENCHMARK;V1;%u;ERRO_MALLOC;%u;0\n", N, heapAposAlloc);
        return;
    }

    for (uint32_t i = 0; i < N; i++) {
        sw.push(next_sample());
    }

    unsigned long t0 = micros();
    for (uint32_t i = 0; i < BENCH_REPS; i++) {
        sw.push(next_sample());
    }
    unsigned long dt = micros() - t0;

    uint32_t latMedia = (uint32_t)(dt / BENCH_REPS);

    Serial.printf("BENCHMARK;V1;%u;%u;%u;%u\n",
                  N, latMedia, heapAposAlloc, heapDelta);

    if (g_mqttReady) {
        char jbuf[128];
        snprintf(jbuf, sizeof(jbuf),
            "{\"vertente\":\"V1\",\"n\":%u,\"lat_us\":%u,\"heap_livre\":%u,\"heap_delta\":%u}",
            N, latMedia, heapAposAlloc, heapDelta);
        g_mqttClient.publish("elderly/benchmark", jbuf, false);
    }
}

template<size_t CAP>
static void run_benchmark_v2(CircularBuffer<float, CAP>& buf, uint32_t N) {
    uint32_t heapAntes = ESP.getFreeHeap();

    buf.clear();

    for (uint32_t i = 0; i < N; i++) {
        buf.push(next_sample());
    }

    unsigned long t0 = micros();
    for (uint32_t i = 0; i < BENCH_REPS; i++) {
        buf.push(next_sample());
    }
    unsigned long dt = micros() - t0;

    uint32_t heapDepois = ESP.getFreeHeap();
    uint32_t latMedia   = (uint32_t)(dt / BENCH_REPS);
    uint32_t heapDelta  = (heapAntes >= heapDepois) ? (heapAntes - heapDepois) : 0;

    Serial.printf("BENCHMARK;V2;%u;%u;%u;%u\n",
                  N, latMedia, heapDepois, heapDelta);

    if (g_mqttReady) {
        char jbuf[128];
        snprintf(jbuf, sizeof(jbuf),
            "{\"vertente\":\"V2\",\"n\":%u,\"lat_us\":%u,\"heap_livre\":%u,\"heap_delta\":%u}",
            N, latMedia, heapDepois, heapDelta);
        g_mqttClient.publish("elderly/benchmark", jbuf, false);
    }
}

static void demo_v1_bloqueante() {
    Serial.println("# ── DEMO V1: envio síncrono bloqueante ─────────────────");
    Serial.printf( "# Taxa alvo  : 1 amostra a cada %u ms\n",  SENSOR_PERIOD_MS);
    Serial.printf( "# Latência   : %u ms por envio MQTT simulado\n", MQTT_LATENCY_MS);
    Serial.println("# Resultado  : cada amostra bloqueia o loop inteiro");
    Serial.println("# DEMO_V1;AMOSTRA;VALOR;TEMPO_REAL_MS;JITTER_MS");

    static const uint32_t DEMO_N = 8;

    for (uint32_t i = 0; i < DEMO_N; i++) {
        unsigned long t0 = micros();
        float sample = next_sample();

        delay(MQTT_LATENCY_MS);

        unsigned long dt     = (micros() - t0) / 1000;
        int32_t       jitter = (int32_t)dt - (int32_t)SENSOR_PERIOD_MS;

        Serial.printf("DEMO_V1;%u;%.2f;%lu;%d\n", i, sample, dt, jitter);
    }

    Serial.println("# Conclusão: taxa real ≈ 1 / MQTT_LATENCY, não 1 / SENSOR_PERIOD");
    Serial.println();
}

static void Task_Produtor(void* pv) {
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (true) {
        float sample = next_sample();

        if (xSemaphoreTake(g_demoBufMutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            bool ok = g_demoBuf.push(sample);
            xSemaphoreGive(g_demoBufMutex);

            if (ok) {
                g_produzidas++;
            } else {
                g_descartadas++;
            }
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

static void Task_Network(void* pv) {
    TickType_t xLastWake = xTaskGetTickCount();
    while (true) {
        if (xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (!g_mqttClient.connected()) {
                g_mqttReady = false;
                if (g_mqttClient.connect(MQTT_CLIENT_ID "-bench")) {
                    g_mqttReady = true;
                    Serial.println("[Network] MQTT reconectado.");
                }
            } else {
                g_mqttClient.loop();
                g_mqttReady = true;
            }
            xSemaphoreGive(g_mqttMutex);
        }
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(50));
    }
}

static void Task_Consumidor(void* pv) {
    while (true) {
        float sample;
        bool  got = false;

        if (xSemaphoreTake(g_demoBufMutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            got = g_demoBuf.pop(sample);
            xSemaphoreGive(g_demoBufMutex);
        }

        if (got) {
            if (g_mqttReady &&
                xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(200)) == pdTRUE)
            {
                StaticJsonDocument<128> doc;
                doc["seq"]      = g_consumidas;
                doc["value"]    = round(sample * 100.0f) / 100.0f;
                doc["buf_size"] = (uint32_t)g_demoBuf.size();
                doc["buf_cap"]  = (uint32_t)g_demoBuf.capacity();
                doc["produced"] = g_produzidas;
                char buf[128];
                serializeJson(doc, buf);
                g_mqttClient.publish(SAMPLES_TOPIC, buf, false);
                xSemaphoreGive(g_mqttMutex);
            } else if (!g_mqttReady) {
                g_descartadas++;
            }

            g_consumidas++;

            if (g_consumidas % 100 == 0) {
                Serial.printf("MQTT_SEND;%u;%.2f;buf=%u/%u;prod=%u;desc=%u\n",
                              g_consumidas, sample,
                              g_demoBuf.size(), g_demoBuf.capacity(),
                              g_produzidas, g_descartadas);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static void Task_Status(void* pv) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        Serial.printf("STATUS;heap=%u;prod=%u;cons=%u;desc=%u;buf_ocup=%u/%u\n",
                      ESP.getFreeHeap(),
                      g_produzidas, g_consumidas, g_descartadas,
                      g_demoBuf.size(), g_demoBuf.capacity());
    }
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════════╗");
    Serial.println("║  Benchmark Telemetria — Vertente 1 vs Vertente 2      ║");
    Serial.println("║  Colunas CSV:                                          ║");
    Serial.println("║  TIPO;VERT;N;LAT_MEDIA_US;HEAP_LIVRE_B;HEAP_DELTA_B  ║");
    Serial.println("╚════════════════════════════════════════════════════════╝");
    Serial.printf("  Heap inicial: %u bytes livres\n\n", ESP.getFreeHeap());

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Conectando a \"%s\"", WIFI_SSID);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
        g_mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
        if (g_mqttClient.connect(MQTT_CLIENT_ID "-bench")) {
            g_mqttReady = true;
            Serial.printf("[MQTT] Conectado → publicando em '%s'\n", SAMPLES_TOPIC);
        } else {
            Serial.printf("[MQTT] Falha (rc=%d) — demo roda sem publicação.\n",
                          g_mqttClient.state());
        }
    } else {
        Serial.println("[WiFi] Sem conexão — demo roda sem publicação MQTT.");
    }
    Serial.println();

    demo_v1_bloqueante();

    Serial.println("BENCHMARK;VERTENTE;N;LAT_MEDIA_US;HEAP_LIVRE_BYTES;HEAP_DELTA_BYTES");

    run_benchmark_v1(100);
    run_benchmark_v2(g_buf100, 100);

    run_benchmark_v1(5000);
    run_benchmark_v2(g_buf5k, 5000);

    run_benchmark_v1(20000);
    run_benchmark_v2(g_buf20k, 20000);

    Serial.println();
    Serial.println("# ── Resumo esperado ──────────────────────────────────");
    Serial.println("# V1, N=100  :  LAT_MEDIA baixa (~us)    HEAP_DELTA > 0");
    Serial.println("# V1, N=5k   :  LAT_MEDIA cresce ~50x    HEAP_DELTA > 0");
    Serial.println("# V1, N=20k  :  LAT_MEDIA cresce ~200x   HEAP_DELTA > 0");
    Serial.println("# V2, N=100  :  LAT_MEDIA constante      HEAP_DELTA = 0");
    Serial.println("# V2, N=5k   :  LAT_MEDIA constante      HEAP_DELTA = 0");
    Serial.println("# V2, N=20k  :  LAT_MEDIA constante      HEAP_DELTA = 0");
    Serial.println();

    Serial.println("# ── DEMO V2: Produtor–Consumidor assíncrono ────────────");
    Serial.printf( "# Produtor : 1 amostra a cada %u ms (500 Hz)\n", SENSOR_PERIOD_MS);
    Serial.printf( "# Consumidor: simula MQTT bloqueante de %u ms\n", MQTT_LATENCY_MS);
    Serial.println("# O buffer absorve a diferença — Produtor nunca para.");
    Serial.println("# Monitor: MQTT_SEND;seq;valor;buf=ocup/cap;prod=N;desc=N");
    Serial.println("# Status periódico a cada 5s.");
    Serial.println();

    g_demoBufMutex = xSemaphoreCreateMutex();
    g_mqttMutex    = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(Task_Produtor,   "Produtor",   4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(Task_Consumidor, "Consumidor", 6144, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(Task_Network,    "Network",    4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(Task_Status,     "Status",     2048, nullptr, 1, nullptr, 1);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
