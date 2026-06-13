#include "bsp_i2c.h"

i2c_master_bus_handle_t bus_handle = NULL;
void bsp_i2c_init(void)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = GPIO_NUM_42,
        .sda_io_num = GPIO_NUM_41,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    i2c_new_master_bus(&i2c_bus_config, &bus_handle);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return bus_handle;
}