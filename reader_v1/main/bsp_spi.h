#ifndef BSP_SPI_H
#define BSP_SPI_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "board_pins.h"

#define BSP_LCD_SPI_HOST             SPI2_HOST
#define BSP_LCD_SPI_MOSI_IO          BSP_GPIO_SPI2_MOSI
#define BSP_LCD_SPI_CLK_IO           BSP_GPIO_SPI2_CLK
#define BSP_LCD_SPI_MISO_IO          BSP_GPIO_SPI2_MISO
#define BSP_LCD_SPI_MAX_TRANSFER_SZ  (LCD_H_RES * 40 * sizeof(uint16_t))

esp_err_t bsp_spi2_lcd_init(void);
bool bsp_spi2_lcd_is_initialized(void);
spi_host_device_t bsp_get_spi2_lcd_host(void);

esp_err_t bsp_spi2_bus_lock(TickType_t timeout);
void bsp_spi2_bus_unlock(void);
bool bsp_spi2_bus_unlock_from_isr(void);

#endif
