#include "xl9555_io.h"

#include <stddef.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define XL9555_I2C_ADDR        0x20
#define XL9555_I2C_PORT        I2C_NUM_0
#define XL9555_I2C_SCL_GPIO    42
#define XL9555_I2C_SDA_GPIO    41
#define XL9555_I2C_FREQ_HZ     400000
#define XL9555_I2C_TIMEOUT_MS  50
#define XL9555_I2C_RETRY_COUNT 3
#define XL9555_IO_LOCK_MS      50

#define XL9555_REG_INPUT_0     0x00
#define XL9555_REG_INPUT_1     0x01
#define XL9555_REG_OUTPUT_0    0x02
#define XL9555_REG_OUTPUT_1    0x03
#define XL9555_REG_CONFIG_0    0x06
#define XL9555_REG_CONFIG_1    0x07

#define XL9555_DIR_OUTPUT      0
#define XL9555_DIR_INPUT       1

static const char *TAG = "XL9555_IO";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_xl9555 = NULL;
static SemaphoreHandle_t s_io_lock = NULL;

static esp_err_t xl9555_lock(void)
{
    ESP_RETURN_ON_FALSE(s_io_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "io lock not ready");
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(XL9555_IO_LOCK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void xl9555_unlock(void)
{
    if (s_io_lock != NULL) {
        (void)xSemaphoreGive(s_io_lock);
    }
}

static esp_err_t xl9555_transmit_with_retry(const uint8_t *buf, size_t len)
{
    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < XL9555_I2C_RETRY_COUNT; ++i) {
        ret = i2c_master_transmit(s_xl9555, buf, len, XL9555_I2C_TIMEOUT_MS);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }

    return ret;
}

static esp_err_t xl9555_transmit_receive_with_retry(const uint8_t *tx_buf,
                                                    size_t tx_len,
                                                    uint8_t *rx_buf,
                                                    size_t rx_len)
{
    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < XL9555_I2C_RETRY_COUNT; ++i) {
        ret = i2c_master_transmit_receive(
            s_xl9555, tx_buf, tx_len, rx_buf, rx_len, XL9555_I2C_TIMEOUT_MS);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    }

    return ret;
}

static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx_buf[2] = {reg, value};

    ESP_RETURN_ON_FALSE(s_xl9555 != NULL, ESP_ERR_INVALID_STATE, TAG, "xl9555 not initialized");
    return xl9555_transmit_with_retry(tx_buf, sizeof(tx_buf));
}

static esp_err_t xl9555_read_reg(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE((s_xl9555 != NULL) && (value != NULL), ESP_ERR_INVALID_ARG, TAG, "invalid read arg");
    return xl9555_transmit_receive_with_retry(&reg, 1, value, 1);
}

static esp_err_t xl9555_set_pin_dir(uint8_t port, uint8_t pin, uint8_t dir)
{
    uint8_t reg_addr = (port == 0) ? XL9555_REG_CONFIG_0 : XL9555_REG_CONFIG_1;
    uint8_t reg_val = 0;

    ESP_RETURN_ON_FALSE((port <= 1) && (pin <= 7), ESP_ERR_INVALID_ARG, TAG, "invalid pin");
    ESP_RETURN_ON_ERROR(xl9555_read_reg(reg_addr, &reg_val), TAG, "read config failed");

    if (dir == XL9555_DIR_INPUT) {
        reg_val |= (1U << pin);
    } else {
        reg_val &= (uint8_t)~(1U << pin);
    }

    return xl9555_write_reg(reg_addr, reg_val);
}

static esp_err_t xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level)
{
    uint8_t reg_addr = (port == 0) ? XL9555_REG_OUTPUT_0 : XL9555_REG_OUTPUT_1;
    uint8_t reg_val = 0;

    ESP_RETURN_ON_FALSE((port <= 1) && (pin <= 7), ESP_ERR_INVALID_ARG, TAG, "invalid pin");
    ESP_RETURN_ON_ERROR(xl9555_read_reg(reg_addr, &reg_val), TAG, "read output failed");

    if (level) {
        reg_val |= (1U << pin);
    } else {
        reg_val &= (uint8_t)~(1U << pin);
    }

    return xl9555_write_reg(reg_addr, reg_val);
}

