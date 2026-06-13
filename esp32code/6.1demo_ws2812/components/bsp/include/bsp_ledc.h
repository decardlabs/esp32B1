#ifndef __BSP_LED_H__
#define __BSP_LED_H__
#include "driver/ledc.h"
#include "bsp_gpio.h"

typedef enum
{
    LEDC_DUTY_MODE  = 0,
    LEDC_FADE_MODE  = 1,
} ledc_state_t;

void bsp_ledc_init(ledc_state_t ledc_state);
void bsp_ledc_sg90_set_angle(uint32_t angle);
void bsp_ledc_sg90_init(void);

#endif
