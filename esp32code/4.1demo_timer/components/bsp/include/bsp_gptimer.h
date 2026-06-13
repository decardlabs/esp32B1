#ifndef __BSP_GPTIMER_H__
#define __BSP_GPTIMER_H__

#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


void bsp_gptimer_init(QueueHandle_t queue_handle);

#endif

