#ifndef __BSP_I2C_H__
#define __BSP_I2C_H__

#include "driver/i2c_master.h"

void bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);


#endif

