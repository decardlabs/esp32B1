#ifndef __WS2812B_H__
#define __WS2812B_H__
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "esp_err.h"



#define LED_STRIP_USE_DMA  0

#define LED_STRIP_LED_COUNT 4
#define LED_STRIP_MEMORY_BLOCK_WORDS 0 

#define LED_STRIP_GPIO_PIN  0
#define LED_STRIP_RMT_RES_HZ  (10 * 1000 * 1000)

esp_err_t ws2812_clear_all(void);
esp_err_t ws2812_show_color(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
esp_err_t ws2812_show_breathing_step(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, uint16_t phase);
esp_err_t ws2812_show_rainbow_step(uint8_t brightness, uint16_t base_hue);
esp_err_t ws2812_show_comet_step(uint8_t brightness, int head);

void effect_breathing(uint8_t r, uint8_t g, uint8_t b);
void effect_rainbow(void); 
void effect_comet(void); 
led_strip_handle_t ws2812_init(void);
bool ws2812_is_initialized(void);



#endif
