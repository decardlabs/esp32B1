#ifndef __XL9555_H__
#define __XL9555_H__

#include "bsp_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define XL9555_I2C_ADDR      0x20         // 芯片I2C地址（A0/A1/A2均接GND时为0x20）

// -------------------------- 寄存器地址（手册9.2节，保留原有） --------------------------
#define XL9555_REG_INPUT_0   0x00    // 输入寄存器0（P00~P07）
#define XL9555_REG_INPUT_1   0x01    // 输入寄存器1（P10~P17）
#define XL9555_REG_OUTPUT_0  0x02    // 输出寄存器0（P00~P07）
#define XL9555_REG_OUTPUT_1  0x03    // 输出寄存器1（P10~P17）
#define XL9555_REG_POLARITY_0 0x04   // 极性反转寄存器0（可选，默认0不反转）
#define XL9555_REG_POLARITY_1 0x05   // 极性反转寄存器1（可选）
#define XL9555_REG_CONFIG_0  0x06    // 配置寄存器0（P0口方向：1=输入，0=输出）
#define XL9555_REG_CONFIG_1  0x07    // 配置寄存器1（P1口方向：1=输入，0=输出）

// -------------------------- 硬件引脚映射（保留原有，无需修改） --------------------------
#define LED_PORT  1
#define LED1_PIN   4
#define LED2_PIN   5
#define LED3_PIN   6
#define LED4_PIN   7


#define TXS0108_PORT   1
#define TXS0108_PIN    2

#define TFT_PORT   1
#define TFT_RES_PIN   3
#define TFT_BLK_PIN   1

#define KEY_PORT   0
#define KEY1_PIN   4
#define KEY2_PIN   5
#define KEY3_PIN   6
#define KEY4_PIN   7

#define OV_PORT   0
#define OV_RESET_PIN  3
#define OV_PWDN_PIN  2

#define BEEP_PORT   0
#define BEEP_PIN    1

#define SPK_PORT   0
#define SPK_PIN    0


void xl9555_set_pin_dir(uint8_t port, uint8_t pin, bool dir);
void xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level);
void xl9555_get_pin_level(uint8_t port, uint8_t pin, bool *level);
void xl9555_init(void);

#endif

