# VitaLink — Pulseira de Assistência ao Idoso

Protótipo de pulseira com **botão de pânico** e **detecção inteligente de quedas** via acelerômetro MPU-6500/MPU-6050. O dispositivo (ESP8266) usa uma máquina de estados de 5 fases — incluindo verificação de postura por produto escalar e janela de cancelamento de falsos positivos — para publicar alertas via MQTT, exibidos em um dashboard web em tempo real.

O mesmo projeto serve de base para o trabalho de **Análise de Algoritmos**, demonstrando empiricamente a diferença entre uma abordagem O(n) (memmove) e um Buffer Circular O(1) para gerenciamento de telemetria de sensor.

---

## Arquitetura do Sistema

```
┌─────────────────────┐  elderly/alerts   ┌──────────────────┐  WebSocket  ┌───────────────────┐
│  ESP8266 / Mock     │ ─────────────────▶│                  │ ◀──────────▶│ Dashboard (Flask) │
│  Botão + MPU-6500   │  elderly/samples  │ Mosquitto Broker │             │  localhost:5000   │
│                     │ ─────────────────▶│  localhost:1883  │             │                   │
│  Benchmark ESP32    │  elderly/benchmark│                  │             │  - Alertas        │
│  ou Mock (b)        │ ─────────────────▶│                  │             │  - Telemetria     │
└─────────────────────┘                   └──────────────────┘             │  - Performance    │
                                                                           └───────────────────┘
```

| Tópico | Publicado por | Exibido no dashboard |
|---|---|---|
| `elderly/alerts` | ESP8266 (firmware) ou Mock | Status, log de eventos |
| `elderly/samples` | ESP8266 (sensor real) | Gráfico de telemetria em tempo real |
| `elderly/benchmark` | ESP32 (benchmark) ou Mock (`b`) | Gráfico de performance V1 vs V2 |

---

## Estrutura de Arquivos

```
projetos_embarcados/
├── esp32-esp8266/
│   ├── include/
│   │   ├── config.h              # Wi-Fi, broker MQTT, GPIOs e thresholds
│   │   └── CircularBuffer.h      # Buffer circular genérico O(1) sem heap
│   ├── src/
│   │   └── main.cpp              # Firmware ESP8266 — loop único + MPU-6500 + MQTT
│   ├── benchmarks/
│   │   ├── benchmark.cpp         # Benchmark SlidingWindow (V1) vs CircularBuffer (V2)
│   │   └── GUIA_BENCHMARK.md     # Como executar e interpretar o benchmark
│   └── platformio.ini            # Ambientes: esp8266dev e benchmark (ESP32)
├── applications/
│   ├── simulation/
│   │   └── mock_esp32.py         # Simulador: botão de pânico + queda + telemetria
│   └── dashboard/
│       ├── app.py                # Backend Flask + Socket.IO + paho-mqtt
│       ├── requirements.txt      # Dependências Python
│       └── templates/
│           └── index.html        # Interface web
├── docs/
├── schematics/
└── README.md
```

---

## Pré-requisitos

