#include "xl9555.h"
#include "i2c_bus.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* (TAG unused - LOG not needed for this driver) */static i2c_master_dev_handle_t s_dev = NULL;

/* Register map */
#define REG_INPUT_0     0x00
#define REG_INPUT_1     0x01
#define REG_OUTPUT_0    0x02
#define REG_OUTPUT_1    0x03
#define REG_CONFIG_0    0x06
#define REG_CONFIG_1    0x07

/* Key config on Port 0 pins 4-7 */
#define KEY_PORT        0
#define KEY1_PIN        4
#define KEY2_PIN        5
#define KEY3_PIN        6
#define KEY4_PIN        7

/* LCD control on Port 1 */
#define TFT_RES_PIN     3
#define TFT_BLK_PIN     1

/* Audio control on Port 0 (from PIN_MAP.md) */
#define AUDIO_PORT      0

static esp_err_t reg_write(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    i2c_bus_lock();
    esp_err_t ret = i2c_master_transmit(s_dev, buf, 2, portMAX_DELAY);
    i2c_bus_unlock();
    return ret;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data)
{
    i2c_bus_lock();
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, 1, portMAX_DELAY);
    i2c_bus_unlock();
    return ret;
}

static esp_err_t pin_set_dir_output(uint8_t port, uint8_t pin)
{
    uint8_t reg = (port == 0) ? REG_CONFIG_0 : REG_CONFIG_1;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    val &= ~(1 << pin); /* 0 = output */
    return reg_write(reg, val);
}

static esp_err_t pin_set_dir_input(uint8_t port, uint8_t pin)
{
    uint8_t reg = (port == 0) ? REG_CONFIG_0 : REG_CONFIG_1;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    val |= (1 << pin); /* 1 = input */
    return reg_write(reg, val);
}

static esp_err_t pin_set_level(uint8_t port, uint8_t pin, bool high)
{
    uint8_t reg = (port == 0) ? REG_OUTPUT_0 : REG_OUTPUT_1;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    if (high) val |= (1 << pin);
    else      val &= ~(1 << pin);
    return reg_write(reg, val);
}

esp_err_t xl9555_init(void)
{
    if (s_dev != NULL) return ESP_OK;
    if (i2c_bus_get_handle() == NULL) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(i2c_bus_get_handle(), &cfg, &s_dev);
}

esp_err_t xl9555_lcd_control_init(void)
{
    esp_err_t ret;
    ret = pin_set_dir_output(1, TFT_RES_PIN);
    if (ret != ESP_OK) return ret;
    ret = pin_set_dir_output(1, TFT_BLK_PIN);
    if (ret != ESP_OK) return ret;
    /* Default: RES=high, BLK=low */
    pin_set_level(1, TFT_RES_PIN, 1);
    pin_set_level(1, TFT_BLK_PIN, 0);
    return ESP_OK;
}

esp_err_t xl9555_lcd_reset(void)
{
    pin_set_level(1, TFT_RES_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    pin_set_level(1, TFT_RES_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t xl9555_backlight_on(void)
{
    return pin_set_level(1, TFT_BLK_PIN, 1);
}

esp_err_t xl9555_backlight_off(void)
{
    return pin_set_level(1, TFT_BLK_PIN, 0);
}

esp_err_t xl9555_speaker_enable(bool on)
{
    esp_err_t ret = pin_set_dir_output(AUDIO_PORT, XL9555_SPK_PIN);
    if (ret != ESP_OK) return ret;
    /* MD8002A SHUTDOWN is active-high: drive LOW to enable the speaker amp */
    return pin_set_level(AUDIO_PORT, XL9555_SPK_PIN, on ? 0 : 1);
}

esp_err_t xl9555_beep_enable(bool on)
{
    esp_err_t ret = pin_set_dir_output(AUDIO_PORT, XL9555_BEEP_PIN);
    if (ret != ESP_OK) return ret;
    /* Board wiring: BEEP is active-low (validated in test001). */
    return pin_set_level(AUDIO_PORT, XL9555_BEEP_PIN, on ? 0 : 1);
}

esp_err_t xl9555_key_init(void)
{
    esp_err_t ret;
    ret = pin_set_dir_input(KEY_PORT, KEY1_PIN); if (ret) return ret;
    ret = pin_set_dir_input(KEY_PORT, KEY2_PIN); if (ret) return ret;
    ret = pin_set_dir_input(KEY_PORT, KEY3_PIN); if (ret) return ret;
    ret = pin_set_dir_input(KEY_PORT, KEY4_PIN); if (ret) return ret;
    return ESP_OK;
}

esp_err_t xl9555_key_read(xl9555_key_t key, bool *pressed)
{
    if (pressed == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t pin;
    switch (key) {
    case XL9555_KEY1: pin = KEY1_PIN; break;
    case XL9555_KEY2: pin = KEY2_PIN; break;
    case XL9555_KEY3: pin = KEY3_PIN; break;
    case XL9555_KEY4: pin = KEY4_PIN; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg = REG_INPUT_0;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    *pressed = ((val & (1 << pin)) == 0);
    return ESP_OK;
}

const char *xl9555_key_name(xl9555_key_t key)
{
    switch (key) {
    case XL9555_KEY1: return "KEY1";
    case XL9555_KEY2: return "KEY2";
    case XL9555_KEY3: return "KEY3";
    case XL9555_KEY4: return "KEY4";
    default: return "?";
    }
}
