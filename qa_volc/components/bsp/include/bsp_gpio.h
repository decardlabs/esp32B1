#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__
#include "driver/gpio.h"

#define BOOT_GPIO  GPIO_NUM_0
#define LED_GPIO  GPIO_NUM_1
#define KEY_GPIO  GPIO_NUM_40

void bsp_led_init(void);
void bsp_key_init(void);
int  bsp_gpio_get_level(gpio_num_t gpio_num);
esp_err_t bsp_gpio_set_level(gpio_num_t gpio_num, uint32_t level); 
void bsp_gpio_toggle_level(gpio_num_t gpio_num);   

#endif

