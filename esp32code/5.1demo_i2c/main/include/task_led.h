#ifndef __TASK_LED_H__
#define __TASK_LED_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_gpio.h"
#include "esp_log.h"
#include "bsp_gptimer.h"



void led_task(void *pvParameters);
TaskHandle_t led_static_task_create(void);
BaseType_t led_task_create(void);


#endif


