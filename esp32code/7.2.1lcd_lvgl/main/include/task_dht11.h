#ifndef __TASK_DHT11_H__
#define __TASK_DHT11_H__


#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dht.h>
#include "esp_log.h"
#include "task_oled.h"

#define DHT_GPIO_PIN GPIO_NUM_1

BaseType_t dht11_task_create(void);

#endif

