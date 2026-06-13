#ifndef __TASK_UART_EVENTS_H__
#define __TASK_UART_EVENTS_H__
#include "bsp_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include "bsp_gpio.h"

#define UART_QUEUE_SIZE    10
#define UART_BUFFER_SIZE   32
BaseType_t uart1_events_task_create(void);

#endif