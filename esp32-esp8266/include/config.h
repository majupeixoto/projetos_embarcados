#pragma once

// Wi-Fi
#define WIFI_SSID       "SUA_REDE_WIFI"
#define WIFI_PASSWORD   "SUA_SENHA_WIFI"

// IPv6
// Quando true, ativa SLAAC logo após conectar ao Wi-Fi e loga o endereço IPv6.
// Exige que o roteador anuncie prefixos IPv6 (RA) na rede local.
// Para conectar o MQTT via IPv6, substitua MQTT_BROKER pelo endereço IPv6 do
// host do broker (ex: "fe80::1" link-local ou "2001:db8::1" global único).
#define ENABLE_IPV6     true

//MQTT Broker
#define MQTT_BROKER     "192.168.1.100"   // IP do broker local (ex: Mosquitto)
                                          // Para IPv6: "2001:db8::cafe" ou "fe80::1"
#define MQTT_PORT       1883
#define MQTT_TOPIC      "elderly/alerts"
#define MQTT_CLIENT_ID  "esp32_01"
#define DEVICE_ID       "esp32_01"

//GPIO
#define PIN_BUTTON      15   // Botão de pânico (pull-up interno)
#define PIN_LED_RED     25   // LED RGB — canal vermelho
#define PIN_LED_GREEN   26   // LED RGB — canal verde
#define PIN_LED_BLUE    27   // LED RGB — canal azul

//MPU-6500 (I2C)
#define MPU_SDA_PIN     21        // Pino de dados I2C (padrão ESP32)
#define MPU_SCL_PIN     22        // Pino de clock I2C (padrão ESP32)
#define MPU_I2C_ADDR    0x68      // AD0 ligado ao GND → 0x68
                                  // AD0 ligado ao 3.3V → mude para 0x69


// COMO CALIBRAR (com o sensor físico):
//   1. Descomente a linha de debug em Task_Sensors (busque "Serial.printf mag")
//   2. Abra o monitor serial (115200 baud)
//   3. Segure a pulseira em repouso → magnitude deve marcar ~1.0 g
//   4. Balance o pulso rapidamente → anote o pico (impacto leve ≈ 1.5–2.0 g)
//   5. Simule uma queda segura sobre uma superfície macia → anote o pico
//   6. Ajuste IMPACT_THRESHOLD_G para um valor entre o balanço e a queda real

#define FREEFALL_THRESHOLD_G    0.50f  // g — abaixo disso = possível queda livre
#define FREEFALL_SAMPLES        5      // amostras consecutivas (5 * 20ms = 100ms mínimo)
#define IMPACT_THRESHOLD_G      2.80f  // g — acima disso APÓS freefall = impacto confirmado
#define FALL_WINDOW_MS          500    // ms — janela máxima entre freefall e impacto
#define IMMOBILITY_THRESHOLD    0.15f  // g — variação máxima de magnitude para "imóvel"
#define IMMOBILITY_DELAY_MS     3000   // ms — aguarda o corpo sossegar após impacto
#define CANCEL_WINDOW_MS        15000  // ms — janela para cancelar alarme via botão
#define MPU_SAMPLE_MS           20     // ms — intervalo de leitura do sensor (50 Hz)

//Estados visuais do LED RGB
//
//  LED_CONNECTING : Azul              — conectando ao Wi-Fi / broker MQTT
//  LED_ONLINE     : Verde             — monitorando normalmente
//  LED_PRE_ALERT  : Amarelo piscando  — janela de cancelamento de 15s ativa
//                   (Vermelho + Verde acesos simultaneamente)
//  LED_ALERT      : Vermelho fixo 3s  — queda confirmada e alerta enviado
//  LED_OFF        : Apagado           — usado no ciclo de piscar do pré-alerta
//
// Timing
#define DEBOUNCE_MS             50
#define MQTT_RECONNECT_DELAY_MS 5000
#define WIFI_RECONNECT_DELAY_MS 10000
#define ALERT_LED_DURATION_MS   3000
#define PRE_ALERT_BLINK_MS      500    // ms — período do piscar amarelo no pré-alerta
