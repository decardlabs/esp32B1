#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "driver/gpio.h"

/* ==================== WiFi / MQTT ==================== */
/* ⚠️ 烧录前请修改为实际值 */
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define MQTT_BROKER     "YOUR_SERVER_IP"
#define MQTT_PORT       1883
#define MQTT_USERNAME   "YOUR_MQTT_USER"
#define MQTT_PASSWORD   "YOUR_MQTT_PASSWORD"
#define DEVICE_ID       "esp32_01"

/* ==================== I2C0 (XL9555 + ES8388) ==================== */
#define PIN_I2C0_SDA    GPIO_NUM_41
#define PIN_I2C0_SCL    GPIO_NUM_42
#define XL9555_I2C_ADDR 0x20
#define ES8388_I2C_ADDR 0x10
/* XL9555 pin: speaker enable (P0.0, active low) */
#define XL9555_SPK_PIN  0

/* ==================== I2S0 (→ ES8388) ==================== */
#define PIN_I2S0_MCLK   GPIO_NUM_3
#define PIN_I2S0_BCLK   GPIO_NUM_46
#define PIN_I2S0_LRCK   GPIO_NUM_9
#define PIN_I2S0_DOUT   GPIO_NUM_10

/* Default audio settings */
#define AUDIO_SAMPLE_RATE   16000

#endif /* BOARD_PINS_H */
