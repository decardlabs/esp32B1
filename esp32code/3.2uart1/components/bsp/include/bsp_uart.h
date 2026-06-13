#ifndef __BSP_UART_H__
#define __BSP_UART_H__
#include "driver/uart.h"
#include "driver/gpio.h"

#define UART1_PORT      UART_NUM_1
#define UART1_TXD_PIN   GPIO_NUM_11
#define UART1_RXD_PIN   GPIO_NUM_12

void bsp_uart1_init(void);

#endif
