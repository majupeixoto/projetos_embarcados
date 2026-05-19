# VitaLink — Pulseira de Assistência ao Idoso

Protótipo de pulseira com **botão de pânico** e **detecção inteligente de quedas** via acelerômetro MPU-6500. O dispositivo (ESP32 + FreeRTOS) usa uma máquina de estados de 5 fases — incluindo verificação de postura por produto escalar e janela de cancelamento de falsos positivos — para publicar alertas via MQTT, exibidos em um dashboard web em tempo real.

---

## Arquitetura do Sistema

```
┌──────────────────────┐        MQTT         ┌──────────────────┐      WebSocket      ┌───────────────────┐
│  ESP32 / Simulador   │ ───────────────────▶│ Mosquitto Broker │ ◀──────────────────▶│ Dashboard (Flask) │
│  Botão + MPU-6500    │  elderly/alerts     │  localhost:1883  │                     │  localhost:5000   │
└──────────────────────┘                     └──────────────────┘                     └───────────────────┘
```

**Fluxo:** Botão pressionado ou queda detectada → ESP32 publica JSON no tópico MQTT → Flask recebe e repassa via Socket.IO → Dashboard atualiza em tempo real.

**FreeRTOS no ESP32:**
```
Core 0 — Task_Sensors (prioridade 2)       Core 1 — Task_MQTT (prioridade 1)
  ├─ Polling do botão (debounce 50ms)         ├─ Reconexão Wi-Fi automática
  ├─ Leitura MPU-6500 a 50 Hz (I2C)          ├─ Reconexão MQTT automática
  └─ Máquina de estados de queda (5 fases)   ├─ Consome xAlertQueue
       ├─ IDLE → FREEFALL_DETECTED            └─ Heartbeat a cada 30s
       ├─ FREEFALL_DETECTED → IMPACT_DETECTED
       ├─ IMPACT_DETECTED → VERIFYING
       ├─ VERIFYING → PRE_ALERT (ou IDLE se descartado)
       └─ PRE_ALERT ──▶ xAlertQueue (AlertEvent_t)
                    └── cancelado pelo botão → IDLE
```

---

## Estrutura de Arquivos

```
projetos_embarcados/
├── esp32-esp8266/
│   ├── include/
│   │   ├── config.h              # Wi-Fi, broker MQTT, GPIOs e thresholds do MPU-6500
│   │   └── CircularBuffer.h      # Buffer circular genérico O(1) sem heap
│   ├── src/
│   │   └── main.cpp              # Firmware ESP32 — FreeRTOS + MPU-6500 + MQTT
│   ├── benchmarks/
│   │   └── benchmark.cpp         # Benchmark SlidingWindow vs CircularBuffer
│   └── platformio.ini            # Configuração de build (env:esp32dev + env:benchmark)
├── applications/
│   ├── simulation/
│   │   └── mock_esp32.py         # Simulador Python: botão de pânico + queda física
│   └── dashboard/
│       ├── app.py                # Backend Flask + Socket.IO + paho-mqtt
│       ├── requirements.txt      # Dependências Python do dashboard
│       └── templates/
│           └── index.html        # Interface web do dashboard
├── docs/                         # Relatório PDF, imagens e referências
├── schematics/                   # Esquemas de ligação (Fritzing / KiCad)
├── REQ_ALGORITMOS.md             # Requisitos do componente de algoritmos
├── REQ_EMBARCADOS.md             # Requisitos do componente embarcados
└── README.md                     # Este arquivo
```

---

## Pré-requisitos

