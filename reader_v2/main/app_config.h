#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char tts_appid[32];
    char tts_token[256];
    char tts_voice[64];
    char tts_cluster[32];
    char tts_resource_id[64];
    char tts_api_key[256];
} app_config_t;

/* Global config instance — defined in main.c */
extern app_config_t g_app_config;

esp_err_t app_config_load(app_config_t *cfg);

#endif
