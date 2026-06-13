#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "esp_err.h"

#define CONFIG_MAX_KEY_LEN   64
#define CONFIG_MAX_VAL_LEN   256
#define CONFIG_MAX_ENTRIES   32

typedef struct {
    char key[CONFIG_MAX_KEY_LEN];
    char val[CONFIG_MAX_VAL_LEN];
} config_entry_t;

typedef struct {
    config_entry_t entries[CONFIG_MAX_ENTRIES];
    int count;
} config_t;

esp_err_t config_parse(const char *filepath, config_t *cfg);
const char *config_get_string(const config_t *cfg, const char *key, const char *default_val);
int config_get_int(const config_t *cfg, const char *key, int default_val);

#endif
