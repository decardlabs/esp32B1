#ifndef TUSB_MSC_H
#define TUSB_MSC_H

#include "esp_err.h"
#include "sdmmc_cmd.h"

/* Callback when host writes new data to the MSC volume */
typedef void (*tusb_msc_write_cb_t)(void);

esp_err_t tusb_msc_init(sdmmc_card_t *card);
void tusb_msc_set_write_callback(tusb_msc_write_cb_t cb);
void tusb_msc_reset_eject(void);
void tusb_msc_disconnect(void);
void tusb_msc_connect(void);
bool tusb_msc_is_connected(void);
void tusb_msc_suspend(void);
void tusb_msc_resume(void);

#endif
