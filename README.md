# VitaLink — Pulseira de Assistência ao Idoso

Protótipo de pulseira com botão de pânico para monitoramento de idosos. O dispositivo (ESP32) envia alertas via MQTT para um broker local, exibidos em um dashboard web em tempo real.

---

## Arquitetura do Sistema

```
┌─────────────────┐        MQTT        ┌──────────────────┐       WebSocket      ┌───────────────────┐
│  ESP32 / Simul. │ ──────────────────▶│ Mosquitto Broker │ ◀───────────────────▶│ Dashboard (Flask) │
│  (Pulseira)     │  elderly/alerts    │  localhost:1883  │                      │  localhost:5000   │
└─────────────────┘                    └──────────────────┘                      └───────────────────┘
```

**Fluxo:** Botão pressionado → ESP32 publica JSON no tópico MQTT → Flask recebe e repassa via Socket.IO → Dashboard atualiza em tempo real.

---

## Estrutura de Arquivos

```
projetos_embarcados/
├── include/
│   └── config.h              # Credenciais Wi-Fi, IP do broker, GPIOs
├── src/
│   └── main.cpp              # Firmware ESP32 com FreeRTOS
├── simulation/
│   └── mock_esp32.py         # Simulador Python (substitui o ESP32 físico)
├── dashboard/
│   ├── app.py                # Backend Flask + Socket.IO + paho-mqtt
│   ├── requirements.txt      # Dependências Python do dashboard
│   └── templates/
│       └── index.html        # Interface web do dashboard
└── platformio.ini            # Configuração de build do firmware
```

---

## Pré-requisitos

| Ferramenta | Finalidade | Download |
|---|---|---|
| [Mosquitto](https://mosquitto.org/download/) | Broker MQTT local | mosquitto.org |
| Python 3.10+ | Dashboard e simulador | python.org |
| PlatformIO (opcional) | Compilar e gravar firmware | Extensão VS Code |

---

## Parte 1 — Rodando sem Hardware (Simulação)

Ideal para desenvolver e testar o dashboard antes de ter o ESP32 em mãos.

### Passo 1 — Iniciar o Broker MQTT

Abra um terminal e execute:

```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -v
```

> Deixe este terminal aberto. O `-v` ativa logs verbosos para acompanhar as mensagens em tempo real.

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

Pressione **ENTER** para simular o botão de pânico. O dashboard atualiza instantaneamente.

**Opções do simulador:**

```powershell
# Broker em outra máquina da rede
python mock_esp32.py --broker 192.168.1.100

# Simular outro dispositivo
python mock_esp32.py --device esp32_02

# Broker público para testes rápidos (dados visíveis publicamente)
python mock_esp32.py --broker test.mosquitto.org
```

### Formato das mensagens MQTT

```json
{ "status": "alert",  "device_id": "esp32_01", "uptime_ms": 12400 }
{ "status": "online", "device_id": "esp32_01", "uptime_ms": 42000 }
{ "status": "offline","device_id": "esp32_01", "uptime_ms": 0     }
```

| Status | Quando | LED no firmware |
|---|---|---|
| `online` | Conexão estabelecida / heartbeat (30s) | Verde |
| `alert` | Botão de pânico pressionado | Vermelho pulsante |
| `offline` | LWT — enviado pelo broker se a conexão cair | — |

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
```

> **Cátodo Comum:** o pino mais longo (ou marcado com `-`) vai ao GND.  
> **Resistores:** use 220Ω ou 330Ω em cada canal de cor para limitar corrente.

### Adaptações necessárias no código

#### 1. `include/config.h` — obrigatório

Edite as três linhas abaixo antes de gravar o firmware:

```cpp
// Linha 4 — nome da sua rede Wi-Fi
#define WIFI_SSID       "NomeDaSuaRede"

// Linha 5 — senha da sua rede Wi-Fi
#define WIFI_PASSWORD   "SuaSenha"

// Linha 8 — IP da máquina onde o Mosquitto está rodando
//           Descubra rodando: ipconfig | findstr "IPv4"
#define MQTT_BROKER     "192.168.1.XXX"
```

#### 2. `include/config.h` — se usar outros pinos GPIO

```cpp
#define PIN_BUTTON      15   // altere se conectar o botão em outro pino
#define PIN_LED_RED     25   // altere conforme sua montagem
#define PIN_LED_GREEN   26
#define PIN_LED_BLUE    27
```

#### 3. `src/main.cpp` — se o LED for Ânodo Comum

O código padrão funciona com **Cátodo Comum** (HIGH = acende).  
Se o seu LED for **Ânodo Comum** (HIGH = apaga), inverta a lógica na função `setLed()`:

```cpp
// Antes (cátodo comum)
digitalWrite(PIN_LED_RED,   state == LED_ALERT     ? HIGH : LOW);

// Depois (ânodo comum)
digitalWrite(PIN_LED_RED,   state == LED_ALERT     ? LOW : HIGH);
```

Faça o mesmo para os canais verde e azul.

#### 4. `platformio.ini` — se usar outra variante do ESP32

```ini
board = esp32dev          # ESP32 DevKit v1 (padrão, 38 pinos)
board = nodemcu-32s       # NodeMCU ESP32
board = esp32-s3-devkitc-1  # ESP32-S3
board = esp32-c3-devkitm-1  # ESP32-C3
```

### Gravando o Firmware

Com o ESP32 conectado via USB:

```powershell
# Dentro da pasta raiz do projeto (onde está o platformio.ini)
pio run --target upload

# Acompanhar logs do dispositivo
pio device monitor --baud 115200
```

Ou pelo VS Code: clique na seta **→ Upload** na barra inferior do PlatformIO.

### Verificando o funcionamento

Após gravar, o LED deve:

1. **Piscar azul** enquanto conecta ao Wi-Fi
2. **Ficar verde** quando conectar ao broker MQTT
3. **Ficar vermelho** por 3 segundos ao pressionar o botão, depois voltar ao verde

No terminal do Mosquitto você verá:

```
client esp32_01 connected
elderly/alerts {"status":"online","device_id":"esp32_01","uptime_ms":3200}
elderly/alerts {"status":"alert","device_id":"esp32_01","uptime_ms":15800}
```

---

## Resolução de Problemas

| Sintoma | Causa provável | Solução |
|---|---|---|
| Dashboard não abre | Flask não iniciou | Rode `python app.py` e veja o erro no terminal |
| "Broker desconectado" no dashboard | Mosquitto não está rodando | Execute o passo 1 |
| Simulador recusa conexão | Mosquitto parado | `& "C:\Program Files\mosquitto\mosquitto.exe" -v` |
| LED fica só azul no ESP32 | Wi-Fi não conecta | Confirme SSID/senha em `config.h` |
| LED azul → mas sem verde | Broker inacessível | Confirme o IP em `MQTT_BROKER` e que o Mosquitto está na mesma rede |
| Botão não dispara alerta | Debounce ou GPIO errado | Confira `PIN_BUTTON` em `config.h` e a montagem |
| LED acende a cor errada | LED Ânodo Comum | Inverta HIGH/LOW em `setLed()` conforme descrito acima |

---

## Próximos Passos Sugeridos

- [ ] Adicionar acelerômetro **MPU-6050** para detecção automática de quedas
- [ ] Implementar **OTA** (atualização de firmware via Wi-Fi) — descomentar a linha em `platformio.ini`
- [ ] Adicionar **GPS** para rastreamento de localização
- [ ] Enviar notificações via **WhatsApp ou e-mail** ao disparar alerta
- [ ] Substituir o broker local por um serviço cloud (HiveMQ, AWS IoT)
