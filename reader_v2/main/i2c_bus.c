#include "i2c_bus.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static i2c_master_bus_handle_t s_bus = NULL;
static SemaphoreHandle_t s_bus_mutex = NULL;
static StaticSemaphore_t s_bus_mutex_buffer;

esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL) return ESP_OK;

    s_bus_mutex = xSemaphoreCreateMutexStatic(&s_bus_mutex_buffer);
    if (!s_bus_mutex) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C0_SCL,
        .sda_io_num = PIN_I2C0_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

void i2c_bus_lock(void)
{
    if (s_bus_mutex) {
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (s_bus_mutex) {
        xSemaphoreGive(s_bus_mutex);
    }
}
