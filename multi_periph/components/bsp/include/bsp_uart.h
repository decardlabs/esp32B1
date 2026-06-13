#ifndef __BSP_UART_H__
#define __BSP_UART_H__
#include "driver/uart.h"
#include "bsp_board_pins.h"

#define UART_PORT   UART_NUM_1
#define UART_TX_PIN BSP_GPIO_UART1_TX
#define UART_RX_PIN BSP_GPIO_UART1_RX

void bsp_uart1_init(void);


#endif
