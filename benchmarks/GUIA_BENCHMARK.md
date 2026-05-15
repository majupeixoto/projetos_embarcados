# Guia do Benchmark — Vertente 1 vs Vertente 2

Este guia explica o que foi implementado, por que funciona assim e como executar cada etapa.

---

## O Problema que este Benchmark Demonstra

O ESP32 lê um sensor a **500 Hz** (uma amostra a cada 2 ms). Para enviar esses dados via MQTT, a rede introduz uma latência de **~80 ms** por publicação.

A pergunta central do trabalho é: **como gerenciar essa disparidade sem perder amostras?**

```
Sensor:   ──●──●──●──●──●──●──●──●──  (a cada 2 ms)
Rede MQTT: ──────────────────────────────────●  (a cada ~80 ms)
                                              ↑
                       como absorver essa diferença?
```

---

## As Duas Vertentes

### Vertente 1 — O Anti-Padrão (O(n))

Usa um array dinâmico alocado com `malloc()`. Quando o array está cheio, cada nova amostra exige mover **todos os elementos** uma posição para a esquerda com `memmove()`:

```
Antes: [ a0 | a1 | a2 | a3 | a4 ]   ← array cheio, a0 é o mais antigo
        ↓    ↓    ↓    ↓
Depois: [ a1 | a2 | a3 | a4 | NOVO]  ← memmove deslocou 4 elementos
```

Para N = 20.000 amostras, cada inserção move **80.000 bytes** — puro desperdício de CPU que bloqueia a leitura do sensor.

| N        | Bytes movidos por inserção | Tempo estimado (ESP32 @ 240 MHz) |
|----------|---------------------------|----------------------------------|
| 100      | 400 bytes                 | ~2 µs                            |
| 5.000    | 20.000 bytes              | ~100 µs                          |
| 20.000   | 80.000 bytes              | ~400 µs                          |

### Vertente 2 — O Buffer Circular (O(1))

Usa dois índices (`HEAD` e `TAIL`) que **avançam com aritmética modular**. Nenhum elemento é movido — apenas o índice muda.

```
Estado inicial (CAP = 5, count = 3):

  índice:   0     1     2     3     4
          [ a0  | a1  | a2  |  -  |  -  ]
            ↑                   ↑
           TAIL               HEAD

push(a3):   HEAD avança → índice 3 recebe a3
pop()  :    TAIL avança → lê a0, índice 0 "liberado"
```

Independente de N, `push()` e `pop()` sempre executam:
1. Uma atribuição de elemento
2. Um incremento com módulo (`(idx + 1) % CAP`)

**Complexidade: O(1) para qualquer N.**

---

## Estrutura dos Arquivos

```
projetos_embarcados/
├── include/
│   └── CircularBuffer.h      ← Classe template — pode ser usada em qualquer projeto
├── benchmarks/
│   ├── benchmark.cpp         ← main() do benchmark (substitui src/main.cpp)
│   └── GUIA_BENCHMARK.md     ← este arquivo
└── platformio.ini            ← já tem o ambiente [env:benchmark] configurado
```

---

## Passo a Passo para Executar

### Pré-requisitos

- VS Code com a extensão **PlatformIO** instalada
- ESP32 conectado via USB
- Monitor serial aberto em **115200 baud**

---

### Passo 1 — Verificar o ambiente no `platformio.ini`

Abra [platformio.ini](../platformio.ini) e confirme que o bloco abaixo existe:

```ini
[env:benchmark]
platform    = espressif32
board       = esp32dev
framework   = arduino
build_src_filter = -<src/> +<benchmarks/>
monitor_speed    = 115200
```

A linha `build_src_filter` é o que faz o compilador usar `benchmarks/benchmark.cpp` **em vez de** `src/main.cpp` (firmware VitaLink). Os dois projetos coexistem sem conflito.

---

### Passo 2 — Compilar e gravar o benchmark

Abra o terminal integrado do VS Code (`Ctrl + '`) e execute:

```powershell
# Compila e grava usando o ambiente benchmark
pio run -e benchmark --target upload
```

> Se preferir a interface gráfica: na barra inferior do PlatformIO, selecione o ambiente **benchmark** antes de clicar em Upload.

---

### Passo 3 — Abrir o monitor serial

```powershell
pio device monitor -e benchmark
```

Aguarde ~1,5 segundo para a inicialização. A saída começa assim:

```
╔════════════════════════════════════════════════════════╗
║  Benchmark Telemetria — Vertente 1 vs Vertente 2      ║
║  Colunas CSV:                                          ║
║  TIPO;VERT;N;LAT_MEDIA_US;HEAP_LIVRE_B;HEAP_DELTA_B  ║
╚════════════════════════════════════════════════════════╝
  Heap inicial: 298432 bytes livres
```

---

### Passo 4 — Entender as três seções da saída

#### Seção A — Demo do gargalo síncrono (Vertente 1)

```
# ── DEMO V1: envio síncrono bloqueante ─────────────────
# Taxa alvo  : 1 amostra a cada 2 ms
# Latência   : 80 ms por envio MQTT simulado
DEMO_V1;0;  0.00;82;80
DEMO_V1;1; 10.00;82;80
DEMO_V1;2; 19.87;82;80
```

Leitura: `DEMO_V1 ; nº_amostra ; valor ; tempo_real_ms ; jitter_ms`

O `jitter_ms` de 80 ms mostra que o sensor não consegue amostrar a 500 Hz — fica travado esperando a rede a cada ciclo.

