#include "bsp_i2c.h"


i2c_master_bus_handle_t i2c0_bus_handle = NULL;
SemaphoreHandle_t i2c0_bus_mutex = NULL;

void i2c0_bus_mutex_init(void)
{
    i2c0_bus_mutex = xSemaphoreCreateMutex();
}

void bsp_i2c0_lock(void)
{
    xSemaphoreTake(i2c0_bus_mutex, portMAX_DELAY);
}

void bsp_i2c0_unlock(void)
{
    xSemaphoreGive(i2c0_bus_mutex);
}

void bsp_i2c0_init(void)
{
    i2c0_bus_mutex_init();
    i2c_master_bus_config_t i2c0_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = GPIO_NUM_42,
        .sda_io_num = GPIO_NUM_41,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    i2c_new_master_bus(&i2c0_bus_cfg, &i2c0_bus_handle);
}

i2c_master_bus_handle_t bsp_get_i2c0_bus_handle(void)
{
    return i2c0_bus_handle;
}