#ifndef __TASK_LEDC_H__
#define __TASK_LEDC_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "bsp_ledc.h"


BaseType_t ledc_task_create(void);

#endif