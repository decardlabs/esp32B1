#ifndef __TASK_UART_H__
#define __TASK_UART_H__
#include "bsp_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include "bsp_gpio.h"

// 协议常量
#define FRAME_HEAD      0xAA     // 帧头标识
#define FRAME_MAX_LEN   32       // 最大帧长度

// 状态机状态
typedef enum {
    STATE_WAIT_HEAD = 0,        // 等待帧头状态
    STATE_WAIT_LEN,             // 等待长度字段状态
    STATE_WAIT_DATA,            // 等待数据字段状态
    STATE_WAIT_CHECKSUM,        // 等待校验和状态
} proto_state_t;

void uart1_task(void *pvParameters);
BaseType_t uart1_task_create(void);  
uint8_t checksum_sum(const uint8_t *data, uint16_t len);
void process_frame(const uint8_t *data, uint16_t len);
void proto_parse_byte(uint8_t byte);

#endif

