#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "driver/gpio.h"

/* SPI2 bus (shared by LCD and TF card) */
#define PIN_SPI2_MOSI           GPIO_NUM_11
#define PIN_SPI2_CLK            GPIO_NUM_12
#define PIN_SPI2_MISO           GPIO_NUM_13

/* LCD ST7796 */
#define PIN_LCD_CS              GPIO_NUM_21
#define PIN_LCD_DC              GPIO_NUM_1

/* TF / microSD card */
#define PIN_TF_CS               GPIO_NUM_40

/* I2C0 (XL9555 GPIO expander) */
#define PIN_I2C0_SDA            GPIO_NUM_41
#define PIN_I2C0_SCL            GPIO_NUM_42

/* XL9555 I2C address */
#define XL9555_I2C_ADDR         0x20

/* XL9555 pin assignments for LCD control (port 1) */
#define XL9555_TFT_RES_PIN      3
#define XL9555_TFT_BLK_PIN      1

/* LCD resolution */
#define LCD_H_RES               320
#define LCD_V_RES               480

#endif /* BOARD_PINS_H */
