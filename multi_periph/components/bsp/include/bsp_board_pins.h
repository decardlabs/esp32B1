#ifndef __BSP_BOARD_PINS_H__
#define __BSP_BOARD_PINS_H__

#include "driver/gpio.h"

/* ESP32-S3 direct-connected pins (single source of truth). */
#define BSP_GPIO_BOOT_BTN            GPIO_NUM_0
#define BSP_GPIO_STATUS_LED          GPIO_NUM_1
#define BSP_GPIO_KEY0                GPIO_NUM_40

#define BSP_GPIO_DHT11_DATA          GPIO_NUM_1
#define BSP_GPIO_WS2812_DATA         GPIO_NUM_0

#define BSP_GPIO_UART1_TX            GPIO_NUM_17
#define BSP_GPIO_UART1_RX            GPIO_NUM_18

#define BSP_GPIO_I2C0_SDA            GPIO_NUM_41
#define BSP_GPIO_I2C0_SCL            GPIO_NUM_42

#define BSP_GPIO_SPI2_MOSI           GPIO_NUM_11
#define BSP_GPIO_SPI2_CLK            GPIO_NUM_12
#define BSP_GPIO_SPI2_MISO           GPIO_NUM_13

#define BSP_GPIO_LCD_DC              GPIO_NUM_1
#define BSP_GPIO_LCD_CS              GPIO_NUM_21
#define BSP_GPIO_TOUCH_CS            GPIO_NUM_2
#define BSP_GPIO_TOUCH_IRQ           GPIO_NUM_8

#define BSP_GPIO_TF_SD_CS            GPIO_NUM_40

#define BSP_GPIO_CAMERA_D6           GPIO_NUM_17
#define BSP_GPIO_CAMERA_D7           GPIO_NUM_18

#endif