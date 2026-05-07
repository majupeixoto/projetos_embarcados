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

// ─── Timing ──────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS             50
#define MQTT_RECONNECT_DELAY_MS 5000
#define WIFI_RECONNECT_DELAY_MS 10000
#define HEARTBEAT_INTERVAL_MS   30000
#define ALERT_LED_DURATION_MS   3000
