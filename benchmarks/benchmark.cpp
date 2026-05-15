/**
 * benchmark.cpp — Vertente 1 vs Vertente 2: Análise Comparativa de Latência
 * ==========================================================================
 * Projeto : Otimização de Telemetria com Buffer Circular — ESP32
 * Autor   : Marlon Silva Ferreira  |  2026
 *
 * COMO USAR
 * ─────────
 * Este arquivo é o main() do projeto de benchmark. Ele NÃO é o firmware
 * VitaLink (src/main.cpp). Para compilar:
 *
 *   Opção A (recomendada): crie um projeto PlatformIO separado e copie
 *     este arquivo para src/main.cpp daquele projeto.
 *
 *   Opção B (temporária): no platformio.ini deste projeto, adicione:
 *     [env:benchmark]
 *     build_src_filter = -<src/> +<benchmarks/>
 *     board = esp32dev
 *     framework = arduino
 *   E compile com: pio run -e benchmark
 *
 * SAÍDA SERIAL (115200 baud)
 * ──────────────────────────
 * Linhas CSV prontas para colar no Excel / Python / Google Sheets:
 *
 *   BENCHMARK;VERTENTE;N;LAT_MEDIA_US;HEAP_LIVRE_BYTES;HEAP_DELTA_BYTES
 *
 * Use os dados para montar o "Gráfico de Performance" do Entregável 2.
 * LAT_MEDIA_US é a latência média por inserção em microssegundos.
 * HEAP_DELTA_BYTES mostra quanto heap a Vertente 1 consome (V2 = 0).
 *
 * ESTRUTURA DO ARQUIVO
 * ────────────────────
 *   1. SlidingWindow  — Vertente 1 (anti-padrão O(n))
 *   2. Globais        — instâncias estáticas do CircularBuffer (sem heap)
 *   3. Benchmarks     — run_benchmark_v1() e run_benchmark_v2()
 *   4. Demo bloqueante— mostra o gargalo síncrono da V1 no Serial
 *   5. Tasks FreeRTOS — Produtor e Consumidor (Vertente 2 assíncrona)
 *   6. setup() / loop()
 */

#include <Arduino.h>
#include <cstring>                  // memmove
#include <cmath>                    // sinf
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "CircularBuffer.h"

// ═══════════════════════════════════════════════════════════════════════════════
// PARÂMETROS DO BENCHMARK
// ═══════════════════════════════════════════════════════════════════════════════

// Número de inserções cronometradas por cenário.
// Manter baixo (100) evita tempos de execução excessivos em N=20000.
// O que importa é a LATÊNCIA POR INSERÇÃO, não o tempo total.
static const uint32_t BENCH_REPS = 100;

// Latência de rede simulada para o demo MQTT (ms)
static const uint32_t MQTT_LATENCY_MS = 80;

// Taxa de amostragem desejada do sensor (ms entre leituras)
static const uint32_t SENSOR_PERIOD_MS = 2;   // 500 Hz

// ═══════════════════════════════════════════════════════════════════════════════
// VERTENTE 1 — ANTI-PADRÃO: JANELA DESLIZANTE COM memmove
// ═══════════════════════════════════════════════════════════════════════════════
//
// Gerencia uma janela de N amostras com alocação dinâmica (malloc) e
// deslocamento linear de todos os elementos a cada nova inserção quando cheia.
//
// ANÁLISE ASSINTÓTICA
// ───────────────────
//   Construtor : O(1) — apenas malloc() + atribuições
//   push()     : O(n) — memmove desloca (N-1) elementos de 4 bytes cada
//                       → para N=20000: move 80.000 bytes por inserção
//   get(i)     : O(1) — acesso direto por índice
//   Destrutor  : O(1) — free()
//
// IMPACTO NO SISTEMA
// ──────────────────
//   • Heap: consome N*sizeof(float) bytes a cada instância
//   • Fragmentação: malloc/free repetidos fragmentam o heap ao longo do tempo
//   • Jitter: memmove de N=20000 elementos ≈ centenas de µs por inserção
//             → a taxa de amostragem cai proporcional ao tamanho de N
// ═══════════════════════════════════════════════════════════════════════════════

class SlidingWindow {
public:

    // Construtor: aloca N floats no HEAP (ponto de fragmentação)
    // Complexidade: O(1) — malloc é aproximadamente constante para tamanhos fixos
    explicit SlidingWindow(uint32_t capacity)
        : _cap(capacity), _count(0), _data(nullptr)
    {
        _data = static_cast<float*>(malloc(_cap * sizeof(float)));
        // Se malloc falhar (heap cheio), _data fica nullptr.
        // Verifique valid() antes de usar.
    }

