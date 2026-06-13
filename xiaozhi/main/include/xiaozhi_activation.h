#ifndef __XIAOZHI_ACTIVATION_H__
#define __XIAOZHI_ACTIVATION_H__

#include "esp_err.h"

#define XIAOZHI_DEVICE_ID_LEN   24
#define XIAOZHI_CLIENT_ID_LEN   40
#define XIAOZHI_WS_URL_LEN      256
#define XIAOZHI_WS_TOKEN_LEN    512

typedef struct {
    char device_id[XIAOZHI_DEVICE_ID_LEN];
    char client_id[XIAOZHI_CLIENT_ID_LEN];
    char ws_url[XIAOZHI_WS_URL_LEN];
    char ws_token[XIAOZHI_WS_TOKEN_LEN];
    int ws_version;
} xiaozhi_server_config_t;

esp_err_t xiaozhi_activation_prepare(xiaozhi_server_config_t *config);

#endif
