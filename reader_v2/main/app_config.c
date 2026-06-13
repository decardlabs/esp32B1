#include "app_config.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sd_card.h"

static const char *TAG = "CONFIG";

static void trim_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ')) {
        s[--len] = '\0';
    }
}

static void trim_leading(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static esp_err_t parse_line(char *line, const char *key, char *value, size_t vmax)
{
    trim_leading(line);
    if (line[0] == '#' || line[0] == '\0') return ESP_ERR_NOT_FOUND;

    char *eq = strchr(line, '=');
    if (!eq) return ESP_ERR_NOT_FOUND;

    size_t klen = (size_t)(eq - line);
    if (klen != strlen(key)) return ESP_ERR_NOT_FOUND;
    if (strncmp(line, key, klen) != 0) return ESP_ERR_NOT_FOUND;

    const char *vstart = eq + 1;
    while (*vstart == ' ' || *vstart == '\t') vstart++;
    strncpy(value, vstart, vmax - 1);
    value[vmax - 1] = '\0';
    trim_newline(value);
    return ESP_OK;
}

esp_err_t app_config_load(app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));

    sd_card_lock();
    FILE *f = fopen("/sdcard/config.ini", "r");
    if (!f) {
        sd_card_unlock();
        ESP_LOGW(TAG, "config.ini not found on SD card");
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (parse_line(line, "WIFI_SSID", cfg->wifi_ssid, sizeof(cfg->wifi_ssid)) == ESP_OK) continue;
        if (parse_line(line, "WIFI_PASS", cfg->wifi_pass, sizeof(cfg->wifi_pass)) == ESP_OK) continue;
        if (parse_line(line, "TTS_APPID", cfg->tts_appid, sizeof(cfg->tts_appid)) == ESP_OK) continue;
        if (parse_line(line, "TTS_APP_ID", cfg->tts_appid, sizeof(cfg->tts_appid)) == ESP_OK) continue;
        if (parse_line(line, "TTS_TOKEN", cfg->tts_token, sizeof(cfg->tts_token)) == ESP_OK) continue;
        if (parse_line(line, "TTS_ACCESS_TOKEN", cfg->tts_token, sizeof(cfg->tts_token)) == ESP_OK) continue;
        if (parse_line(line, "TTS_VOICE", cfg->tts_voice, sizeof(cfg->tts_voice)) == ESP_OK) continue;
        if (parse_line(line, "TTS_CLUSTER", cfg->tts_cluster, sizeof(cfg->tts_cluster)) == ESP_OK) continue;
        if (parse_line(line, "TTS_RESOURCE_ID", cfg->tts_resource_id, sizeof(cfg->tts_resource_id)) == ESP_OK) continue;
        if (parse_line(line, "TTS_API_KEY", cfg->tts_api_key, sizeof(cfg->tts_api_key)) == ESP_OK) continue;
        if (parse_line(line, "TTS_APP_KEY", cfg->tts_api_key, sizeof(cfg->tts_api_key)) == ESP_OK) continue;
        if (parse_line(line, "TTS_SECRET_ID", cfg->tts_api_key, sizeof(cfg->tts_api_key)) == ESP_OK) continue;
        if (parse_line(line, "TTS_SECRET_KEY", cfg->tts_api_key, sizeof(cfg->tts_api_key)) == ESP_OK) continue;
    }
    fclose(f);
    sd_card_unlock();

    ESP_LOGI(TAG, "Config loaded: SSID=%s, TTS_VOICE=%s",
             cfg->wifi_ssid, cfg->tts_voice);
    return ESP_OK;
}