| Ferramenta | Finalidade | Instalação |
|---|---|---|
| [Mosquitto](https://mosquitto.org/download/) | Broker MQTT local | `winget install EclipseFoundation.Mosquitto` |
| Python 3.10+ | Dashboard e simulador | python.org |
| PlatformIO | Compilar e gravar firmware | Extensão VS Code |

---

## Parte 1 — Rodando sem Hardware (Simulação)

Ideal para desenvolver e testar o dashboard antes de ter o ESP32 em mãos.

### Passo 1 — Iniciar o Broker MQTT

```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -v
```

> Deixe este terminal aberto. O `-v` ativa logs verbosos para acompanhar as mensagens.

### Passo 2 — Iniciar o Dashboard

Em um **segundo terminal**:

```powershell
cd projetos_embarcados/applications/dashboard
pip install -r requirements.txt
python app.py
```

Acesse **http://localhost:5000** no navegador.

### Passo 3 — Iniciar o Simulador

Em um **terceiro terminal**:

```powershell
cd projetos_embarcados/applications/simulation
python mock_esp32.py
```

---

## Lógica de Detecção de Quedas

A detecção passa por **5 fases sequenciais** antes de enviar qualquer alerta, reduzindo drasticamente os falsos positivos causados por sentar, agachar ou movimentos bruscos normais.

### Fase 1 — Queda Livre (`FREEFALL_DETECTED`)

O firmware monitora continuamente a magnitude do vetor de aceleração:

$$A_m = \sqrt{a_x^2 + a_y^2 + a_z^2}$$

Quando $A_m$ cai abaixo de `FREEFALL_THRESHOLD_G` (0.50 g) por 5 amostras consecutivas (~100ms), a queda livre é confirmada. Em IDLE, o firmware também mantém uma **média exponencial** (EMA, α=0.05, τ≈390ms) dos 3 eixos — o "vetor de postura estável" usado na Fase 4.

### Fase 2 — Impacto (`IMPACT_DETECTED`)

Se $A_m$ superar `IMPACT_THRESHOLD_G` (2.80 g) dentro de 500ms da queda livre, o impacto é confirmado. Caso contrário, o evento é descartado (ex: pular de um degrau baixo).

### Fase 3 — Imobilidade (`VERIFYING`, parte 1)

O firmware aguarda `IMMOBILITY_DELAY_MS` (3s) para o corpo sossegar e então coleta 10 amostras (~200ms). Se a variação de magnitude entre elas for maior que `IMMOBILITY_THRESHOLD` (0.15 g), a pessoa está em movimento — provavelmente se levantando — e o evento é descartado.

### Fase 4 — Verificação de Postura (`VERIFYING`, parte 2)

Com a pessoa imóvel confirmada, o firmware calcula o produto escalar entre o vetor estável (pré-queda) e o vetor atual:

$$\cos\theta = \frac{\vec{a}_{estavel} \cdot \vec{a}_{atual}}{|\vec{a}_{estavel}| \cdot |\vec{a}_{atual}|}$$

- **cos θ < 0.707** (ângulo > 45°): orientação mudou significativamente → corpo provavelmente horizontal → **queda validada**
- **cos θ ≥ 0.707**: postura similar à original → provavelmente sentou/agachou → **descartado**

### Fase 5 — Pré-Alerta e Cancelamento (`PRE_ALERT`)

Queda validada ativa o LED **amarelo piscante** e um timer de `CANCEL_WINDOW_MS` (15s). Durante este período:

- O idoso pressiona o botão → alarme cancelado, LED volta ao verde, **nenhum JSON é publicado**
- O timer expira sem ação → alerta `{"status":"alert","cause":"fall"}` publicado no MQTT, LED vermelho por 3s

> **Botão de pânico manual (ENTER / `cause: manual`):** dispara alerta **imediatamente**, sem passar pelas 5 fases e sem pré-alerta. Útil quando o idoso percebe que vai cair antes de cair.

---

## Simulando Eventos

### Comandos disponíveis no simulador

| Comando | Ação |
|---|---|
| `ENTER` | Botão de pânico — alerta imediato (`cause: manual`), sem pré-alerta |
| `f` + `ENTER` | Simular queda completa — 4 fases + janela de cancelamento de 15s |
| `c` + `ENTER` | Cancelar o pré-alerta ativo (simula o idoso pressionando o botão) |
| `s` + `ENTER` | Simular sentar/abaixar — mostra por que o sistema **não** dispara alarme |
| `h` + `ENTER` | Heartbeat manual |
| `q` + `ENTER` | Encerrar |

### Como simular uma queda

1. Com o simulador rodando, digite `f` e pressione **ENTER**
2. O terminal mostrará as **4 fases** da detecção, seguidas do pré-alerta:

```
  ↓ QUEDA LIVRE    mag=0.18g  (threshold: <0.50g)  duração≈210ms
  💥 IMPACTO        mag=4.73g  (threshold: >2.80g)
  ⏳ IMOBILIDADE    aguardando 3s para o corpo sossegar...
  📐 POSTURA        cosθ=0.423  (< 0.707 → ângulo > 45°)  var=0.05g  → QUEDA VALIDADA

  ⚠  PRÉ-ALERTA ATIVO — impacto=4.73g
     Digite 'c' + Enter para CANCELAR o alarme.

  ⏱  15s restantes...
```

3. **Cenário A — deixar o tempo expirar (queda real):**
   - Aguarde os 15 segundos sem digitar nada
   - O simulador publica `{"status":"alert","cause":"fall","accel_g":4.73,...}`
   - No dashboard, o log exibe **"Queda Detectada"** com o valor do impacto em `g`

4. **Cenário B — cancelar o alarme (falso positivo):**
   - Durante a contagem regressiva, digite `c` e pressione **ENTER**
   - O simulador exibe `✓ Alarme CANCELADO — falso positivo confirmado`
   - Nenhuma mensagem MQTT de alerta é publicada; o dispositivo volta a `online`

### Como simular um sentar (sem alarme)

Digite `s` e pressione **ENTER**. O simulador exibe o perfil de aceleração das fases do movimento, mostrando que a magnitude nunca cai abaixo de `0.50g` (sem queda livre) nem sobe acima de `2.80g` (sem impacto) — portanto o firmware permanece em **IDLE**.

### Modo automático (stress test do dashboard)

```powershell
# Queda aleatória a cada ~30 segundos (jitter de ±20%)
python mock_esp32.py --auto-fall 30

# Combinando com outro broker e device ID
python mock_esp32.py --broker 192.168.1.100 --device esp32_02 --auto-fall 60
```

### Formato das mensagens MQTT

```json
// Queda detectada pelo acelerômetro
{ "status": "alert",  "cause": "fall",      "accel_g": 4.73, "device_id": "esp32_01", "uptime_ms": 12400 }

// Botão de pânico pressionado manualmente
{ "status": "alert",  "cause": "manual",    "accel_g": 0.0,  "device_id": "esp32_01", "uptime_ms": 15800 }

// Heartbeat periódico (30s)
{ "status": "online", "cause": "heartbeat",                  "device_id": "esp32_01", "uptime_ms": 42000 }

// LWT — publicado pelo broker se a conexão cair abruptamente
{ "status": "offline","cause": "lwt",                        "device_id": "esp32_01", "uptime_ms": 0     }
```

| `status` | `cause` | Quando ocorre | LED no firmware |
|---|---|---|---|
| `online` | `heartbeat` | Conexão estabelecida ou a cada 30s | Verde |
| `alert` | `manual` | Botão de pânico pressionado (disparo imediato, sem pré-alerta) | Vermelho (3s) |
| `alert` | `fall` | Queda validada pelas 5 fases + pré-alerta expirado sem cancelamento | Vermelho (3s) |
| `offline` | `lwt` | Broker detecta desconexão abrupta | — |

> **Nota:** durante a janela de pré-alerta (15s) o LED pisca em **amarelo** (vermelho + verde simultâneos). Se o idoso pressionar o botão neste período, o alarme é cancelado e **nenhuma** mensagem `"cause":"fall"` é publicada.

---

## Parte 2 — Rodando com Hardware Físico

> **O dashboard funciona igualmente com o hardware real — sem nenhuma modificação.**
> Tanto o simulador Python quanto o ESP32 físico publicam o mesmo JSON no mesmo tópico MQTT (`elderly/alerts`). O dashboard assina esse tópico e não precisa saber quem publicou.
>
> O único pré-requisito é:
> 1. Configurar `MQTT_BROKER` em `include/config.h` com o IP da máquina onde o Mosquitto está rodando
>    (descubra com `ipconfig | findstr "IPv4"` no Windows)
> 2. Garantir que o ESP32 esteja conectado à **mesma rede Wi-Fi** que a máquina do broker
>
> Com isso, o fluxo é idêntico ao da simulação — o dashboard nem precisa saber o IP do ESP32.

### Esquema de Montagem

```
ESP32                LED RGB (Cátodo Comum)         Botão
─────                ─────────────────────         ──────
GPIO 25 ──[220Ω]──── Vermelho (R)
GPIO 26 ──[220Ω]──── Verde    (G)
GPIO 27 ──[220Ω]──── Azul     (B)
GND    ──────────── Cátodo (pino comum)

GPIO 15 ──────────── Terminal A do botão
GND    ──────────── Terminal B do botão
                    (pull-up interno ativo no firmware)

MPU-6500             ESP32
────────             ─────
VCC  ──────────────── 3.3V
GND  ──────────────── GND
SDA  ──────────────── GPIO 21
SCL  ──────────────── GPIO 22
AD0  ──────────────── GND     (define endereço I2C como 0x68)
```

> **Cátodo Comum:** o pino mais longo do LED (ou marcado com `-`) vai ao GND.
> **Resistores:** use 220Ω ou 330Ω em cada canal de cor para limitar a corrente.
> **MPU-6500:** opere em 3.3V — não conecte em 5V ou o sensor será danificado.

### Adaptações necessárias no código

#### 1. `include/config.h` — obrigatório

```cpp
#define WIFI_SSID       "NomeDaSuaRede"    // sua rede Wi-Fi
#define WIFI_PASSWORD   "SuaSenha"          // sua senha
#define MQTT_BROKER     "192.168.1.XXX"     // IP da máquina com Mosquitto
                                            // descubra com: ipconfig | findstr "IPv4"
```

#### 2. `include/config.h` — se usar outros pinos GPIO

```cpp
#define PIN_BUTTON      15   // pino do botão de pânico
#define PIN_LED_RED     25   // LED RGB — canal vermelho
#define PIN_LED_GREEN   26   // LED RGB — canal verde
#define PIN_LED_BLUE    27   // LED RGB — canal azul
#define MPU_SDA_PIN     21   // I2C — dados
#define MPU_SCL_PIN     22   // I2C — clock
#define MPU_I2C_ADDR    0x68 // 0x68 se AD0=GND | 0x69 se AD0=3.3V
```

#### 3. `src/main.cpp` — se o LED for Ânodo Comum

O código padrão funciona com **Cátodo Comum** (HIGH = acende).
Se o seu LED for **Ânodo Comum** (HIGH = apaga), inverta a lógica na função `setLed()`:

```cpp
// Antes (cátodo comum — padrão)
digitalWrite(PIN_LED_RED, state == LED_ALERT ? HIGH : LOW);

// Depois (ânodo comum)
digitalWrite(PIN_LED_RED, state == LED_ALERT ? LOW : HIGH);
```

Repita para os canais verde e azul.

#### 4. `platformio.ini` — se usar outra variante do ESP32

```ini
board = esp32dev            # ESP32 DevKit v1 (padrão, 38 pinos)
board = nodemcu-32s         # NodeMCU ESP32
board = esp32-s3-devkitc-1  # ESP32-S3
board = esp32-c3-devkitm-1  # ESP32-C3
```

### Calibrando os Thresholds do MPU-6500

Os thresholds estão em `include/config.h`. Para ajustá-los com o sensor físico:

1. Descomente a linha de debug em `Task_Sensors` no `src/main.cpp`:
```cpp
// Serial.printf("[MPU] ax=%.3f ay=%.3f az=%.3f mag=%.3f g\n", ax, ay, az, mag);
```

2. Abra o monitor serial e observe os valores:

| Situação | Magnitude esperada |
|---|---|
| Sensor em repouso | ~1.0 g (gravidade) |
| Caminhada normal | ~1.0 – 1.5 g |
| Queda livre | ~0.0 – 0.3 g |
| Impacto leve (sentado no chão) | ~2.5 – 3.5 g |
| Impacto médio (escorregão) | ~3.5 – 5.0 g |
| Impacto forte (desmaio) | ~5.0 – 8.0 g |

3. Ajuste os valores conforme o que você observar:

```cpp
// Detecção primária (queda livre + impacto)
#define FREEFALL_THRESHOLD_G  0.50f  // reduz se não detectar, aumenta se der falso positivo
#define IMPACT_THRESHOLD_G    2.80f  // reduz para quedas leves, aumenta para evitar falsos
#define FALL_WINDOW_MS        500    // aumenta se a superfície for muito macia

// Verificação de postura (fase VERIFYING)
#define IMMOBILITY_THRESHOLD  0.15f  // aumenta se o idoso se mover muito após a queda
#define IMMOBILITY_DELAY_MS   3000   // aumenta em superfícies com rebote prolongado

// Janela de cancelamento de falsos positivos
#define CANCEL_WINDOW_MS      15000  // aumenta para dar mais tempo ao idoso
```

### Gravando o Firmware

```powershell
# Na pasta raiz do projeto
pio run --target upload

# Monitorar logs em tempo real
pio device monitor --baud 115200
```

Ou pelo VS Code: botão **→ Upload** na barra inferior do PlatformIO.

### Verificando o funcionamento

Após gravar, o LED deve:

1. **Azul** — conectando ao Wi-Fi e ao broker MQTT
2. **Verde** — tudo conectado, monitorando normalmente
3. **Amarelo piscando** — queda validada, aguardando 15s para o idoso cancelar
4. **Vermelho por 3s** — alerta confirmado enviado ao MQTT, depois volta ao verde

No monitor serial você verá (sequência de queda confirmada):
```
[System] VitaLink — inicializando...
[MPU] WHO_AM_I = 0x70 (MPU-6500 ✓)
[Sensors] MPU-6500 OK — monitoramento de queda ativado.
[WiFi] Conectado! IP: 192.168.1.105
[MQTT] Broker conectado!
[Sensors] Freefall confirmado (mag=0.12g). Aguardando impacto...
[Sensors] Impacto detectado (4.21g). Aguardando imobilidade (3000 ms)...
[Sensors] Queda validada (cosθ=0.431, var=0.04g). Entrando em PRE-ALERTA (15 s).
[Sensors] Janela expirada. QUEDA CONFIRMADA (4.21g). Enviando alerta MQTT.
[MQTT] Alerta publicado: {"status":"alert","cause":"fall","accel_g":4.21,...}
```

E se o idoso cancelar dentro dos 15s:
```
[Sensors] Alarme CANCELADO — falso positivo confirmado pelo usuario.
```

---

## Resolução de Problemas

| Sintoma | Causa provável | Solução |
|---|---|---|
| Dashboard não abre | Flask não iniciou | Rode `python app.py` e leia o erro |
| "Broker desconectado" no dashboard | Mosquitto não está rodando | Execute o Passo 1 |
| Simulador recusa conexão | Mosquitto parado | `& "C:\Program Files\mosquitto\mosquitto.exe" -v` |
| LED fica só azul | Wi-Fi não conecta | Confirme SSID e senha em `config.h` |
| LED azul mas sem verde | Broker inacessível | Confirme o IP em `MQTT_BROKER` |
| Botão não dispara alerta | GPIO errado ou fiação | Confira `PIN_BUTTON` e a montagem |
| LED acende a cor errada | LED Ânodo Comum | Inverta HIGH/LOW em `setLed()` |
| MPU não encontrado | I2C não conectado | Verifique SDA/SCL e o pino AD0 |
| `WHO_AM_I` inesperado | Modelo diferente | 0x68 = MPU-6050, 0x71 = MPU-9250 (ambos compatíveis) |
| Muitos falsos positivos de queda | `IMPACT_THRESHOLD_G` baixo | Aumente o valor em `config.h` |
| Quedas reais não detectadas | `IMPACT_THRESHOLD_G` alto | Reduza o valor em `config.h` |
| Queda detectada mas não envia alerta | Idoso ou cuidador pressionou o botão | Comportamento correto — o pré-alerta foi cancelado |
| LED amarelo pisca mas não envia alerta | `CANCEL_WINDOW_MS` expirou sem botão | Verifique a fiação do `PIN_BUTTON` |
| Sistema descarta quedas reais (fase VERIFYING) | `IMMOBILITY_THRESHOLD` baixo | Aumente o valor; pode haver vibração residual na superfície |
| Sistema não descarta sentadas | `IMPACT_THRESHOLD_G` baixo | Sentadas raramente passam de 2.80g — verifique a montagem |

---

## Próximos Passos Sugeridos

- [x] Máquina de estados de 5 fases com verificação de postura (produto escalar)
- [x] Janela de cancelamento de falsos positivos via botão físico (15s)
- [ ] Implementar **OTA** (atualização de firmware via Wi-Fi)
- [ ] Adicionar **GPS** para rastreamento de localização
- [ ] Enviar notificações via **WhatsApp ou e-mail** ao disparar alerta
- [ ] Substituir o broker local por um serviço cloud (HiveMQ, AWS IoT)
- [ ] Adicionar **sensor de frequência cardíaca** para monitoramento contínuo
