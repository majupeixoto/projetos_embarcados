#pragma once

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
#define WIFI_SSID       "SUA_REDE_WIFI"
#define WIFI_PASSWORD   "SUA_SENHA_WIFI"

// ─── MQTT Broker ─────────────────────────────────────────────────────────────
#define MQTT_BROKER     "192.168.1.100"   // IP do broker local (ex: Mosquitto)
#define MQTT_PORT       1883
#define MQTT_TOPIC      "elderly/alerts"
#define MQTT_CLIENT_ID  "esp32_01"
#define DEVICE_ID       "esp32_01"

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define PIN_BUTTON      15   // Botão de pânico (pull-up interno)
#define PIN_LED_RED     25   // LED RGB — canal vermelho
#define PIN_LED_GREEN   26   // LED RGB — canal verde
#define PIN_LED_BLUE    27   // LED RGB — canal azul

// ─── MPU-6500 (I2C) ──────────────────────────────────────────────────────────
#define MPU_SDA_PIN     21        // Pino de dados I2C (padrão ESP32)
#define MPU_SCL_PIN     22        // Pino de clock I2C (padrão ESP32)
#define MPU_I2C_ADDR    0x68      // AD0 ligado ao GND → 0x68
                                  // AD0 ligado ao 3.3V → mude para 0x69

// ─── Thresholds de detecção de queda ─────────────────────────────────────────
//
// MÁQUINA DE ESTADOS (5 fases):
//
//   IDLE → FREEFALL_DETECTED → IMPACT_DETECTED → VERIFYING → PRE_ALERT → IDLE
//
//   IDLE             : atualiza EMA do vetor de postura estável continuamente.
//   FREEFALL_DETECTED: mag < FREEFALL_THRESHOLD por N amostras; aguarda impacto.
//   IMPACT_DETECTED  : impacto confirmado; aguarda IMMOBILITY_DELAY_MS.
//   VERIFYING        : checa imobilidade (var < IMMOBILITY_THRESHOLD) E mudança
//                      de postura (cosθ < 0.707, ou seja, ângulo > 45°).
//   PRE_ALERT        : janela de CANCEL_WINDOW_MS — botão cancela; senão envia.
//
// COMO CALIBRAR (com o sensor físico):
//   1. Descomente a linha de debug em Task_Sensors (busque "Serial.printf mag")
//   2. Abra o monitor serial (115200 baud)
//   3. Segure a pulseira em repouso → magnitude deve marcar ~1.0 g
//   4. Balance o pulso rapidamente → anote o pico (impacto leve ≈ 1.5–2.0 g)
//   5. Simule uma queda segura sobre uma superfície macia → anote o pico
//   6. Ajuste IMPACT_THRESHOLD_G para um valor entre o balanço e a queda real
//
// REFERÊNCIA DE VALORES TÍPICOS:
//   Repouso          ≈ 1.0 g   (gravidade)
//   Caminhada normal ≈ 1.2 g   (pico)
//   Queda livre      ≈ 0.0 g   (sensor em queda livre ideal)
//   Impacto no chão  ≈ 3–8 g   (depende da altura e superfície)
//
#define FREEFALL_THRESHOLD_G    0.50f  // g — abaixo disso = possível queda livre
#define FREEFALL_SAMPLES        5      // amostras consecutivas (5 * 20ms = 100ms mínimo)
#define IMPACT_THRESHOLD_G      2.80f  // g — acima disso APÓS freefall = impacto confirmado
#define FALL_WINDOW_MS          500    // ms — janela máxima entre freefall e impacto
#define IMMOBILITY_THRESHOLD    0.15f  // g — variação máxima de magnitude para "imóvel"
#define IMMOBILITY_DELAY_MS     3000   // ms — aguarda o corpo sossegar após impacto
#define CANCEL_WINDOW_MS        15000  // ms — janela para cancelar alarme via botão
#define MPU_SAMPLE_MS           20     // ms — intervalo de leitura do sensor (50 Hz)

// ─── Estados visuais do LED RGB ───────────────────────────────────────────────
//
//  LED_CONNECTING : Azul              — conectando ao Wi-Fi / broker MQTT
//  LED_ONLINE     : Verde             — monitorando normalmente
//  LED_PRE_ALERT  : Amarelo piscando  — janela de cancelamento de 15s ativa
//                   (Vermelho + Verde acesos simultaneamente)
//  LED_ALERT      : Vermelho fixo 3s  — queda confirmada e alerta enviado
//  LED_OFF        : Apagado           — usado no ciclo de piscar do pré-alerta
//
// ─── Timing ──────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS             50
#define MQTT_RECONNECT_DELAY_MS 5000
#define WIFI_RECONNECT_DELAY_MS 10000
#define HEARTBEAT_INTERVAL_MS   30000
#define ALERT_LED_DURATION_MS   3000
#define PRE_ALERT_BLINK_MS      500    // ms — período do piscar amarelo no pré-alerta
