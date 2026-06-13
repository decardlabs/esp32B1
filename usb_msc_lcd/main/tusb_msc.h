#ifndef TUSB_MSC_H
#define TUSB_MSC_H

#include "esp_err.h"
#include "sdmmc_cmd.h"

/**
 * Initialize TinyUSB MSC device using the given SD card.
 * The card must already be initialized.
 */
esp_err_t tusb_msc_init(sdmmc_card_t *card);

#endif /* TUSB_MSC_H */
