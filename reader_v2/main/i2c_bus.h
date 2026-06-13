#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t i2c_bus_init(void);
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/* I2C bus lock (shared by XL9555 and ES8388 on I2C0) */
void i2c_bus_lock(void);
void i2c_bus_unlock(void);

#endif
