#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__
#include "driver/gpio.h"

#define LED_GPIO  GPIO_NUM_1
#define KEY_GPIO  GPIO_NUM_40

void bsp_led_init(void);
void bsp_key_init(void);



#endif

