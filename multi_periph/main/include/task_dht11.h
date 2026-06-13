#ifndef __TASK_DHT11_H__
#define __TASK_DHT11_H__


#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dht.h>
#include "esp_log.h"
#include "task_oled.h"
#include "bsp_board_pins.h"

#define DHT_GPIO_PIN BSP_GPIO_DHT11_DATA

BaseType_t dht11_task_create(void);

#endif