---

#### Seção B — Tabela de latência (dados para o Gráfico de Performance)

```
BENCHMARK;VERTENTE;N;LAT_MEDIA_US;HEAP_LIVRE_BYTES;HEAP_DELTA_BYTES
BENCHMARK;V1;100;   3;298032;400
BENCHMARK;V2;100;   1;298432;0
BENCHMARK;V1;5000; 180;278432;20000
BENCHMARK;V2;5000;  1;298432;0
BENCHMARK;V1;20000;720;218432;80000
BENCHMARK;V2;20000; 1;298432;0
```

| Coluna            | O que significa                                              |
|-------------------|--------------------------------------------------------------|
| `LAT_MEDIA_US`    | Tempo médio por inserção em microssegundos                   |
| `HEAP_LIVRE_BYTES`| Heap disponível após a alocação do buffer V1                 |
| `HEAP_DELTA_BYTES`| Bytes consumidos do heap (V2 deve sempre ser **0**)          |

**Como plotar:** copie essas linhas no Excel ou Google Sheets, use "Dados → Texto para Colunas" com separador `;`, depois crie um gráfico de barras com N no eixo X e `LAT_MEDIA_US` no eixo Y, uma série para cada vertente.

---

#### Seção C — Demo FreeRTOS Produtor–Consumidor (Vertente 2)

Após os benchmarks, as Tasks iniciam e o monitor exibe:

```
MQTT_SEND;100;  0.84;buf=40/512;prod=2003;desc=0
MQTT_SEND;200; -0.84;buf=38/512;prod=4007;desc=0
STATUS;heap=298100;prod=5012;cons=250;desc=0;buf_ocup=42/512
```

| Linha         | Significado                                                  |
|---------------|--------------------------------------------------------------|
| `MQTT_SEND`   | A cada 100 amostras consumidas: nº, valor, ocupação do buffer|
| `STATUS`      | A cada 5 s: saúde geral — heap, produzidas, consumidas, descartadas |

O campo `desc=0` confirma que o Produtor (500 Hz) **não perdeu nenhuma amostra** mesmo com o Consumidor bloqueado por 80 ms a cada envio.

---

### Passo 5 — Voltar ao firmware VitaLink

Basta selecionar o ambiente `esp32dev` e gravar normalmente. O benchmark não altera nada no `src/main.cpp`.

```powershell
pio run -e esp32dev --target upload
```

---

## Como Funciona a Classe `CircularBuffer<T, CAP>`

### Criando uma instância

```cpp
// Tamanho definido em tempo de compilação — sem malloc
CircularBuffer<float, 512> buffer;
```

### Inserindo dados (produtor)

```cpp
float leitura = analogRead(34) * 0.001f;

bool ok = buffer.push(leitura);
// ok == true  → inserido sem perda
// ok == false → buffer estava cheio, dado mais antigo foi descartado (overwrite)
```

### Consumindo dados

```cpp
float valor;
if (buffer.pop(valor)) {
    // valor recebido com sucesso
    Serial.println(valor);
}
```

### Verificando o estado

```cpp
buffer.isEmpty();    // true se não há nada para ler
buffer.isFull();     // true se próximo push() vai sobrescrever
buffer.size();       // quantos elementos há agora
buffer.capacity();   // retorna CAP (constante de compilação)
buffer.clear();      // reseta sem zerar o array — O(1)
```

### Usando em FreeRTOS (com proteção de mutex)

```cpp
// Declaração global (array fica em BSS — sem heap)
static CircularBuffer<float, 256> g_buf;
static SemaphoreHandle_t          g_mutex;

// No setup():
g_mutex = xSemaphoreCreateMutex();

// Task Produtora:
if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
    g_buf.push(lerSensor());
    xSemaphoreGive(g_mutex);
}

// Task Consumidora:
float v;
if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
    bool ok = g_buf.pop(v);
    xSemaphoreGive(g_mutex);
    if (ok) enviarMQTT(v);
}
```

---

## Resumo das Complexidades (para o relatório LaTeX)

| Operação              | Vertente 1 (`SlidingWindow`) | Vertente 2 (`CircularBuffer`) |
|-----------------------|------------------------------|-------------------------------|
| Inserção              | **O(n)** — memmove           | **O(1)** — atribuição + módulo|
| Remoção               | O(1)                         | O(1)                          |
| Consulta por índice   | O(1)                         | O(1)                          |
| Alocação de memória   | Heap (malloc) — fragmentação | BSS (estático) — sem heap     |
| Jitter de inserção    | Cresce com N                 | Constante                     |
| Impacto na rede Wi-Fi | Bloqueia a amostragem        | Absorvido pelo buffer         |

---

## Resolução de Problemas

| Sintoma | Causa provável | Solução |
|---|---|---|
| `ERRO_HEAP_INSUFICIENTE` na linha V1 | Heap < N * 4 bytes | Execute os cenários menores primeiro; reinicie o ESP32 antes do N=20000 |
| Monitor serial vazio | Baud rate errado | Confirme 115200 no monitor |
| Ambiente `benchmark` não aparece | `platformio.ini` não salvo | Salve o arquivo e aguarde o PlatformIO reindexar (~5 s) |
| `desc` crescendo no STATUS | Buffer cheio (Consumidor lento) | Aumente `CAP` em `g_demoBuf` ou reduza `MQTT_LATENCY_MS` |
| Upload falha com `esp32dev` selecionado | Ambiente errado | Selecione `benchmark` antes de gravar |
