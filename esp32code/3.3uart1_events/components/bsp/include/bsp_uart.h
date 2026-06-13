#ifndef __BSP_UART_H__
#define __BSP_UART_H__
#include "driver/uart.h"

#define UART_PORT   UART_NUM_1
#define UART_TX_PIN 17
#define UART_RX_PIN 18

void bsp_uart1_init(void);
void bsp_uart1_events_init(int uart_events_queue_size, QueueHandle_t *uart_events_queue_handle);

#endif
