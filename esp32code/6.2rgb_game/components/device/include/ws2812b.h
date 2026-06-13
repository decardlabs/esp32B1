#ifndef __WS2812B_H__
#define __WS2812B_H__
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "esp_err.h"



#define LED_STRIP_USE_DMA  0

#define LED_STRIP_LED_COUNT 60
#define LED_STRIP_MEMORY_BLOCK_WORDS 64

#define LED_STRIP_GPIO_PIN  0
#define LED_STRIP_RMT_RES_HZ  (10 * 1000 * 1000)

void effect_breathing(uint8_t r, uint8_t g, uint8_t b);
void effect_rainbow(void); 
void effect_comet(void); 
led_strip_handle_t ws2812_init(void);



#endif
