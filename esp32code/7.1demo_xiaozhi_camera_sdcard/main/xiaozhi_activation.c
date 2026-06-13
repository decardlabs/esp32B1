#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "xiaozhi_activation.h"
#include "xiaozhi_ui_status.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define XIAOZHI_LANG_CODE                  "zh-CN"
#define XIAOZHI_WS_PROTOCOL_DEFAULT        1
#define XIAOZHI_HTTP_TIMEOUT_MS            8000
#define XIAOZHI_HTTP_RETRY_MS              1000
#define XIAOZHI_ACTIVATE_ERROR_DELAY_MS    2000
#define XIAOZHI_ACTIVATE_POLL_MAX          10
#define XIAOZHI_ACTIVATE_POLL_MAX_DELAY_MS 3000

typedef struct {
    char *data;
    size_t len;
} xiaozhi_http_buffer_t;

typedef struct {
    bool has_activation;
    bool has_websocket;
    char activation_message[128];
    char activation_code[64];
    char activation_challenge[128];
    int activation_timeout_ms;
    char ws_url[XIAOZHI_WS_URL_LEN];
    char ws_token[XIAOZHI_WS_TOKEN_LEN];
    int ws_version;
} xiaozhi_activation_response_t;

static const char *TAG = "XIAOZHI_ACT";

static void xiaozhi_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static esp_err_t xiaozhi_http_event_handler(esp_http_client_event_t *evt)
{
    xiaozhi_http_buffer_t *buffer = evt->user_data;

    if ((evt == NULL) || (buffer == NULL)) {
        return ESP_OK;
    }

    if ((evt->event_id == HTTP_EVENT_ON_DATA) && (evt->data_len > 0) && (evt->data != NULL)) {
        char *new_data = realloc(buffer->data, buffer->len + evt->data_len + 1);
        if (new_data == NULL) {
            ESP_LOGE(TAG, "http response realloc failed");
            return ESP_ERR_NO_MEM;
        }

        buffer->data = new_data;
        memcpy(buffer->data + buffer->len, evt->data, evt->data_len);
        buffer->len += evt->data_len;
        buffer->data[buffer->len] = '\0';
    }

    return ESP_OK;
}

static int xiaozhi_normalize_ws_version(int version)
{
    if ((version < 1) || (version > 3)) {
        return XIAOZHI_WS_PROTOCOL_DEFAULT;
    }

    return version;
}

static esp_err_t xiaozhi_get_device_id(char *device_id, size_t len)
{
    uint8_t mac[6] = {0};
    esp_err_t ret;

    if ((device_id == NULL) || (len < 18)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(device_id, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

static void xiaozhi_generate_uuid(char *uuid, size_t len)
{
    uint8_t uuid_raw[16];

    esp_fill_random(uuid_raw, sizeof(uuid_raw));
    uuid_raw[6] = (uuid_raw[6] & 0x0F) | 0x40;
    uuid_raw[8] = (uuid_raw[8] & 0x3F) | 0x80;

    snprintf(uuid, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid_raw[0], uuid_raw[1], uuid_raw[2], uuid_raw[3],
             uuid_raw[4], uuid_raw[5], uuid_raw[6], uuid_raw[7],
             uuid_raw[8], uuid_raw[9], uuid_raw[10], uuid_raw[11],
             uuid_raw[12], uuid_raw[13], uuid_raw[14], uuid_raw[15]);
}

static esp_err_t xiaozhi_get_or_create_uuid(char *uuid, size_t len)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle;
    size_t required_size = len;

    if ((uuid == NULL) || (len < 37)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open("board", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, "uuid", uuid, &required_size);
    if ((ret == ESP_OK) && (uuid[0] != '\0')) {
        nvs_close(nvs_handle);
        return ESP_OK;
    }

    xiaozhi_generate_uuid(uuid, len);
    ESP_LOGI(TAG, "generated client id: %s", uuid);

    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "uuid", uuid), exit, TAG, "save uuid failed");
    ESP_GOTO_ON_ERROR(nvs_commit(nvs_handle), exit, TAG, "commit uuid failed");

    ret = ESP_OK;

exit:
    nvs_close(nvs_handle);
    return ret;
}

static void xiaozhi_get_user_agent(char *user_agent, size_t len)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if ((user_agent == NULL) || (len == 0)) {
        return;
    }

    snprintf(user_agent, len, "%s/%s", app_desc->project_name, app_desc->version);
}

