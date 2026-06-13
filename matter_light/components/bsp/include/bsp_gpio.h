#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__
#include "driver/gpio.h"
#include "bsp_board_pins.h"

#define BOOT_GPIO  BSP_GPIO_BOOT_BTN
#define LED_GPIO   BSP_GPIO_STATUS_LED
#define KEY_GPIO   BSP_GPIO_KEY0

void bsp_led_init(void);
void bsp_key_init(void);
int  bsp_gpio_get_level(gpio_num_t gpio_num);
esp_err_t bsp_gpio_set_level(gpio_num_t gpio_num, uint32_t level); 
void bsp_gpio_toggle_level(gpio_num_t gpio_num);   

#endif

