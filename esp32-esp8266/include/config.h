#pragma once

#define WIFI_SSID       "" 
#define WIFI_PASSWORD   ""

#define MQTT_BROKER     ""
#define MQTT_PORT       1883
#define MQTT_TOPIC      "elderly/alerts"
#define MQTT_CLIENT_ID  "esp8266_01"
#define DEVICE_ID       "esp8266_01"
#define PIN_BUTTON      0
#define PIN_LED_RED     12
#define PIN_LED_GREEN   13
#define PIN_LED_BLUE    14

#define MPU_SDA_PIN     4
#define MPU_SCL_PIN     5
#define MPU_I2C_ADDR    0x68

#define FREEFALL_THRESHOLD_G    0.50f
#define FREEFALL_SAMPLES        5
#define IMPACT_THRESHOLD_G      1.80f
#define FALL_WINDOW_MS          1000
#define IMMOBILITY_THRESHOLD    0.15f
#define IMMOBILITY_DELAY_MS     3000
#define CANCEL_WINDOW_MS        10000
#define MPU_SAMPLE_MS           20 

// Timings gerais
#define DEBOUNCE_MS             50
#define MQTT_RECONNECT_DELAY_MS 5000
#define WIFI_RECONNECT_DELAY_MS 10000
#define ALERT_LED_DURATION_MS   15000
#define PRE_ALERT_BLINK_MS      500
