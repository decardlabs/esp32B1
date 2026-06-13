#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include "sdmmc_cmd.h"

/**
 * Initialize SD card on the given SPI host.
 * The SPI bus must already be initialized (by LCD init).
 * Returns the card handle via out_card.
 */
esp_err_t sd_card_init(int spi_host, sdmmc_card_t **out_card);

#endif /* SD_CARD_H */
