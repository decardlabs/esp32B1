#ifndef LCD_ST7796_H
#define LCD_ST7796_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize SPI2 bus, add LCD device, init ST7796 panel,
 * display static text, then turn on backlight.
 * After this call, SPI2 bus is released and available for SD card.
 */
esp_err_t lcd_init_and_show_text(void);

/**
 * Get the SPI host device used by LCD (for SD card to share).
 */
int lcd_get_spi_host(void);

#endif /* LCD_ST7796_H */
