#ifndef __TASK_TIMER_H__
#define __TASK_TIMER_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

void esptimer_init(void);
void freertos_timer_init(void);
BaseType_t timer_task_create(void);


#endif

