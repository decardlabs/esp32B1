#ifndef __TASK_LED_C_H__
#define __TASK_LED_C_H__
#include "bsp_ledc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

BaseType_t ledc_task_create(void);


#endif