static char *xiaozhi_build_system_info_json(const xiaozhi_server_config_t *config)
{
    cJSON *root = NULL;
    cJSON *chip_info_obj = NULL;
    cJSON *app_obj = NULL;
    cJSON *ota_obj = NULL;
    cJSON *board_obj = NULL;
    char *json_str = NULL;
    char sha256_str[65] = {0};
    char compile_time[40];
    char min_heap_str[16];
    uint32_t flash_size = 0;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_chip_info_t chip_info;
    int i;

    if (config == NULL) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    snprintf(compile_time, sizeof(compile_time), "%sT%sZ", app_desc->date, app_desc->time);
    snprintf(min_heap_str, sizeof(min_heap_str), "%u", (unsigned int)esp_get_minimum_free_heap_size());

    for (i = 0; i < 32; i++) {
        snprintf(sha256_str + (i * 2), sizeof(sha256_str) - (i * 2), "%02x", app_desc->app_elf_sha256[i]);
    }

    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", XIAOZHI_LANG_CODE);
    cJSON_AddNumberToObject(root, "flash_size", flash_size);
    cJSON_AddStringToObject(root, "minimum_free_heap_size", min_heap_str);
    cJSON_AddStringToObject(root, "mac_address", config->device_id);
    cJSON_AddStringToObject(root, "uuid", config->client_id);
    cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);

    chip_info_obj = cJSON_AddObjectToObject(root, "chip_info");
    if (chip_info_obj != NULL) {
        cJSON_AddNumberToObject(chip_info_obj, "model", chip_info.model);
        cJSON_AddNumberToObject(chip_info_obj, "cores", chip_info.cores);
        cJSON_AddNumberToObject(chip_info_obj, "revision", chip_info.revision);
        cJSON_AddNumberToObject(chip_info_obj, "features", chip_info.features);
    }

    app_obj = cJSON_AddObjectToObject(root, "application");
    if (app_obj != NULL) {
        cJSON_AddStringToObject(app_obj, "name", app_desc->project_name);
        cJSON_AddStringToObject(app_obj, "version", app_desc->version);
        cJSON_AddStringToObject(app_obj, "compile_time", compile_time);
        cJSON_AddStringToObject(app_obj, "idf_version", app_desc->idf_ver);
        cJSON_AddStringToObject(app_obj, "elf_sha256", sha256_str);
    }

    ota_obj = cJSON_AddObjectToObject(root, "ota");
    if ((ota_obj != NULL) && (running != NULL)) {
        cJSON_AddStringToObject(ota_obj, "label", running->label);
    }

    board_obj = cJSON_AddObjectToObject(root, "board");
    if (board_obj != NULL) {
        cJSON_AddStringToObject(board_obj, "name", "fengge-esp32s3");
        cJSON_AddStringToObject(board_obj, "target", CONFIG_IDF_TARGET);
        cJSON_AddStringToObject(board_obj, "audio_codec", "es8388");
        cJSON_AddStringToObject(board_obj, "display", "st7796");
    }

    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static esp_err_t xiaozhi_http_post(const char *url, const char *device_id, const char *client_id,
                                   const char *payload, int *status_code, char **response_body)
{
    esp_err_t ret;
    esp_http_client_handle_t client;
    esp_http_client_config_t config = {0};
    xiaozhi_http_buffer_t buffer = {0};
    char user_agent[96];

    if ((url == NULL) || (device_id == NULL) || (client_id == NULL) || (payload == NULL) ||
        (status_code == NULL) || (response_body == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_get_user_agent(user_agent, sizeof(user_agent));

    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = XIAOZHI_HTTP_TIMEOUT_MS;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.user_data = &buffer;
    config.event_handler = xiaozhi_http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id", device_id);
    esp_http_client_set_header(client, "Client-Id", client_id);
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_http_client_set_header(client, "Accept-Language", XIAOZHI_LANG_CODE);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "http post failed: %s, url=%s", esp_err_to_name(ret), url);
        esp_http_client_cleanup(client);
        free(buffer.data);
        return ret;
    }

    *status_code = esp_http_client_get_status_code(client);
    if (buffer.data == NULL) {
        buffer.data = calloc(1, 1);
        if (buffer.data == NULL) {
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
    }

    *response_body = buffer.data;
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static void xiaozhi_parse_ota_response(const char *body, xiaozhi_activation_response_t *response)
{
    cJSON *root;
    cJSON *activation;
    cJSON *websocket;
    cJSON *item;

    if ((body == NULL) || (response == NULL)) {
        return;
    }

    memset(response, 0, sizeof(*response));
    response->ws_version = XIAOZHI_WS_PROTOCOL_DEFAULT;
    response->activation_timeout_ms = XIAOZHI_HTTP_RETRY_MS;

    root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "ota response parse failed");
        return;
    }

    activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (cJSON_IsObject(activation)) {
        item = cJSON_GetObjectItemCaseSensitive(activation, "message");
        if (cJSON_IsString(item) && (item->valuestring != NULL)) {
            xiaozhi_safe_copy(response->activation_message, sizeof(response->activation_message), item->valuestring);
            response->has_activation = true;
        }

        item = cJSON_GetObjectItemCaseSensitive(activation, "code");
        if (cJSON_IsString(item) && (item->valuestring != NULL)) {
            xiaozhi_safe_copy(response->activation_code, sizeof(response->activation_code), item->valuestring);
            response->has_activation = true;
        }

        item = cJSON_GetObjectItemCaseSensitive(activation, "challenge");
        if (cJSON_IsString(item) && (item->valuestring != NULL)) {
            xiaozhi_safe_copy(response->activation_challenge, sizeof(response->activation_challenge), item->valuestring);
            response->has_activation = true;
        }

        item = cJSON_GetObjectItemCaseSensitive(activation, "timeout_ms");
        if (cJSON_IsNumber(item)) {
            response->activation_timeout_ms = item->valueint;
            response->has_activation = true;
        }
    }

    websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        cJSON *url = cJSON_GetObjectItemCaseSensitive(websocket, "url");
        cJSON *token = cJSON_GetObjectItemCaseSensitive(websocket, "token");
        cJSON *version = cJSON_GetObjectItemCaseSensitive(websocket, "version");

        if (cJSON_IsString(url) && (url->valuestring != NULL)) {
            xiaozhi_safe_copy(response->ws_url, sizeof(response->ws_url), url->valuestring);
        }
        if (cJSON_IsString(token) && (token->valuestring != NULL)) {
            xiaozhi_safe_copy(response->ws_token, sizeof(response->ws_token), token->valuestring);
        }
        if (cJSON_IsNumber(version)) {
            response->ws_version = xiaozhi_normalize_ws_version(version->valueint);
        } else if (cJSON_IsString(version) && (version->valuestring != NULL)) {
            response->ws_version = xiaozhi_normalize_ws_version(atoi(version->valuestring));
        }

        if ((response->ws_url[0] != '\0') && (response->ws_token[0] != '\0')) {
            response->has_websocket = true;
        }
    }

    cJSON_Delete(root);
}

