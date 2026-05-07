# VitaLink — Pulseira de Assistência ao Idoso

Protótipo de pulseira com **botão de pânico** e **detecção automática de quedas** via acelerômetro MPU-6500. O dispositivo (ESP32 + FreeRTOS) publica alertas via MQTT para um broker local, exibidos em um dashboard web em tempo real.

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
  └─ Máquina de estados de queda             ├─ Consome xAlertQueue
       └─▶ xAlertQueue (AlertEvent_t)         └─ Heartbeat a cada 30s
```

---

## Estrutura de Arquivos

```
projetos_embarcados/
├── include/
│   └── config.h              # Wi-Fi, broker MQTT, GPIOs e thresholds do MPU-6500
├── src/
│   └── main.cpp              # Firmware ESP32 — FreeRTOS + MPU-6500 + MQTT
├── simulation/
│   └── mock_esp32.py         # Simulador Python: botão de pânico + queda física
├── dashboard/
│   ├── app.py                # Backend Flask + Socket.IO + paho-mqtt
│   ├── requirements.txt      # Dependências Python do dashboard
│   └── templates/
│       └── index.html        # Interface web do dashboard
├── platformio.ini            # Configuração de build do firmware
└── README.md                 # Este arquivo
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
cd projetos_embarcados/dashboard
pip install -r requirements.txt
python app.py
```

Acesse **http://localhost:5000** no navegador.

### Passo 3 — Iniciar o Simulador

Em um **terceiro terminal**:

```powershell
cd projetos_embarcados/simulation
python mock_esp32.py
```

---

## Simulando Eventos

### Comandos disponíveis no simulador

| Comando | Ação |
|---|---|
| `ENTER` | Botão de pânico (`cause: manual`) |
| `f` + `ENTER` | Simular queda com física realista (`cause: fall`) |
| `h` + `ENTER` | Heartbeat manual |
| `q` + `ENTER` | Encerrar |

### Como simular uma queda

1. Com o simulador rodando, digite `f` e pressione **ENTER**
2. O terminal mostrará as duas fases da queda:

```
  ↓ FREEFALL  mag=0.18 g  (duração simulada: 210 ms)
  💥 IMPACTO   mag=4.73 g  (threshold: 2.5 g)
  [08:42:31] ALERTA — QUEDA DETECTADA (4.73 g)  {"status":"alert","cause":"fall",...}
```

3. Após 3 segundos o dispositivo volta automaticamente para `online`
4. No dashboard, o log exibe **"Queda Detectada"** (ícone laranja) com o valor do impacto em `g`

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
| `alert` | `manual` | Botão de pânico pressionado | Vermelho (3s) |
| `alert` | `fall` | Queda detectada pelo MPU-6500 | Vermelho (3s) |
| `offline` | `lwt` | Broker detecta desconexão abrupta | — |

---

## Parte 2 — Rodando com Hardware Físico

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
#define FREEFALL_THRESHOLD_G  0.40f  // reduz se não detectar, aumenta se der falso positivo
#define IMPACT_THRESHOLD_G    2.50f  // reduz para quedas leves, aumenta para evitar falsos
#define FALL_WINDOW_MS        500    // aumenta se a superfície for muito macia
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
3. **Vermelho por 3s** — botão pressionado ou queda detectada, depois volta ao verde

No monitor serial você verá:
```
[System] VitaLink — inicializando...
[MPU] WHO_AM_I = 0x70 (MPU-6500 ✓)
[Sensors] MPU-6500 OK — monitoramento de queda ativado.
[WiFi] Conectado! IP: 192.168.1.105
[MQTT] Broker conectado!
[Sensors] Freefall detectado (mag=0.12g). Aguardando impacto...
[Sensors] QUEDA DETECTADA! impacto=4.21g
[MQTT] Alerta publicado: {"status":"alert","cause":"fall","accel_g":4.21,...}
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
| Muitos falsos positivos de queda | Threshold baixo | Aumente `IMPACT_THRESHOLD_G` em `config.h` |
| Quedas reais não detectadas | Threshold alto | Reduza `IMPACT_THRESHOLD_G` em `config.h` |

---

## Próximos Passos Sugeridos

- [ ] Implementar **OTA** (atualização de firmware via Wi-Fi)
- [ ] Adicionar **GPS** para rastreamento de localização
- [ ] Enviar notificações via **WhatsApp ou e-mail** ao disparar alerta
- [ ] Substituir o broker local por um serviço cloud (HiveMQ, AWS IoT)
- [ ] Adicionar **sensor de frequência cardíaca** para monitoramento contínuo
