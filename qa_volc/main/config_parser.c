#include "config_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "config_parser";

/**
 * @brief Trim leading and trailing whitespace from a string in-place.
 *
 * Leading whitespace is skipped by advancing the pointer; trailing
 * whitespace is overwritten with a null terminator.  The function
 * returns a pointer into the original buffer (or to the original
 * null terminator if the string is all whitespace).
 */
static char *trim_whitespace(char *str)
{
    char *end;

    /* Trim leading space */
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return str;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    end[1] = '\0';

    return str;
}

esp_err_t config_parse(const char *filepath, config_t *cfg)
{
    FILE *f;
    char line[CONFIG_MAX_VAL_LEN];
    char *p, *key, *val;

    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg->count = 0;

    if (filepath == NULL) {
        ESP_LOGE(TAG, "filepath is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open '%s'", filepath);
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), f) != NULL && cfg->count < CONFIG_MAX_ENTRIES) {
        /* Remove trailing newline / carriage return */
        p = strchr(line, '\n');
        if (p) *p = '\0';
        p = strchr(line, '\r');
        if (p) *p = '\0';

        /* Skip leading whitespace before any checks */
        p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }

        /* Find the '=' separator */
        key = p;
        p = strchr(key, '=');
        if (p == NULL) {
            ESP_LOGW(TAG, "Skipping malformed line (no '='): '%s'", key);
            continue;
        }

        /* Split key and value */
        *p = '\0';
        val = p + 1;

        /* Trim whitespace from both */
        key = trim_whitespace(key);
        if (*key == '\0') {
            ESP_LOGW(TAG, "Skipping line with empty key");
            continue;
        }

        val = trim_whitespace(val);

        /* Store the entry (truncated if necessary) */
        strncpy(cfg->entries[cfg->count].key, key, CONFIG_MAX_KEY_LEN - 1);
        cfg->entries[cfg->count].key[CONFIG_MAX_KEY_LEN - 1] = '\0';

        strncpy(cfg->entries[cfg->count].val, val, CONFIG_MAX_VAL_LEN - 1);
        cfg->entries[cfg->count].val[CONFIG_MAX_VAL_LEN - 1] = '\0';

        cfg->count++;
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d config entr%s from '%s'",
             cfg->count, cfg->count == 1 ? "y" : "ies", filepath);
    return ESP_OK;
}

const char *config_get_string(const config_t *cfg, const char *key, const char *default_val)
{
    if (cfg == NULL || key == NULL) {
        return default_val;
    }

    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].val;
        }
    }

    return default_val;
}

int config_get_int(const config_t *cfg, const char *key, int default_val)
{
    const char *val = config_get_string(cfg, key, NULL);
    if (val == NULL) {
        return default_val;
    }

    char *endptr;
    long result = strtol(val, &endptr, 10);

    if (*endptr != '\0') {
        ESP_LOGW(TAG, "Key '%s' has non-numeric value '%s', returning default (%d)",
                 key, val, default_val);
        return default_val;
    }

    return (int)result;
}
