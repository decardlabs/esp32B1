#ifndef __BSP_LEDC_H__
#define __BSP_LEDC_H__

#include "driver/ledc.h"
#include "bsp_gpio.h"
#include "esp_log.h"


typedef enum {
    LEDC_DUTY_MODE = 0,
    LEDC_FADE_MODE,
} led_state_t;

void bsp_ledc_init(led_state_t ledc_mode);

void bsp_ledc_sg90_set_angle(uint8_t angle);
void bsp_ledc_sg90_init(void);

#endif

