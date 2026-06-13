#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include "sdmmc_cmd.h"

esp_err_t sd_card_init(sdmmc_card_t **out_card);
void sd_card_deinit(void);
sdmmc_card_t *sd_card_get_handle(void);

#endif
