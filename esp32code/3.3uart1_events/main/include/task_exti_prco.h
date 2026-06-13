#ifndef __TASK_EXTI_PRCO_H__
#define __TASK_EXTI_PRCO_H__
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_gpio.h"
#include "bsp_exti.h"
#include "esp_log.h"

void exti_task_prco(void *pvParameters);
BaseType_t exti_task_prco_create(void);

#endif
