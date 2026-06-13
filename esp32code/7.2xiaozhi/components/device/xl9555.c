#include "xl9555.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XL9555_REG_INPUT_0       0x00
#define XL9555_REG_INPUT_1       0x01
#define XL9555_REG_OUTPUT_0      0x02
#define XL9555_REG_OUTPUT_1      0x03
#define XL9555_REG_CONFIG_0      0x06
#define XL9555_REG_CONFIG_1      0x07

#define XL9555_DIR_OUTPUT        0
#define XL9555_DIR_INPUT         1

#define XL9555_LED_PORT          1
#define XL9555_LED1_PIN          4
#define XL9555_LED2_PIN          5
#define XL9555_LED3_PIN          6
#define XL9555_LED4_PIN          7

#define XL9555_TXS0108_PORT      1
#define XL9555_TXS0108_PIN       2

#define XL9555_TFT_PORT          1
#define XL9555_TFT_RES_PIN       3
#define XL9555_TFT_BLK_PIN       1

#define XL9555_KEY_PORT          0
#define XL9555_KEY1_PIN          4
#define XL9555_KEY2_PIN          5
#define XL9555_KEY3_PIN          6
#define XL9555_KEY4_PIN          7

#define XL9555_OV_PORT           0
#define XL9555_OV_RESET_PIN      3
#define XL9555_OV_PWDN_PIN       2

#define XL9555_BEEP_PORT         0
#define XL9555_BEEP_PIN          1

#define XL9555_SPK_PORT          0
#define XL9555_SPK_PIN           0
// MD8002A SHUTDOWN is active-high; drive SPK_EN low to enable the speaker amp.
#define XL9555_SPK_ENABLE_LEVEL  0

static const char *TAG = "XL9555";

static i2c_master_bus_handle_t xl9555_i2c0_bus_handle = NULL;
static i2c_master_dev_handle_t xl9555_dev_handle = NULL;

static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t tx_buf[2] = {reg, data};

    if (xl9555_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_i2c0_lock();
    esp_err_t ret = i2c_master_transmit(xl9555_dev_handle, tx_buf, sizeof(tx_buf), portMAX_DELAY);
    bsp_i2c0_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C Write Reg 0x%02x Failed! 0x%02x", reg, data);
    }
    return ret;
}

static esp_err_t xl9555_read_reg(uint8_t reg, uint8_t *data)
{
    if ((xl9555_dev_handle == NULL) || (data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_i2c0_lock();
    esp_err_t ret = i2c_master_transmit_receive(xl9555_dev_handle, &reg, 1, data, 1, portMAX_DELAY);
    bsp_i2c0_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C Read Reg 0x%02x Failed!", reg);
    }
    return ret;
}

static esp_err_t xl9555_set_pin_dir(uint8_t port, uint8_t pin, bool dir)
{
    uint8_t reg_addr;
    uint8_t pin_value;
    esp_err_t ret;

    if ((port > 1) || (pin > 7)) {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return ESP_ERR_INVALID_ARG;
    }

    reg_addr = (port == 0) ? XL9555_REG_CONFIG_0 : XL9555_REG_CONFIG_1;
    ret = xl9555_read_reg(reg_addr, &pin_value);
    if (ret != ESP_OK) {
        return ret;
    }

    if (dir) {
        pin_value |= (1 << pin);
    } else {
        pin_value &= ~(1 << pin);
    }

    return xl9555_write_reg(reg_addr, pin_value);
}

static esp_err_t xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level)
{
    uint8_t reg_addr;
    uint8_t pin_value;
    esp_err_t ret;

    if ((port > 1) || (pin > 7)) {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return ESP_ERR_INVALID_ARG;
    }

    reg_addr = (port == 0) ? XL9555_REG_OUTPUT_0 : XL9555_REG_OUTPUT_1;
    ret = xl9555_read_reg(reg_addr, &pin_value);
    if (ret != ESP_OK) {
        return ret;
    }

    if (level) {
        pin_value |= (1 << pin);
    } else {
        pin_value &= ~(1 << pin);
    }

    return xl9555_write_reg(reg_addr, pin_value);
}

static esp_err_t xl9555_get_pin_level(uint8_t port, uint8_t pin, bool *level)
{
    uint8_t reg_addr;
    uint8_t pin_value;
    esp_err_t ret;

    if ((port > 1) || (pin > 7) || (level == NULL)) {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return ESP_ERR_INVALID_ARG;
    }

    reg_addr = (port == 0) ? XL9555_REG_INPUT_0 : XL9555_REG_INPUT_1;
    ret = xl9555_read_reg(reg_addr, &pin_value);
    if (ret != ESP_OK) {
        return ret;
    }

    *level = (pin_value & (1 << pin)) != 0;
    return ESP_OK;
}

static esp_err_t xl9555_output_init(uint8_t port, uint8_t pin, bool level)
{
    esp_err_t ret = xl9555_set_pin_dir(port, pin, XL9555_DIR_OUTPUT);

    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_set_pin_level(port, pin, level);
}

static esp_err_t xl9555_input_init(uint8_t port, uint8_t pin)
{
    return xl9555_set_pin_dir(port, pin, XL9555_DIR_INPUT);
}

