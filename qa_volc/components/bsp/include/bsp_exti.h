#ifndef __BSP_EXTI_H__
#define __BSP_EXTI_H__
#include "bsp_gpio.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

void bsp_exti_init(gpio_num_t gpio_num);    
void bsp_exti_register_task_handle(TaskHandle_t task_handle);
#endif