| Ferramenta | Finalidade | Windows | Linux |
|---|---|---|---|
| [Mosquitto](https://mosquitto.org/download/) | Broker MQTT local | `winget install EclipseFoundation.Mosquitto` | `sudo apt install mosquitto mosquitto-clients` |
| Python 3.10+ | Dashboard e simulador | python.org | `sudo apt install python3 python3-pip` |
| PlatformIO | Compilar e gravar firmware | Extensão VS Code | Extensão VS Code |

---

## Parte 1 — Rodando sem Hardware (Simulação)

Ideal para desenvolver e testar o dashboard antes de ter o ESP8266 em mãos.

### Passo 1 — Iniciar o Broker MQTT

**Windows:**
```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -v
```

**Linux:**
```bash
mosquitto -v
```

> Se o Mosquitto estiver rodando como serviço, pare-o antes:
> ```bash
> sudo systemctl stop mosquitto
> mosquitto -v
> ```

> Deixe este terminal aberto. O `-v` ativa logs verbosos para acompanhar as mensagens.

### Passo 2 — Iniciar o Dashboard

Em um **segundo terminal**:

```bash
cd projetos_embarcados/applications/dashboard
pip install -r requirements.txt   # Windows
pip3 install -r requirements.txt  # Linux
python app.py    # Windows
python3 app.py   # Linux
```

Acesse **http://localhost:5000** no navegador.

### Passo 3 — Iniciar o Simulador

Em um **terceiro terminal**:

```bash
cd projetos_embarcados/applications/simulation
python mock_esp32.py    # Windows
python3 mock_esp32.py   # Linux
```

---

## Simulando Eventos

### Comandos disponíveis no simulador

| Comando | Ação |
|---|---|
| `ENTER` | Botão de pânico — alerta imediato (`cause: manual`), sem pré-alerta |
| `f` + `ENTER` | Simular queda completa — 4 fases + janela de cancelamento de 10s |
| `c` + `ENTER` | Cancelar o pré-alerta ativo (simula o idoso pressionando o botão) |
| `s` + `ENTER` | Simular sentar/abaixar — mostra por que o sistema **não** dispara alarme |
| `b` + `ENTER` | Publicar resultados do benchmark no dashboard (Análise de Performance) |
| `q` + `ENTER` | Encerrar |

### Como simular uma queda

1. Com o simulador rodando, digite `f` e pressione **ENTER**
2. O terminal mostrará as **4 fases** da detecção, seguidas do pré-alerta:

```
  ↓ QUEDA LIVRE    mag=0.18g  (threshold: <0.50g)  duração≈210ms
  💥 IMPACTO        mag=4.73g  (threshold: >1.80g)
  ⏳ IMOBILIDADE    aguardando 3s para o corpo sossegar...
  📐 POSTURA        cosθ=0.423  (< 0.707 → ângulo > 45°)  var=0.05g  → QUEDA VALIDADA

  ⚠  PRÉ-ALERTA ATIVO — impacto=4.73g
     Digite 'c' + Enter para CANCELAR o alarme.

  ⏱  10s restantes...
```

3. **Cenário A — deixar o tempo expirar (queda real):**
   - Aguarde os 10 segundos sem digitar nada
   - O simulador publica `{"status":"alert","cause":"fall","accel_g":4.73,...}`
   - No dashboard, o log exibe **"Queda Confirmada"** com o valor do impacto em g

4. **Cenário B — cancelar o alarme (falso positivo):**
   - Durante a contagem regressiva, digite `c` e pressione **ENTER**
   - O simulador exibe `✓ Alarme CANCELADO — falso positivo confirmado`
   - Nenhuma mensagem MQTT de alerta é publicada; o dispositivo volta a `online`

### Como simular um sentar (sem alarme)

Digite `s` e pressione **ENTER**. O simulador exibe o perfil de aceleração do movimento, mostrando que a magnitude nunca cai abaixo de `0.50g` (sem queda livre) nem sobe acima de `1.80g` (sem impacto) — o firmware permanece em **IDLE**.

### Modo automático (stress test do dashboard)

```bash
# Queda aleatória a cada ~30 segundos
python mock_esp32.py --auto-fall 30

# Combinando com outro broker e device ID
python mock_esp32.py --broker 192.168.1.100 --device esp32_02 --auto-fall 60
```

### Publicar dados do benchmark (Análise de Performance)

Digite `b` + **ENTER** no simulador. Ele publica os resultados V1 vs V2 em `elderly/benchmark` — o dashboard exibe o gráfico de barras e o log de latência automaticamente.

---

## Lógica de Detecção de Quedas

A detecção passa por **5 fases sequenciais**, reduzindo drasticamente os falsos positivos causados por sentar, agachar ou movimentos bruscos normais.

### Fase 1 — Queda Livre (`FREEFALL_DETECTED`)

O firmware monitora continuamente a magnitude do vetor de aceleração:

$$A_m = \sqrt{a_x^2 + a_y^2 + a_z^2}$$

Quando $A_m$ cai abaixo de `FREEFALL_THRESHOLD_G` (0.50 g) por 5 amostras consecutivas (~100ms), a queda livre é confirmada. Em IDLE, o firmware também mantém uma **média exponencial** (EMA, α=0.05, τ≈390ms) dos 3 eixos — o "vetor de postura estável" usado na Fase 4.

### Fase 2 — Impacto (`IMPACT_DETECTED`)

Se $A_m$ superar `IMPACT_THRESHOLD_G` (1.80 g) dentro de `FALL_WINDOW_MS` (1000ms) da queda livre, o impacto é confirmado. Caso contrário, o evento é descartado.

### Fase 3 — Imobilidade (`VERIFYING`, parte 1)

O firmware aguarda `IMMOBILITY_DELAY_MS` (3s) para o corpo sossegar e coleta 10 amostras (~200ms). Se a variação de magnitude entre elas for maior que `IMMOBILITY_THRESHOLD` (0.15 g), a pessoa está em movimento — evento descartado.

### Fase 4 — Verificação de Postura (`VERIFYING`, parte 2)

Com a pessoa imóvel confirmada, o firmware calcula o produto escalar entre o vetor estável (pré-queda) e o vetor atual:

$$\cos\theta = \frac{\vec{a}_{estavel} \cdot \vec{a}_{atual}}{|\vec{a}_{estavel}| \cdot |\vec{a}_{atual}|}$$

- **cos θ < 0.707** (ângulo > 45°): orientação mudou → corpo provavelmente horizontal → **queda validada**
- **cos θ ≥ 0.707**: postura similar à original → provavelmente sentou/agachou → **descartado**

### Fase 5 — Pré-Alerta e Cancelamento (`PRE_ALERT`)

Queda validada ativa o LED **amarelo piscante** e um timer de `CANCEL_WINDOW_MS` (10s). Durante este período:

- O idoso pressiona o botão → alarme cancelado, LED volta ao verde, **nenhum JSON é publicado**
- O timer expira sem ação → alerta `{"status":"alert","cause":"fall"}` publicado no MQTT, LED vermelho por 15s

> **Botão de pânico manual (ENTER / `cause: manual`):** dispara alerta **imediatamente**, sem passar pelas 5 fases. Útil quando o idoso percebe que vai cair antes de cair.

---

## Formato das Mensagens MQTT (`elderly/alerts`)

```json
{ "status": "alert",  "cause": "fall",   "accel_g": 2.09, "device_id": "esp8266_01", "uptime_ms": 23992 }
{ "status": "alert",  "cause": "manual",                  "device_id": "esp8266_01", "uptime_ms": 15800 }
{ "status": "online", "cause": "online",                  "device_id": "esp8266_01", "uptime_ms": 6449  }
{ "status": "offline","cause": "lwt",                     "device_id": "esp8266_01", "uptime_ms": 0     }
```

| `status` | `cause` | Quando ocorre | LED no firmware |
|---|---|---|---|
| `online` | `online` | Conexão estabelecida ou reconexão após queda | Verde |
| `alert` | `manual` | Botão de pânico pressionado (disparo imediato, sem pré-alerta) | Vermelho (15s) |
| `alert` | `fall` | Queda validada pelas 5 fases + pré-alerta expirado sem cancelamento | Vermelho (15s) |
| `offline` | `lwt` | Broker detecta desconexão abrupta | — |

> **Nota:** durante a janela de pré-alerta (10s) o LED pisca em **amarelo** (vermelho + verde simultâneos). Se o idoso pressionar o botão neste período, o alarme é cancelado e **nenhuma** mensagem `"cause":"fall"` é publicada.

---

## Parte 2 — Rodando com Hardware Físico (ESP8266)

> O dashboard funciona igualmente com o hardware real — sem nenhuma modificação. O ESP8266 e o simulador publicam o mesmo JSON nos mesmos tópicos MQTT. O único pré-requisito é configurar `MQTT_BROKER` em `config.h` com o IP da máquina onde o Mosquitto está rodando.

### Esquema de Montagem

```
ESP8266              LED RGB (Cátodo Comum)        Botão
───────              ─────────────────────         ──────
GPIO 12 ──[220Ω]──── Vermelho (R)
GPIO 13 ──[220Ω]──── Verde    (G)
GPIO 14 ──[220Ω]──── Azul     (B)
GND     ──────────── Cátodo (pino comum)

GPIO 0  ──────────── Terminal A do botão
GND     ──────────── Terminal B do botão
                     (pull-up interno ativo no firmware)

MPU-6500/6050        ESP8266
─────────────        ───────
VCC  ─────────────── 3.3V
GND  ─────────────── GND
SDA  ─────────────── GPIO 4
SCL  ─────────────── GPIO 5
AD0  ─────────────── GND   (endereço I2C 0x68)
```

> **Cátodo Comum:** o pino mais longo do LED (ou marcado com `-`) vai ao GND.
> **Resistores:** use 220Ω ou 330Ω em cada canal de cor para limitar a corrente.
> **MPU-6500/6050:** opere em 3.3V — não conecte em 5V ou o sensor será danificado.

### Adaptações necessárias no código

#### 1. `include/config.h` — obrigatório

```cpp
#define WIFI_SSID       "NomeDaSuaRede"
#define WIFI_PASSWORD   "SuaSenha"
#define MQTT_BROKER     "192.168.1.XXX"     // IP da máquina com Mosquitto
                                            // Windows: ipconfig | findstr "IPv4"
                                            // Linux:   hostname -I
```

#### 2. `include/config.h` — se usar outros pinos GPIO

```cpp
#define PIN_BUTTON      0    // pino do botão de pânico
#define PIN_LED_RED     12   // LED RGB — canal vermelho
#define PIN_LED_GREEN   13   // LED RGB — canal verde
#define PIN_LED_BLUE    14   // LED RGB — canal azul
#define MPU_SDA_PIN     4    // I2C — dados
#define MPU_SCL_PIN     5    // I2C — clock
#define MPU_I2C_ADDR    0x68 // 0x68 se AD0=GND | 0x69 se AD0=3.3V
```

#### 3. `src/main.cpp` — se o LED for Ânodo Comum

O código padrão funciona com **Cátodo Comum** (HIGH = acende). Se o seu LED for **Ânodo Comum** (HIGH = apaga), inverta a lógica na função `setLed()`:

```cpp
// Antes (cátodo comum — padrão)
digitalWrite(PIN_LED_RED, state == LED_ALERT ? HIGH : LOW);

// Depois (ânodo comum)
digitalWrite(PIN_LED_RED, state == LED_ALERT ? LOW : HIGH);
```

### Gravando o Firmware

```bash
pio run -e esp8266dev --target upload

# Monitorar logs em tempo real
pio device monitor -e esp8266dev
```

> **Linux:** se receber `Permission denied` na porta serial:
> ```bash
> sudo usermod -aG dialout $USER
> ```

### Verificando o funcionamento

Após gravar, o LED deve:

1. **Azul** — conectando ao Wi-Fi e ao broker MQTT
2. **Verde** — tudo conectado, monitorando normalmente
3. **Amarelo piscando** — queda validada, aguardando 10s para o idoso cancelar
4. **Vermelho por 15s** — alerta confirmado enviado ao MQTT, depois volta ao verde

No monitor serial (sequência de queda confirmada):

```
[System] VitaLink ESP8266 — inicializando...
[MPU] WHO_AM_I = 0x68 (MPU-6050 — compativel)
[Sensors] MPU OK — monitoramento de queda ativado.
[WiFi] Conectando a "sua_rede"...
[WiFi] IPv4: 192.168.x.x
[MQTT] Broker conectado!
[MQTT] Online: {"status":"online","cause":"online",...}
[Sensors] Freefall confirmado (mag=0.25g).
[Sensors] Impacto detectado (2.09g).
[Sensors] Queda validada (cosT=0.665, var=0.04g). PRE-ALERTA.
[Sensors] QUEDA CONFIRMADA (2.09g). Enviando alerta.
[MQTT] Alerta publicado: {"status":"alert","cause":"fall","accel_g":2.09,...}
[MQTT] Online: {"status":"online","cause":"online",...}
```

### Telemetria em tempo real

Com o firmware gravado, o ESP8266 também publica leituras do MPU em `elderly/samples` a cada 200ms. O gráfico **Telemetria em Tempo Real** no dashboard exibe a magnitude da aceleração ao vivo — inclusive é possível visualizar a queda livre (abaixo de 0.5g) e o pico de impacto no gráfico.

### Calibrando os Thresholds do MPU

Os thresholds estão em `include/config.h`. Para ajustá-los com o sensor físico, observe os valores no monitor serial e compare com a tabela:

| Situação | Magnitude esperada |
|---|---|
| Sensor em repouso | ~1.0 g (gravidade) |
| Caminhada normal | ~1.0 – 1.5 g |
| Queda livre | ~0.0 – 0.3 g |
| Impacto leve (sentar no chão) | ~1.5 – 2.5 g |
| Impacto médio (escorregão) | ~2.5 – 4.0 g |
| Impacto forte (desmaio) | ~4.0 – 8.0 g |

```cpp
#define FREEFALL_THRESHOLD_G  0.50f  // reduz se não detectar, aumenta se der falso positivo
#define IMPACT_THRESHOLD_G    1.80f  // reduz para quedas leves, aumenta para evitar falsos
#define FALL_WINDOW_MS        1000   // aumenta se a superfície for muito macia
#define IMMOBILITY_THRESHOLD  0.15f  // aumenta se o idoso se mover muito após a queda
#define IMMOBILITY_DELAY_MS   3000   // aumenta em superfícies com rebote prolongado
#define CANCEL_WINDOW_MS      10000  // aumenta para dar mais tempo ao idoso
```

---

## Benchmark — Análise de Algoritmos

Consulte o [GUIA_BENCHMARK.md](esp32-esp8266/benchmarks/GUIA_BENCHMARK.md) para instruções completas.

**Resumo rápido para apresentação sem ESP32:**
1. Inicie o broker + dashboard + simulador
2. Digite `b` + Enter no simulador
3. O dashboard popula o gráfico de **Análise de Performance** com log de latência

---

## Resolução de Problemas

| Sintoma | Causa provável | Solução |
|---|---|---|
| Dashboard não abre | Flask não iniciou | Rode `python app.py` e leia o erro |
| "Broker desconectado" no dashboard | Mosquitto não está rodando | Execute o Passo 1 |
| Simulador recusa conexão | Mosquitto parado | Windows: `& "C:\Program Files\mosquitto\mosquitto.exe" -v` |
| LED fica só azul | Wi-Fi não conecta | Confirme SSID e senha em `config.h` |
| LED azul mas sem verde | Broker inacessível | Confirme o IP em `MQTT_BROKER` |
| Gráfico de telemetria vazio | Firmware sem MPU ou muito antigo | Regrave o firmware e verifique o MPU |
| Gráfico de performance vazio | Benchmark não publicado | Digite `b` no simulador ou rode benchmark no ESP32 |
| Botão não dispara alerta | GPIO errado ou fiação | Confira `PIN_BUTTON` e a montagem |
| LED acende a cor errada | LED Ânodo Comum | Inverta HIGH/LOW em `setLed()` |
| MPU não encontrado | I2C não conectado | Verifique SDA=GPIO4 e SCL=GPIO5 |
| `WHO_AM_I` = 0x68 | MPU-6050 (compatível) | Normal — funciona igual ao MPU-6500 |
| Muitos falsos positivos de queda | `IMPACT_THRESHOLD_G` baixo | Aumente o valor em `config.h` |
| Quedas reais não detectadas | `IMPACT_THRESHOLD_G` alto | Reduza o valor em `config.h` |
| Queda detectada mas não envia alerta | Pré-alerta foi cancelado | Comportamento correto |
| Sistema descarta quedas (fase VERIFYING) | `IMMOBILITY_THRESHOLD` baixo | Aumente o valor |

---

## Próximos Passos Sugeridos

- [x] Máquina de estados de 5 fases com verificação de postura (produto escalar)
- [x] Janela de cancelamento de falsos positivos via botão físico (10s)
- [x] Buffer Circular O(1) para telemetria de sensor
- [x] Dashboard com telemetria em tempo real e análise de performance
- [ ] Implementar **OTA** (atualização de firmware via Wi-Fi)
- [ ] Adicionar **GPS** para rastreamento de localização
- [ ] Enviar notificações via **WhatsApp ou e-mail** ao disparar alerta
- [ ] Substituir o broker local por um serviço cloud (HiveMQ, AWS IoT)
- [ ] Adicionar **sensor de frequência cardíaca** para monitoramento contínuo