static esp_err_t xl9555_get_led_pin(xl9555_board_led_t led, uint8_t *pin)
{
    if (pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (led) {
    case XL9555_BOARD_LED1:
        *pin = XL9555_LED1_PIN;
        return ESP_OK;
    case XL9555_BOARD_LED2:
        *pin = XL9555_LED2_PIN;
        return ESP_OK;
    case XL9555_BOARD_LED3:
        *pin = XL9555_LED3_PIN;
        return ESP_OK;
    case XL9555_BOARD_LED4:
        *pin = XL9555_LED4_PIN;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t xl9555_get_key_pin(xl9555_key_t key, uint8_t *pin)
{
    if (pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (key) {
    case XL9555_KEY1:
        *pin = XL9555_KEY1_PIN;
        return ESP_OK;
    case XL9555_KEY2:
        *pin = XL9555_KEY2_PIN;
        return ESP_OK;
    case XL9555_KEY3:
        *pin = XL9555_KEY3_PIN;
        return ESP_OK;
    case XL9555_KEY4:
        *pin = XL9555_KEY4_PIN;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t xl9555_init(void)
{
    esp_err_t ret;

    if (xl9555_dev_handle != NULL) {
        return ESP_OK;
    }

    xl9555_i2c0_bus_handle = bsp_get_i2c0_bus_handle();
    if (xl9555_i2c0_bus_handle == NULL) {
        ESP_LOGE(TAG, "call bsp_i2c0_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t xl9555_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };

    ret = i2c_master_bus_add_device(xl9555_i2c0_bus_handle, &xl9555_dev_cfg, &xl9555_dev_handle);
    if (ret != ESP_OK) {
        xl9555_dev_handle = NULL;
        ESP_LOGE(TAG, "add i2c device failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

bool xl9555_is_initialized(void)
{
    return xl9555_dev_handle != NULL;
}

esp_err_t xl9555_lcd_gpio_init(void)
{
    esp_err_t ret = xl9555_output_init(XL9555_TFT_PORT, XL9555_TFT_RES_PIN, 1);

    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_output_init(XL9555_TFT_PORT, XL9555_TFT_BLK_PIN, 0);
}

esp_err_t xl9555_lcd_reset(void)
{
    esp_err_t ret = xl9555_set_pin_level(XL9555_TFT_PORT, XL9555_TFT_RES_PIN, 0);

    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = xl9555_set_pin_level(XL9555_TFT_PORT, XL9555_TFT_RES_PIN, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

esp_err_t xl9555_lcd_backlight_on(void)
{
    return xl9555_set_pin_level(XL9555_TFT_PORT, XL9555_TFT_BLK_PIN, 1);
}

esp_err_t xl9555_lcd_backlight_off(void)
{
    return xl9555_set_pin_level(XL9555_TFT_PORT, XL9555_TFT_BLK_PIN, 0);
}

esp_err_t xl9555_ws2812_level_shifter_enable(void)
{
    return xl9555_output_init(XL9555_TXS0108_PORT, XL9555_TXS0108_PIN, 1);
}

esp_err_t xl9555_board_led_set(xl9555_board_led_t led, bool on)
{
    uint8_t pin;
    esp_err_t ret = xl9555_get_led_pin(led, &pin);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = xl9555_set_pin_dir(XL9555_LED_PORT, pin, XL9555_DIR_OUTPUT);
    if (ret != ESP_OK) {
        return ret;
    }

    return xl9555_set_pin_level(XL9555_LED_PORT, pin, !on);
}

esp_err_t xl9555_keys_gpio_init(void)
{
    esp_err_t ret = xl9555_input_init(XL9555_KEY_PORT, XL9555_KEY1_PIN);

    if (ret != ESP_OK) {
        return ret;
    }
    ret = xl9555_input_init(XL9555_KEY_PORT, XL9555_KEY2_PIN);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = xl9555_input_init(XL9555_KEY_PORT, XL9555_KEY3_PIN);
    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_input_init(XL9555_KEY_PORT, XL9555_KEY4_PIN);
}

esp_err_t xl9555_key_is_pressed(xl9555_key_t key, bool *pressed)
{
    uint8_t pin;
    bool level;
    esp_err_t ret;

    if (pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = xl9555_get_key_pin(key, &pin);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = xl9555_get_pin_level(XL9555_KEY_PORT, pin, &level);
    if (ret != ESP_OK) {
        return ret;
    }

    *pressed = (level == 0);
    return ESP_OK;
}

esp_err_t xl9555_beep_set(bool on)
{
    esp_err_t ret = xl9555_set_pin_dir(XL9555_BEEP_PORT, XL9555_BEEP_PIN, XL9555_DIR_OUTPUT);

    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_set_pin_level(XL9555_BEEP_PORT, XL9555_BEEP_PIN, !on);
}

esp_err_t xl9555_camera_gpio_init(void)
{
    esp_err_t ret = xl9555_output_init(XL9555_OV_PORT, XL9555_OV_RESET_PIN, 0);

    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_output_init(XL9555_OV_PORT, XL9555_OV_PWDN_PIN, 1);
}

esp_err_t xl9555_audio_gpio_init(void)
{
    esp_err_t ret = xl9555_speaker_set(true);

    if (ret != ESP_OK) {
        return ret;
    }
    return xl9555_beep_set(false);
}

esp_err_t xl9555_speaker_set(bool on)
{
    return xl9555_output_init(XL9555_SPK_PORT,
                              XL9555_SPK_PIN,
                              on ? XL9555_SPK_ENABLE_LEVEL : !XL9555_SPK_ENABLE_LEVEL);
}
