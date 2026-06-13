#ifndef __BSP_I2C_H__
#define __BSP_I2C_H__

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void bsp_i2c0_lock(void);
void bsp_i2c0_unlock(void);
void bsp_i2c0_init(void);
i2c_master_bus_handle_t bsp_get_i2c0_bus_handle(void);

#endif