    // Destrutor: devolve a memória ao heap (não evita a fragmentação já causada)
    ~SlidingWindow() {
        free(_data);
        _data = nullptr;
    }

    // push — Insere novo valor na janela.
    //
    // Caso 1 — janela parcialmente preenchida (_count < _cap):
    //   Apenas atribui na próxima posição → O(1)
    //
    // Caso 2 — janela CHEIA (_count == _cap):
    //   memmove desloca (_cap - 1) elementos uma posição à esquerda → O(n)
    //   Depois atribui o novo valor na última posição → O(1)
    //
    // Complexidade geral (janela cheia, caso relevante para o benchmark): O(n)
    void push(float value) {
        if (!_data) return;

        if (_count < _cap) {
            // Janela ainda crescendo: inserção simples
            // Complexidade: O(1)
            _data[_count++] = value;
        } else {
            // Janela cheia: ANTI-PADRÃO
            // ─────────────────────────────────────────────────────────────
            // memmove(_dst, _src, bytes):
            //   desloca (_cap - 1) elementos, apagando o mais antigo [0]
            //   e abrindo espaço em [_cap - 1] para o novo valor.
            //
            // Para N=20.000 floats: 19.999 * 4 = ~80KB movidos por chamada.
            // Isso consume ~400µs no ESP32 @ 240MHz — puro desperdício de CPU.
            // Complexidade: O(n)
            // ─────────────────────────────────────────────────────────────
            memmove(_data, _data + 1, (_cap - 1) * sizeof(float));
            _data[_cap - 1] = value;
            // _count permanece igual a _cap
        }
    }

    float    get(uint32_t i) const { return (i < _count) ? _data[i] : 0.0f; }
    uint32_t size()          const { return _count; }
    uint32_t capacity()      const { return _cap;   }
    bool     valid()         const { return _data != nullptr; }

    // Impede cópia: copiaria o ponteiro sem duplicar o array (double-free)
    SlidingWindow(const SlidingWindow&)            = delete;
    SlidingWindow& operator=(const SlidingWindow&) = delete;

private:
    uint32_t  _cap;    // capacidade total da janela
    uint32_t  _count;  // elementos válidos (0 a _cap)
    float*    _data;   // ponteiro para o array no HEAP
};

// ═══════════════════════════════════════════════════════════════════════════════
// VERTENTE 2 — INSTÂNCIAS GLOBAIS DO CIRCULAR BUFFER (sem heap)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Declarar as instâncias como globais estáticos coloca os arrays no segmento
// BSS (Block Started by Symbol), que é:
//   • Alocado em tempo de LINK (não de execução) → sem latência de malloc
//   • Inicializado em zero antes de main() pelo startup do ESP32
//   • Não contabilizado no heap livre reportado por ESP.getFreeHeap()
//   • Imune a fragmentação (endereço fixo durante toda a execução)
//
// Uso de memória (BSS):
//   g_buf100  :    100 * 4 =    400 bytes
//   g_buf5k   :  5.000 * 4 =  20.000 bytes (~20 KB)
//   g_buf20k  : 20.000 * 4 =  80.000 bytes (~78 KB)
//   g_demoBuf :    512 * 4 =   2.048 bytes
//   ─────────────────────────────────────────
//   Total     :              ~100 KB  (< 1/3 da DRAM disponível no ESP32)
// ═══════════════════════════════════════════════════════════════════════════════

static CircularBuffer<float, 100>   g_buf100;   // benchmark N=100
static CircularBuffer<float, 5000>  g_buf5k;    // benchmark N=5.000
static CircularBuffer<float, 20000> g_buf20k;   // benchmark N=20.000

// Buffer do demo FreeRTOS Produtor–Consumidor.
// Capacidade 512: absorve ~1s de amostragem a 500Hz enquanto o consumidor
// está ocupado com o "envio MQTT" de 80ms.
static CircularBuffer<float, 512>   g_demoBuf;

// Mutex protege g_demoBuf contra acesso simultâneo das Tasks
static SemaphoreHandle_t            g_demoBufMutex  = nullptr;

// Contadores compartilhados (acesso apenas via tasks — sem race na leitura serial)
static volatile uint32_t g_produzidas  = 0;
static volatile uint32_t g_consumidas  = 0;
static volatile uint32_t g_descartadas = 0;