static esp_err_t xiaozhi_save_websocket_config(const xiaozhi_server_config_t *config)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle;

    if ((config == NULL) || (config->ws_url[0] == '\0') || (config->ws_token[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open("websocket", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "url", config->ws_url), exit, TAG, "save ws url failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "token", config->ws_token), exit, TAG, "save ws token failed");
    ESP_GOTO_ON_ERROR(nvs_set_i32(nvs_handle, "version", config->ws_version), exit, TAG, "save ws version failed");
    ESP_GOTO_ON_ERROR(nvs_commit(nvs_handle), exit, TAG, "commit ws config failed");

    ret = ESP_OK;

exit:
    nvs_close(nvs_handle);
    return ret;
}

static esp_err_t xiaozhi_load_websocket_config(xiaozhi_server_config_t *config)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle;
    size_t url_size;
    size_t token_size;
    int32_t version = XIAOZHI_WS_PROTOCOL_DEFAULT;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open("websocket", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    url_size = sizeof(config->ws_url);
    token_size = sizeof(config->ws_token);

    ret = nvs_get_str(nvs_handle, "url", config->ws_url, &url_size);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_str(nvs_handle, "token", config->ws_token, &token_size);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_i32(nvs_handle, "version", &version);
    if (ret == ESP_OK) {
        config->ws_version = xiaozhi_normalize_ws_version(version);
    } else {
        config->ws_version = XIAOZHI_WS_PROTOCOL_DEFAULT;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

static void xiaozhi_log_activation_info(const xiaozhi_activation_response_t *response)
{
    if (response == NULL) {
        return;
    }

    ESP_LOGW(TAG, "device is waiting for XiaoZhi activation");
    if (response->activation_message[0] != '\0') {
        ESP_LOGW(TAG, "activation message: %s", response->activation_message);
        xiaozhi_ui_status_set_note(response->activation_message);
    } else {
        xiaozhi_ui_status_set_note("请打开 xiaozhi.me 输入绑定码");
    }
    if (response->activation_code[0] != '\0') {
        ESP_LOGW(TAG, "activation code: %s", response->activation_code);
        ESP_LOGW(TAG, "please open https://xiaozhi.me and add this code");
        xiaozhi_ui_status_set_binding_text(response->activation_code);
    } else {
        xiaozhi_ui_status_set_binding_text("待绑定");
    }
}

static void xiaozhi_fill_server_config(xiaozhi_server_config_t *config, const xiaozhi_activation_response_t *response)
{
    if ((config == NULL) || (response == NULL)) {
        return;
    }

    xiaozhi_safe_copy(config->ws_url, sizeof(config->ws_url), response->ws_url);
    xiaozhi_safe_copy(config->ws_token, sizeof(config->ws_token), response->ws_token);
    config->ws_version = xiaozhi_normalize_ws_version(response->ws_version);
}

static void xiaozhi_build_activate_url(char *url, size_t len)
{
    const char *base_url = CONFIG_XIAOZHI_OTA_URL;

    if ((url == NULL) || (len == 0)) {
        return;
    }

    if ((base_url != NULL) && (base_url[0] != '\0') && (base_url[strlen(base_url) - 1] == '/')) {
        snprintf(url, len, "%sactivate", base_url);
    } else {
        snprintf(url, len, "%s/activate", base_url);
    }
}

static esp_err_t xiaozhi_poll_activation(const xiaozhi_server_config_t *config,
                                         const xiaozhi_activation_response_t *response)
{
    char activate_url[320];
    int delay_ms;
    int i;

    if ((config == NULL) || (response == NULL) || (response->activation_challenge[0] == '\0')) {
        return ESP_FAIL;
    }

    xiaozhi_build_activate_url(activate_url, sizeof(activate_url));
    delay_ms = (response->activation_timeout_ms > 0) ? response->activation_timeout_ms : XIAOZHI_HTTP_RETRY_MS;
    if (delay_ms > XIAOZHI_ACTIVATE_POLL_MAX_DELAY_MS) {
        delay_ms = XIAOZHI_ACTIVATE_POLL_MAX_DELAY_MS;
    }

    for (i = 0; i < XIAOZHI_ACTIVATE_POLL_MAX; i++) {
        char *body = NULL;
        int status_code = 0;
        esp_err_t ret;

        ESP_LOGI(TAG, "activation poll %d/%d", i + 1, XIAOZHI_ACTIVATE_POLL_MAX);
        ret = xiaozhi_http_post(activate_url, config->device_id, config->client_id, "{}", &status_code, &body);
        free(body);

        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_ACTIVATE_ERROR_DELAY_MS));
            continue;
        }

        if (status_code == 200) {
            ESP_LOGI(TAG, "activation successful");
            return ESP_OK;
        }

        if (status_code == 202) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        ESP_LOGW(TAG, "activation pending failed, status=%d", status_code);
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_ACTIVATE_ERROR_DELAY_MS));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t xiaozhi_activation_prepare(xiaozhi_server_config_t *config)
{
    esp_err_t ret;
    xiaozhi_server_config_t saved_config = {0};
    bool has_saved_config = false;
    int ota_fail_count = 0;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->ws_version = XIAOZHI_WS_PROTOCOL_DEFAULT;

    ESP_RETURN_ON_ERROR(xiaozhi_get_device_id(config->device_id, sizeof(config->device_id)), TAG, "get device id failed");
    ESP_RETURN_ON_ERROR(xiaozhi_get_or_create_uuid(config->client_id, sizeof(config->client_id)), TAG, "get client id failed");

    if (strlen(CONFIG_XIAOZHI_ACCESS_TOKEN) > 0) {
        xiaozhi_safe_copy(config->ws_url, sizeof(config->ws_url), CONFIG_XIAOZHI_WS_URL);
        xiaozhi_safe_copy(config->ws_token, sizeof(config->ws_token), CONFIG_XIAOZHI_ACCESS_TOKEN);
        config->ws_version = XIAOZHI_WS_PROTOCOL_DEFAULT;
        ESP_LOGI(TAG, "use XiaoZhi token from menuconfig");
        return ESP_OK;
    }

    xiaozhi_safe_copy(saved_config.device_id, sizeof(saved_config.device_id), config->device_id);
    xiaozhi_safe_copy(saved_config.client_id, sizeof(saved_config.client_id), config->client_id);
    ret = xiaozhi_load_websocket_config(&saved_config);
    if (ret == ESP_OK) {
        has_saved_config = true;
        ESP_LOGI(TAG, "found saved websocket config, version=%d", saved_config.ws_version);
    }

    while (1) {
        char *payload = xiaozhi_build_system_info_json(config);
        char *response_body = NULL;
        int status_code = 0;
        xiaozhi_activation_response_t response = {0};

        if (payload == NULL) {
            return ESP_ERR_NO_MEM;
        }

        ret = xiaozhi_http_post(CONFIG_XIAOZHI_OTA_URL, config->device_id, config->client_id,
                                payload, &status_code, &response_body);
        cJSON_free(payload);
        if (ret != ESP_OK) {
            ota_fail_count++;
            if (has_saved_config && (ota_fail_count >= 3)) {
                ESP_LOGW(TAG, "ota unavailable, use saved websocket config");
                *config = saved_config;
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_HTTP_RETRY_MS));
            continue;
        }

        ota_fail_count = 0;
        xiaozhi_parse_ota_response(response_body, &response);
        free(response_body);

        if (status_code != 200) {
            ESP_LOGW(TAG, "ota response status=%d", status_code);
            if (has_saved_config && (status_code >= 500)) {
                ESP_LOGW(TAG, "server busy, use saved websocket config");
                *config = saved_config;
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_HTTP_RETRY_MS));
            continue;
        }

        if (response.has_activation) {
            xiaozhi_log_activation_info(&response);
            ret = xiaozhi_poll_activation(config, &response);
            if (ret == ESP_OK) {
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_HTTP_RETRY_MS));
            continue;
        }

        if (response.has_websocket) {
            xiaozhi_fill_server_config(config, &response);
            xiaozhi_save_websocket_config(config);
            ESP_LOGI(TAG, "websocket config ready, version=%d", config->ws_version);
            return ESP_OK;
        }

        if (has_saved_config) {
            ESP_LOGW(TAG, "ota returned no websocket info, use saved config");
            *config = saved_config;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "ota returned no websocket config, retrying");
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_HTTP_RETRY_MS));
    }
}
