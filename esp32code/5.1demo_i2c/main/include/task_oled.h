#ifndef __TASK_OLED_H__
#define __TASK_OLED_H__

#include "bsp_i2c.h"
#include "ssd1306.h"
// #include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

BaseType_t oled_task_create(void);

#endif

