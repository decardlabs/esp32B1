#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#include "esp_err.h"

/* Initialize command handler. Called when MQTT is ready. */
esp_err_t cmd_handler_init(void);

/* Process an incoming command payload */
void cmd_handler_process(const char *payload, int len);

#endif /* CMD_HANDLER_H */
