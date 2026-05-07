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
// COMO CALIBRAR (sem hardware):
//   Não é necessário — os valores abaixo são um ponto de partida conservador.
//
// COMO CALIBRAR (com o sensor físico):
//   1. Descomente a linha de debug em Task_Sensors (busque por "Serial.printf mag")
//   2. Abra o monitor serial (115200 baud)
//   3. Segure a pulseira em repouso → magnitude deve marcar ~1.0 g
//   4. Balance o pulso rapidamente → anote o pico (impacto leve ≈ 1.5–2.0 g)
//   5. Simule uma queda segura sobre uma superfície macia → anote o pico
//   6. Ajuste IMPACT_THRESHOLD_G para um valor entre o balanço e a queda real
//   7. Quanto MENOR o valor, MAIS sensível (mais falsos positivos)
//      Quanto MAIOR o valor, MENOS sensível (pode perder quedas reais)
//
// REFERÊNCIA DE VALORES TÍPICOS:
//   Repouso          ≈ 1.0 g  (gravidade)
//   Caminhada normal ≈ 1.2 g  (pico)
//   Queda livre      ≈ 0.0 g  (sensor em queda livre ideal)
//   Impacto no chão  ≈ 3–8 g  (depende da altura e superfície)
//
#define FREEFALL_THRESHOLD_G    0.40f  // g — abaixo disso = possível queda livre
                                       // Aumente se detectar falsos positivos em movimento
#define FREEFALL_SAMPLES        5      // amostras consecutivas para confirmar freefall
                                       // Com MPU_SAMPLE_MS=20 → 5 * 20ms = 100ms de freefall
#define IMPACT_THRESHOLD_G      2.50f  // g — acima disso APÓS freefall = impacto confirmado
                                       // Reduza para 2.0 se quedas leves não forem detectadas
#define FALL_WINDOW_MS          500    // ms — janela máxima entre freefall e impacto
                                       // Aumente se a superfície for muito macia (impacto lento)
#define FALL_COOLDOWN_MS        8000   // ms — bloqueio após alerta (evita duplicatas)
#define MPU_SAMPLE_MS           20     // ms — intervalo de leitura do sensor (50 Hz)

// ─── Timing ──────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS             50
#define MQTT_RECONNECT_DELAY_MS 5000
#define WIFI_RECONNECT_DELAY_MS 10000
#define HEARTBEAT_INTERVAL_MS   30000
#define ALERT_LED_DURATION_MS   3000