// ═══════════════════════════════════════════════════════════════════════════════
// SENSOR SIMULADO
// ═══════════════════════════════════════════════════════════════════════════════

// Gera amostras de uma onda senoidal determinística.
// Simula a leitura de um sensor analógico (ex: acelerômetro, temperatura).
// Complexidade: O(1) — sinf() é uma instrução de FPU no ESP32
static float next_sample() {
    static uint32_t idx = 0;
    return sinf(idx++ * 0.01f) * 100.0f;   // amplitude: ±100 (unidade genérica)
}

// ═══════════════════════════════════════════════════════════════════════════════
// BENCHMARK — VERTENTE 1 (SlidingWindow)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Procedimento:
//   1. Aloca SlidingWindow de capacidade N (malloc) — registra heap
//   2. Pré-preenche com N inserções (garante caminho O(n) nos passos seguintes)
//   3. Cronometra BENCH_REPS inserções com micros()
//   4. Imprime linha CSV
//
// A pré-preenchimento é necessário porque enquanto a janela não está cheia,
// push() é O(1) — precisamos medir o pior caso (janela cheia).
// ═══════════════════════════════════════════════════════════════════════════════

static void run_benchmark_v1(uint32_t N) {
    // Guarda o heap antes de qualquer alocação desta função
    uint32_t heapAntes = ESP.getFreeHeap();

    // Guarda para comparar memória necessária
    if (heapAntes < N * sizeof(float) + 4096) {
        Serial.printf("BENCHMARK;V1;%u;ERRO_HEAP_INSUFICIENTE;%u;0\n", N, heapAntes);
        return;
    }

    // ── Alocação dinâmica (O(1), mas fragmenta o heap) ───────────────────
    SlidingWindow sw(N);
    uint32_t heapAposAlloc = ESP.getFreeHeap();
    uint32_t heapDelta     = heapAntes - heapAposAlloc;   // bytes consumidos do heap

    if (!sw.valid()) {
        Serial.printf("BENCHMARK;V1;%u;ERRO_MALLOC;%u;0\n", N, heapAposAlloc);
        return;
    }

    // ── Pré-preenchimento (não cronometrado) ─────────────────────────────
    // Garante que a janela esteja cheia → próximos push() ativam memmove O(n)
    for (uint32_t i = 0; i < N; i++) {
        sw.push(next_sample());
    }

    // ── Medição: BENCH_REPS inserções com janela CHEIA → O(n) cada ───────
    unsigned long t0 = micros();
    for (uint32_t i = 0; i < BENCH_REPS; i++) {
        sw.push(next_sample());
    }
    unsigned long dt = micros() - t0;
    // ─────────────────────────────────────────────────────────────────────

    // SlidingWindow sai de escopo aqui → destrutor chama free()
    // O heap é devolvido, mas a fragmentação permanece no registro do allocator.

    uint32_t latMedia = (uint32_t)(dt / BENCH_REPS);

    // CSV: TIPO;VERTENTE;N;LAT_MEDIA_US;HEAP_LIVRE_BYTES;HEAP_DELTA_BYTES
    Serial.printf("BENCHMARK;V1;%u;%u;%u;%u\n",
                  N, latMedia, heapAposAlloc, heapDelta);
}

// ═══════════════════════════════════════════════════════════════════════════════
// BENCHMARK — VERTENTE 2 (CircularBuffer)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Mesmo procedimento da V1, mas com CircularBuffer estático.
//
// A função é um template para aceitar instâncias de diferentes capacidades
// (g_buf100, g_buf5k, g_buf20k) sem duplicar código.
//
// HEAP_DELTA deve ser sempre 0: o array já está em BSS, nenhuma alocação
// dinâmica ocorre durante push() ou pop().
// ═══════════════════════════════════════════════════════════════════════════════

