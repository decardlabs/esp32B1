#ifndef __BSP_LEDC_H__
#define __BSP_LEDC_H__

#include "driver/ledc.h"
#include "bsp_gpio.h"
#include "esp_log.h"


#define SG90_FREQ_HZ        50
#define SG90_PERIOD_US      (1000000 / SG90_FREQ_HZ)   // 20000us
#define SG90_MIN_US         500
#define SG90_MAX_US         2500
#define SG90_MAX_ANGLE      180


#define SG90_DUTY_RES       14
#define SG90_DUTY_MAX       ((1U << SG90_DUTY_RES) - 1)

void bsp_ledc_duty_init(void);
void bsp_ledc_fade_init(void);

uint32_t sg90_us_to_duty(uint32_t pulse_us);
esp_err_t bsp_ledc_sg90_set_pulse_us(uint32_t pulse_us);
esp_err_t bsp_ledc_sg90_set_angle(uint8_t angle);
void bsp_ledc_sg90_init(void);

#endif