static esp_err_t xl9555_get_pin_level(uint8_t port, uint8_t pin, bool *level)
{
    uint8_t reg_addr = (port == 0) ? XL9555_REG_INPUT_0 : XL9555_REG_INPUT_1;
    uint8_t reg_val = 0;

    ESP_RETURN_ON_FALSE((port <= 1) && (pin <= 7) && (level != NULL), ESP_ERR_INVALID_ARG, TAG, "invalid pin");
    ESP_RETURN_ON_ERROR(xl9555_read_reg(reg_addr, &reg_val), TAG, "read input failed");

    *level = (reg_val & (1U << pin)) != 0;
    return ESP_OK;
}

esp_err_t xl9555_io_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_io_lock != NULL, ESP_ERR_NO_MEM, TAG, "create io lock failed");
    }

    ESP_RETURN_ON_ERROR(xl9555_lock(), TAG, "io lock failed");

    if (s_xl9555 != NULL) {
        xl9555_unlock();
        return ESP_OK;
    }

    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = XL9555_I2C_PORT,
            .scl_io_num = XL9555_I2C_SCL_GPIO,
            .sda_io_num = XL9555_I2C_SDA_GPIO,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (ret != ESP_OK) {
            xl9555_unlock();
            ESP_LOGE(TAG, "new i2c bus failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = XL9555_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9555);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "add xl9555 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure required keys as input. */
    ret = xl9555_set_pin_dir(key1.port, key1.pin, XL9555_DIR_INPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "key1 input cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_dir(key2.port, key2.pin, XL9555_DIR_INPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "key2 input cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_dir(key3.port, key3.pin, XL9555_DIR_INPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "key3 input cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure LEDs and beeper as output and default to off (active-low). */
    ret = xl9555_set_pin_dir(led1.port, led1.pin, XL9555_DIR_OUTPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led1 output cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_dir(led2.port, led2.pin, XL9555_DIR_OUTPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led2 output cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_dir(led3.port, led3.pin, XL9555_DIR_OUTPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led3 output cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_dir(beep.port, beep.pin, XL9555_DIR_OUTPUT);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "beep output cfg failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = xl9555_set_pin_level(led1.port, led1.pin, 1);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led1 off failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_level(led2.port, led2.pin, 1);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led2 off failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_level(led3.port, led3.pin, 1);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "led3 off failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_set_pin_level(beep.port, beep.pin, 1);
    if (ret != ESP_OK) {
        xl9555_unlock();
        ESP_LOGE(TAG, "beep off failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xl9555_unlock();

    return ESP_OK;
}

esp_err_t xl9555_io_key_is_pressed(const io_map_t *key, bool *pressed)
{
    bool level = true;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE((key != NULL) && (pressed != NULL), ESP_ERR_INVALID_ARG, TAG, "invalid key arg");
    ESP_RETURN_ON_ERROR(xl9555_lock(), TAG, "io lock failed");
    ret = xl9555_get_pin_level(key->port, key->pin, &level);
    xl9555_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "read key failed");
    *pressed = (level == 0);
    return ESP_OK;
}

esp_err_t xl9555_io_led_set(const io_map_t *led, bool on)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(led != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid led arg");
    ESP_RETURN_ON_ERROR(xl9555_lock(), TAG, "io lock failed");
    ret = xl9555_set_pin_level(led->port, led->pin, on ? 0 : 1);
    xl9555_unlock();
    return ret;
}

esp_err_t xl9555_io_beep_set(const io_map_t *beep_io, bool on)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(beep_io != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid beep arg");
    ESP_RETURN_ON_ERROR(xl9555_lock(), TAG, "io lock failed");
    ret = xl9555_set_pin_level(beep_io->port, beep_io->pin, on ? 0 : 1);
    xl9555_unlock();
    return ret;
}
