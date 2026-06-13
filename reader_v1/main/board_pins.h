#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "driver/gpio.h"

/* SPI2 bus (shared by LCD and TF card) */
#define PIN_SPI2_MOSI           GPIO_NUM_11
#define PIN_SPI2_CLK            GPIO_NUM_12
#define PIN_SPI2_MISO           GPIO_NUM_13

/* BSP-compatible aliases for SPI */
#define BSP_GPIO_SPI2_MOSI      PIN_SPI2_MOSI
#define BSP_GPIO_SPI2_CLK       PIN_SPI2_CLK
#define BSP_GPIO_SPI2_MISO      PIN_SPI2_MISO

/* LCD ST7796 */
#define PIN_LCD_CS              GPIO_NUM_21
#define PIN_LCD_DC              GPIO_NUM_1
#define LCD_H_RES               320
#define LCD_V_RES               480

/* BSP-compatible aliases for LCD */
#define BSP_GPIO_LCD_CS         PIN_LCD_CS
#define BSP_GPIO_LCD_DC         PIN_LCD_DC

/* TF / microSD card - CS shares GPIO40 with KEY0 on PCB */
#define PIN_TF_CS               GPIO_NUM_40

/* I2C0 (XL9555 GPIO expander + ES8388 audio codec) */
#define PIN_I2C0_SDA            GPIO_NUM_41
#define PIN_I2C0_SCL            GPIO_NUM_42

/* I2C device addresses on I2C0 */
#define XL9555_I2C_ADDR         0x20
#define ES8388_I2C_ADDR         0x10

/* XL9555 pin assignments (port 1 - LCD control) */
#define XL9555_TFT_RES_PIN      3
#define XL9555_TFT_BLK_PIN      1

/* XL9555 pin assignments (port 0 - speaker) */
#define XL9555_SPK_PIN          0
#define XL9555_BEEP_PIN         1

/* I2S0 for audio playback to ES8388 */
#define PIN_I2S0_MCLK           GPIO_NUM_3
#define PIN_I2S0_BCLK           GPIO_NUM_46
#define PIN_I2S0_LRCK           GPIO_NUM_9
#define PIN_I2S0_DOUT           GPIO_NUM_10

/* TTS audio sample rate */
#define TTS_SAMPLE_RATE         16000

#endif /* BOARD_PINS_H */
