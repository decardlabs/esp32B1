#ifndef APP_MATTER_H
#define APP_MATTER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the Matter stack with On/Off Light endpoint */
esp_err_t app_matter_start(void);

/** Trigger Matter factory reset (clears all fabrics and restarts) */
void app_matter_factory_reset(void);

/** Get the manual setup code for display */
const char *app_matter_get_manual_code(void);

/** Get the QR code setup payload string */
const char *app_matter_get_qr_code(void);

#ifdef __cplusplus
}
#endif

#endif
