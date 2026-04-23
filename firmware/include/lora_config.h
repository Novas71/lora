#pragma once

#ifndef LORA_FREQUENCY_MHZ
#define LORA_FREQUENCY_MHZ 868.1
#endif

#ifndef LORA_BANDWIDTH_KHZ
#define LORA_BANDWIDTH_KHZ 125.0
#endif

#ifndef LORA_SPREADING_FACTOR
#define LORA_SPREADING_FACTOR 9
#endif

#ifndef LORA_CODING_RATE
#define LORA_CODING_RATE 7
#endif

#ifndef LORA_SYNC_WORD
#define LORA_SYNC_WORD 0x12
#endif

#ifndef LORA_TX_POWER_DBM
#define LORA_TX_POWER_DBM 14
#endif

#ifndef LORA_PREAMBLE_LEN
#define LORA_PREAMBLE_LEN 8
#endif

// Default pinout from Meshtastic variant: variants/esp32s3/seeed_xiao_s3
#ifndef LORA_SCK_PIN
#define LORA_SCK_PIN 7
#endif

#ifndef LORA_MISO_PIN
#define LORA_MISO_PIN 8
#endif

#ifndef LORA_MOSI_PIN
#define LORA_MOSI_PIN 9
#endif

#ifndef LORA_CS_PIN
#define LORA_CS_PIN 41
#endif

#ifndef LORA_RST_PIN
#define LORA_RST_PIN 42
#endif

#ifndef LORA_DIO1_PIN
#define LORA_DIO1_PIN 39
#endif

#ifndef LORA_BUSY_PIN
#define LORA_BUSY_PIN 40
#endif

#ifndef APP_NODE_ID
#define APP_NODE_ID 1001
#endif

#ifndef APP_TX_INTERVAL_SEC
#define APP_TX_INTERVAL_SEC 300
#endif

#ifndef APP_TX_INTERVAL_MIN_SEC
#define APP_TX_INTERVAL_MIN_SEC 30
#endif

#ifndef APP_TX_INTERVAL_MAX_SEC
#define APP_TX_INTERVAL_MAX_SEC 86400
#endif

#ifndef DISTANCE_TX_INTERVAL_SEC
#define DISTANCE_TX_INTERVAL_SEC 300
#endif

#ifndef DISTANCE_NODE_ID
#define DISTANCE_NODE_ID 2001
#endif

#ifndef TANK_AREA_M2
#define TANK_AREA_M2 1.0f
#endif

#ifndef TANK_DISTANCE_MIN_MM
#define TANK_DISTANCE_MIN_MM 200
#endif

#ifndef TANK_DISTANCE_MAX_MM
#define TANK_DISTANCE_MAX_MM 2000
#endif

#ifndef JSN_TRIG_PIN
#define JSN_TRIG_PIN 1
#endif

#ifndef JSN_ECHO_PIN
#define JSN_ECHO_PIN 2
#endif

#ifndef JSN_POWER_PIN
#define JSN_POWER_PIN -1
#endif

#ifndef JSN_PING_TIMEOUT_US
#define JSN_PING_TIMEOUT_US 38000UL
#endif

#ifndef SENSOR_BATTERY_ADC_PIN
#define SENSOR_BATTERY_ADC_PIN 3
#endif

#ifndef SENSOR_BATTERY_DIVIDER_RATIO
#define SENSOR_BATTERY_DIVIDER_RATIO 2.0f
#endif

#ifndef SENSOR_LOW_BATTERY_MV
#define SENSOR_LOW_BATTERY_MV 3500
#endif

#ifndef DISTANCE_BATTERY_ADC_PIN
#define DISTANCE_BATTERY_ADC_PIN 3
#endif

#ifndef DISTANCE_BATTERY_DIVIDER_RATIO
#define DISTANCE_BATTERY_DIVIDER_RATIO 2.0f
#endif

#ifndef DISTANCE_LOW_BATTERY_MV
#define DISTANCE_LOW_BATTERY_MV 3500
#endif

#ifndef BATTERY_ADC_SAMPLES
#define BATTERY_ADC_SAMPLES 8
#endif

#ifndef DOWNLINK_RX_WINDOW_MS
#define DOWNLINK_RX_WINDOW_MS 2500
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME_WIFI_PASSWORD"
#endif

#ifndef MQTT_HOST
#define MQTT_HOST "192.168.1.10"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "lora2ha_gateway"
#endif

#ifndef MQTT_BASE_TOPIC
#define MQTT_BASE_TOPIC "lora2ha"
#endif

#ifndef FALLBACK_AP_SSID
#define FALLBACK_AP_SSID "LoRaGateway-Setup"
#endif

#ifndef FALLBACK_AP_PASSWORD
#define FALLBACK_AP_PASSWORD "loragw123"
#endif

#ifndef OTA_HOSTNAME_GATEWAY
#define OTA_HOSTNAME_GATEWAY "lora-gateway"
#endif

#ifndef OTA_HOSTNAME_SENSOR
#define OTA_HOSTNAME_SENSOR "lora-sensor"
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

#ifndef OTA_DEFAULT_WINDOW_SEC
#define OTA_DEFAULT_WINDOW_SEC 300
#endif

#ifndef LORA_AES_KEY_HEX
#define LORA_AES_KEY_HEX "00112233445566778899AABBCCDDEEFF"
#endif

#ifndef LORA_NETWORK_ID
#define LORA_NETWORK_ID 1
#endif
