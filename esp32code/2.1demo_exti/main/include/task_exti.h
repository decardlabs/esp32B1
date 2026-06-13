#ifndef __TASK_EXTI_PROC_H__
#define __TASK_EXTI_PROC_H__
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_gpio.h"
#include "bsp_exti.h"


static void exti_proc_task(void *pvParameters);
BaseType_t exti_proc_task_create(void);

#endif