template<size_t CAP>
static void run_benchmark_v2(CircularBuffer<float, CAP>& buf, uint32_t N) {
    uint32_t heapAntes = ESP.getFreeHeap();

    buf.clear();

    // Pré-preenchimento (igual à V1, para comparação justa)
    for (uint32_t i = 0; i < N; i++) {
        buf.push(next_sample());
    }

    // ── Medição: BENCH_REPS inserções → sempre O(1), independente de N ───
    unsigned long t0 = micros();
    for (uint32_t i = 0; i < BENCH_REPS; i++) {
        buf.push(next_sample());
    }
    unsigned long dt = micros() - t0;
    // ─────────────────────────────────────────────────────────────────────

    uint32_t heapDepois = ESP.getFreeHeap();
    uint32_t latMedia   = (uint32_t)(dt / BENCH_REPS);
    uint32_t heapDelta  = (heapAntes >= heapDepois) ? (heapAntes - heapDepois) : 0;
    // heapDelta deve ser 0: nenhuma alocação dinâmica ocorreu

    Serial.printf("BENCHMARK;V2;%u;%u;%u;%u\n",
                  N, latMedia, heapDepois, heapDelta);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DEMO V1 — GARGALO SÍNCRONO (executa antes do FreeRTOS)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Simula o padrão de coleta+envio SÍNCRONO da Vertente 1:
// cada "publicação MQTT" bloqueia o loop por MQTT_LATENCY_MS.
//
// EFEITO OBSERVÁVEL
// ─────────────────
// Taxa de amostragem desejada: 1 amostra / SENSOR_PERIOD_MS = 500 Hz
// Taxa real                  : 1 amostra / (SENSOR_PERIOD_MS + MQTT_LATENCY_MS)
//                            = 1 amostra / 82 ms ≈ 12 Hz
//
// Jitter por amostra: MQTT_LATENCY_MS - SENSOR_PERIOD_MS = 78 ms
// ═══════════════════════════════════════════════════════════════════════════════

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

        // ── Envio MQTT BLOQUEANTE ─────────────────────────────────────────
        // Em produção seria mqttClient.publish() — a rede bloqueia por dezenas
        // de milissegundos. Durante este delay() NENHUMA leitura ocorre.
        // Taxa real de amostragem cai de 500 Hz para ~12 Hz.
        delay(MQTT_LATENCY_MS);
        // ─────────────────────────────────────────────────────────────────

        unsigned long dt    = (micros() - t0) / 1000;   // ms
        int32_t       jitter = (int32_t)dt - (int32_t)SENSOR_PERIOD_MS;

        Serial.printf("DEMO_V1;%u;%.2f;%lu;%d\n", i, sample, dt, jitter);
    }

    Serial.println("# Conclusão: taxa real ≈ 1 / MQTT_LATENCY, não 1 / SENSOR_PERIOD");
    Serial.println();
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK_PRODUTOR — Core 0, prioridade 3
// ═══════════════════════════════════════════════════════════════════════════════
//
// Lê o sensor a cada SENSOR_PERIOD_MS e empurra a amostra em g_demoBuf.
// Nunca espera pelo consumidor — a latência MQTT é absorvida pelo buffer.
//
// PADRÃO PRODUTOR–CONSUMIDOR
// ──────────────────────────
// O Produtor e o Consumidor rodam em tasks separadas (Cores 0 e 1).
// O CircularBuffer protegido pelo mutex é o canal de comunicação.
// A latência de rede do Consumidor é INVISÍVEL para o Produtor:
//   Produtor: insere em ~1 µs (O(1))
//   Consumidor: drena + envia em 80 ms
// O buffer absorve a diferença (512 slots ≈ 1 segundo de amostras a 500Hz).
//
// Complexidade por iteração: O(1) — push() no CircularBuffer + mutex
// ═══════════════════════════════════════════════════════════════════════════════

