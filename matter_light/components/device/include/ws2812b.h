#ifndef __WS2812B_H__
#define __WS2812B_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "esp_err.h"
#include "esp_check.h"
#include "bsp_board_pins.h"

#define LED_STRIP_USE_DMA  0

#define LED_STRIP_LED_COUNT 4
#define LED_STRIP_MEMORY_BLOCK_WORDS 0

#define LED_STRIP_GPIO_PIN  BSP_GPIO_WS2812_DATA
#define LED_STRIP_RMT_RES_HZ  (10 * 1000 * 1000)

led_strip_handle_t ws2812_init(void);
esp_err_t ws2812_set_all(uint8_t r, uint8_t g, uint8_t b);
esp_err_t ws2812_clear(void);

#endif
