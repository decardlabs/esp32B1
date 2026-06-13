#ifndef __TASK_OLED_H__
#define __TASK_OLED_H__

#include "bsp_i2c.h"
#include "ssd1306.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define OLED_TEXT_MAX_LEN 10

typedef enum {
    OLED_MSG_TYPE_DHT11 = 0,
    OLED_MSG_TYPE_STR = 1,
} oled_msg_type_t;

typedef struct {
    oled_msg_type_t type;
    union {
        struct {
            uint16_t temp;
            uint16_t humi;
        } dht11;
        struct {
            char text[OLED_TEXT_MAX_LEN];
            uint8_t len;
        } str;
    } u_data;
} oled_msg_t;
    

void oled_send_msg(const oled_msg_t *msg, TickType_t timeout);
BaseType_t oled_task_create(void);





#endif