static void Task_Produtor(void* pv) {
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (true) {
        float sample = next_sample();

        // Tenta adquirir o mutex por no máximo 1 ms.
        // Se não conseguir (consumidor travado), descarta a amostra.
        if (xSemaphoreTake(g_demoBufMutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            bool ok = g_demoBuf.push(sample);
            xSemaphoreGive(g_demoBufMutex);

            if (ok) {
                g_produzidas++;
            } else {
                // push() com overwrite: buffer cheio → dado mais antigo descartado
                g_descartadas++;
            }
        }

        // vTaskDelayUntil garante período fixo (SENSOR_PERIOD_MS) independente
        // do tempo que push() levou — evita deriva acumulativa no período.
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK_CONSUMIDOR — Core 1, prioridade 1
// ═══════════════════════════════════════════════════════════════════════════════
//
// Drena g_demoBuf e simula o envio MQTT com delay bloqueante.
// A latência de rede bloqueia SOMENTE esta task — o Produtor não é afetado.
//
// Complexidade por iteração: O(1) — pop() no CircularBuffer
// ═══════════════════════════════════════════════════════════════════════════════

static void Task_Consumidor(void* pv) {
    while (true) {
        float sample;
        bool  got = false;

        if (xSemaphoreTake(g_demoBufMutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            got = g_demoBuf.pop(sample);
            xSemaphoreGive(g_demoBufMutex);
        }

        if (got) {
            // ── Simula latência de rede (bloqueante para ESTE CONSUMIDOR) ─
            // O Produtor continua a 500 Hz durante estes 80 ms.
            // O buffer absorve as amostras produzidas enquanto aqui bloqueado.
            vTaskDelay(pdMS_TO_TICKS(MQTT_LATENCY_MS));
            g_consumidas++;

            // Log a cada 100 amostras consumidas (evita saturar o Serial)
            if (g_consumidas % 100 == 0) {
                Serial.printf("MQTT_SEND;%u;%.2f;buf=%u/%u;prod=%u;desc=%u\n",
                              g_consumidas, sample,
                              g_demoBuf.size(), g_demoBuf.capacity(),
                              g_produzidas, g_descartadas);
            }
        } else {
            // Buffer vazio: aguarda 1 ms antes de tentar novamente
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK_STATUS — Core 1, prioridade 1
// ═══════════════════════════════════════════════════════════════════════════════
// Imprime estatísticas de saúde do sistema a cada 5 s.
// Use esses dados para o "Diagnóstico de Memória" do Entregável 3.
// ═══════════════════════════════════════════════════════════════════════════════

static void Task_Status(void* pv) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        Serial.printf("STATUS;heap=%u;prod=%u;cons=%u;desc=%u;buf_ocup=%u/%u\n",
                      ESP.getFreeHeap(),
                      g_produzidas, g_consumidas, g_descartadas,
                      g_demoBuf.size(), g_demoBuf.capacity());
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1500);   // aguarda o monitor serial conectar

    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════════╗");
    Serial.println("║  Benchmark Telemetria — Vertente 1 vs Vertente 2      ║");
    Serial.println("║  Colunas CSV:                                          ║");
    Serial.println("║  TIPO;VERT;N;LAT_MEDIA_US;HEAP_LIVRE_B;HEAP_DELTA_B  ║");
    Serial.println("╚════════════════════════════════════════════════════════╝");
    Serial.printf("  Heap inicial: %u bytes livres\n\n", ESP.getFreeHeap());

    // ── Fase 1: demo do gargalo síncrono (Vertente 1) ─────────────────────
    demo_v1_bloqueante();

    // ── Fase 2: benchmarks de latência de inserção ────────────────────────
    Serial.println("BENCHMARK;VERTENTE;N;LAT_MEDIA_US;HEAP_LIVRE_BYTES;HEAP_DELTA_BYTES");

    // Cenário N = 100
    run_benchmark_v1(100);
    run_benchmark_v2(g_buf100, 100);

    // Cenário N = 5.000
    run_benchmark_v1(5000);
    run_benchmark_v2(g_buf5k, 5000);

    // Cenário N = 20.000
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

    // ── Fase 3: demo FreeRTOS — Produtor–Consumidor (Vertente 2) ──────────
    Serial.println("# ── DEMO V2: Produtor–Consumidor assíncrono ────────────");
    Serial.printf( "# Produtor : 1 amostra a cada %u ms (500 Hz)\n", SENSOR_PERIOD_MS);
    Serial.printf( "# Consumidor: simula MQTT bloqueante de %u ms\n", MQTT_LATENCY_MS);
    Serial.println("# O buffer absorve a diferença — Produtor nunca para.");
    Serial.println("# Monitor: MQTT_SEND;seq;valor;buf=ocup/cap;prod=N;desc=N");
    Serial.println("# Status periódico a cada 5s.");
    Serial.println();

    g_demoBufMutex = xSemaphoreCreateMutex();

    // Produtor: Core 0, prioridade 3 — alta para não perder amostras
    xTaskCreatePinnedToCore(Task_Produtor,   "Produtor",   4096, nullptr, 3, nullptr, 0);

    // Consumidor: Core 1, prioridade 1 — baixa, aceita latência de rede
    xTaskCreatePinnedToCore(Task_Consumidor, "Consumidor", 4096, nullptr, 1, nullptr, 1);

    // Status: Core 1, prioridade 1
    xTaskCreatePinnedToCore(Task_Status,     "Status",     2048, nullptr, 1, nullptr, 1);
}

void loop() {
    vTaskDelay(portMAX_DELAY);   // FreeRTOS gerencia tudo a partir daqui
}
