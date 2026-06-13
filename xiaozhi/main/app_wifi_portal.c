#include "app_wifi_portal.h"
#include <stdlib.h>
#include <string.h>
#include "app_wifi_store.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_WIFI_PORTAL_AP_CHANNEL       1
#define APP_WIFI_PORTAL_MAX_STA_CONN     4
#define APP_WIFI_PORTAL_HTTP_STACK       8192
#define APP_WIFI_PORTAL_BODY_MAX_LEN     256
#define APP_WIFI_PORTAL_AP_IP            "192.168.4.1"

static const char *TAG = "APP_WIFI_PORTAL";

static httpd_handle_t s_wifi_portal_server = NULL;
static esp_netif_t *s_wifi_portal_netif_ap = NULL;
static bool s_wifi_portal_running = false;
static bool s_wifi_portal_restart_pending = false;
static char s_wifi_portal_ap_ssid[33] = {0};

static void app_wifi_portal_fill_sta_cfg(wifi_config_t *sta_cfg, const char *ssid, const char *password)
{
    size_t ssid_len = 0;
    size_t password_len = 0;

    if (sta_cfg == NULL) {
        return;
    }

    memset(sta_cfg, 0, sizeof(*sta_cfg));
    if (ssid != NULL) {
        ssid_len = strnlen(ssid, sizeof(sta_cfg->sta.ssid));
        memcpy(sta_cfg->sta.ssid, ssid, ssid_len);
    }
    if (password != NULL) {
        password_len = strnlen(password, sizeof(sta_cfg->sta.password));
        memcpy(sta_cfg->sta.password, password, password_len);
    }

    sta_cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg->sta.pmf_cfg.capable = true;
    sta_cfg->sta.pmf_cfg.required = false;
}

static void app_wifi_portal_persist_sta_credentials(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg;
    esp_err_t ret;

    app_wifi_portal_fill_sta_cfg(&sta_cfg, ssid, password);
    ret = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set wifi storage to flash failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "persist sta credentials failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "persisted sta credentials for next boot: ssid=%s", (ssid != NULL) ? ssid : "");
}

static const char *APP_WIFI_PORTAL_INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Xiaozhi Wi-Fi Setup</title>"
"<style>body{font-family:Arial,sans-serif;background:#f4f7fb;color:#1d2a3a;"
"margin:0;padding:20px;}main{max-width:520px;margin:0 auto;background:#fff;"
"border-radius:16px;padding:20px;box-shadow:0 12px 40px rgba(0,0,0,.08);}h1{"
"margin-top:0;font-size:28px;}button,input,select{width:100%;box-sizing:border-box;"
"padding:12px 14px;margin-top:12px;border-radius:10px;border:1px solid #c8d3e1;"
"font-size:16px;}button{background:#1463ff;color:#fff;border:none;font-weight:700;}"
"button.secondary{background:#eff4ff;color:#234;border:1px solid #b9cbff;}"
"#status{margin-top:16px;padding:12px;border-radius:10px;background:#eef5ff;"
"white-space:pre-wrap;}small{color:#5d6b7a;display:block;margin-top:8px;}"
"</style></head><body><main><h1>Xiaozhi Wi-Fi Setup</h1>"
"<p>Connect this device to your home Wi-Fi, then it will reboot automatically.</p>"
"<small id='portalInfo'>Loading portal info...</small>"
"<button class='secondary' onclick='scanWifi()'>Scan Wi-Fi</button>"
"<select id='ssidList'><option value=''>Select Wi-Fi</option></select>"
"<input id='ssidInput' placeholder='Or type Wi-Fi SSID manually'>"
"<input id='passwordInput' type='password' placeholder='Wi-Fi password'>"
"<button onclick='saveWifi()'>Save and Reboot</button>"
"<div id='status'>Ready.</div></main><script>"
"async function loadStatus(){const r=await fetch('/api/status');const data=await r.json();"
"document.getElementById('portalInfo').textContent='AP: '+data.ap_ssid+'  URL: http://'+data.ap_ip;"
"+(data.has_saved?'  (saved Wi-Fi exists)':'');}"
"async function scanWifi(){const s=document.getElementById('status');"
"s.textContent='Scanning nearby Wi-Fi...';const r=await fetch('/api/scan');"
"const data=await r.json();const sel=document.getElementById('ssidList');sel.innerHTML='';"
"const first=document.createElement('option');first.value='';first.textContent='Select Wi-Fi';sel.appendChild(first);"
"(data.items||[]).forEach(item=>{const o=document.createElement('option');o.value=item.ssid;"
"o.textContent=item.ssid+'  RSSI '+item.rssi;sel.appendChild(o);});"
"s.textContent='Scan done. Found '+((data.items||[]).length)+' network(s).';}"
"async function saveWifi(){const sel=document.getElementById('ssidList').value;"
"const manual=document.getElementById('ssidInput').value.trim();"
"const ssid=manual||sel;const password=document.getElementById('passwordInput').value;"
"const s=document.getElementById('status');if(!ssid){s.textContent='Please choose or enter an SSID.';return;}"
"s.textContent='Saving Wi-Fi settings...';const r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:ssid,password:password})});const data=await r.json();"
"s.textContent=data.message||'Done.';}"
"loadStatus();scanWifi();</script></body></html>";

static void app_wifi_portal_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t app_wifi_portal_send_json(httpd_req_t *req, cJSON *root)
{
    char *json_text = NULL;
    esp_err_t ret = ESP_OK;

    if ((req == NULL) || (root == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    json_text = cJSON_PrintUnformatted(root);
    if (json_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, json_text);
    cJSON_free(json_text);
    return ret;
}

static esp_err_t app_wifi_portal_read_body(httpd_req_t *req, char *body, size_t body_size)
{
    int received = 0;
    int ret = 0;

    if ((req == NULL) || (body == NULL) || (body_size == 0) || (req->content_len <= 0) ||
        (req->content_len >= (int)body_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (received < req->content_len) {
        ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    body[received] = '\0';
    return ESP_OK;
}

static bool app_wifi_portal_ssid_seen(const wifi_ap_record_t *records, uint16_t count, uint16_t index)
{
    uint16_t i = 0;

    for (i = 0; i < index; i++) {
        if (strcmp((const char *)records[i].ssid, (const char *)records[index].ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t app_wifi_portal_handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, APP_WIFI_PORTAL_INDEX_HTML);
}

static esp_err_t app_wifi_portal_handle_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(root, "ap_ssid", s_wifi_portal_ap_ssid);
    cJSON_AddStringToObject(root, "ap_ip", APP_WIFI_PORTAL_AP_IP);
    cJSON_AddBoolToObject(root, "has_saved", app_wifi_store_has_saved_credentials());
    if (app_wifi_portal_send_json(req, root) != ESP_OK) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t app_wifi_portal_handle_scan(httpd_req_t *req)
{
    esp_err_t ret;
    uint16_t ap_count = 0;
    wifi_ap_record_t *records = NULL;
    cJSON *root = NULL;
    cJSON *items = NULL;
    uint16_t i = 0;

    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan start failed: %s", esp_err_to_name(ret));
        return httpd_resp_send_500(req);
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    records = calloc(ap_count, sizeof(wifi_ap_record_t));
    root = cJSON_CreateObject();
    items = (root != NULL) ? cJSON_AddArrayToObject(root, "items") : NULL;
    if (((ap_count > 0) && (records == NULL)) || (root == NULL) || (items == NULL)) {
        free(records);
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    if (ap_count > 0) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, records));
    }

    for (i = 0; i < ap_count; i++) {
        cJSON *item = NULL;

        if ((records[i].ssid[0] == '\0') || app_wifi_portal_ssid_seen(records, ap_count, i)) {
            continue;
        }

        item = cJSON_CreateObject();
        if (item == NULL) {
            free(records);
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }

        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddItemToArray(items, item);
    }

    free(records);
    if (app_wifi_portal_send_json(req, root) != ESP_OK) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t app_wifi_portal_handle_save(httpd_req_t *req)
{
    char body[APP_WIFI_PORTAL_BODY_MAX_LEN];
    esp_err_t ret;
    cJSON *root = NULL;
    cJSON *ssid = NULL;
    cJSON *password = NULL;
    cJSON *reply = NULL;

    ret = app_wifi_portal_read_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"message\":\"invalid json\"}");
    }

    ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || (ssid->valuestring == NULL) ||
        (ssid->valuestring[0] == '\0') ||
        (strlen(ssid->valuestring) > APP_WIFI_SSID_MAX_LEN) ||
        (!cJSON_IsString(password) && !cJSON_IsNull(password))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"message\":\"invalid ssid or password\"}");
    }

    ret = app_wifi_store_save(ssid->valuestring,
                              (cJSON_IsString(password) && (password->valuestring != NULL)) ? password->valuestring : "");
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save wifi credentials failed: %s", esp_err_to_name(ret));
        return httpd_resp_send_500(req);
    }

    app_wifi_portal_persist_sta_credentials(ssid->valuestring,
                                            (cJSON_IsString(password) && (password->valuestring != NULL)) ?
                                            password->valuestring :
                                            "");

    reply = cJSON_CreateObject();
    if (reply == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(reply, "message", "Saved. Device will reboot in 1 second.");
    if (app_wifi_portal_send_json(req, reply) != ESP_OK) {
        cJSON_Delete(reply);
        return ESP_FAIL;
    }
    cJSON_Delete(reply);

    if (!s_wifi_portal_restart_pending) {
        s_wifi_portal_restart_pending = true;
        xTaskCreate(app_wifi_portal_restart_task, "wifi_portal_restart", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t app_wifi_portal_stack_init(void)
{
    esp_err_t ret;

    ret = esp_netif_init();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }

    return ESP_OK;
}

static void app_wifi_portal_build_ap_ssid(void)
{
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_wifi_portal_ap_ssid, sizeof(s_wifi_portal_ap_ssid),
             "XIAOZHI_SETUP_%02X%02X", mac[4], mac[5]);
}

static esp_err_t app_wifi_portal_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = app_wifi_portal_handle_index,
        .user_ctx = NULL,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = app_wifi_portal_handle_status,
        .user_ctx = NULL,
    };
    httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = app_wifi_portal_handle_scan,
        .user_ctx = NULL,
    };
    httpd_uri_t save_uri = {
        .uri = "/api/save",
        .method = HTTP_POST,
        .handler = app_wifi_portal_handle_save,
        .user_ctx = NULL,
    };

    config.stack_size = APP_WIFI_PORTAL_HTTP_STACK;
    config.max_uri_handlers = 8;
    ESP_RETURN_ON_ERROR(httpd_start(&s_wifi_portal_server, &config), TAG, "start http server failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_portal_server, &index_uri), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_portal_server, &status_uri), TAG, "register /api/status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_portal_server, &scan_uri), TAG, "register /api/scan failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_portal_server, &save_uri), TAG, "register /api/save failed");
    return ESP_OK;
}

esp_err_t app_wifi_portal_start(void)
{
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_cfg = {0};

    if (s_wifi_portal_running) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(app_wifi_portal_stack_init(), TAG, "wifi stack init failed");
    app_wifi_portal_build_ap_ssid();

    s_wifi_portal_netif_ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_wifi_portal_netif_ap != NULL, ESP_FAIL, TAG, "create default wifi ap failed");

    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set wifi mode failed");

    ap_cfg.ap.ssid_len = strlen(s_wifi_portal_ap_ssid);
    memcpy(ap_cfg.ap.ssid, s_wifi_portal_ap_ssid, ap_cfg.ap.ssid_len);
    ap_cfg.ap.channel = APP_WIFI_PORTAL_AP_CHANNEL;
    ap_cfg.ap.max_connection = APP_WIFI_PORTAL_MAX_STA_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set wifi ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(app_wifi_portal_http_start(), TAG, "http portal start failed");

    s_wifi_portal_running = true;
    ESP_LOGW(TAG, "wifi portal started");
    ESP_LOGW(TAG, "1. connect AP: %s", s_wifi_portal_ap_ssid);
    ESP_LOGW(TAG, "2. open URL: http://%s", APP_WIFI_PORTAL_AP_IP);
    return ESP_OK;
}

bool app_wifi_portal_is_running(void)
{
    return s_wifi_portal_running;
}

const char *app_wifi_portal_get_ap_ssid(void)
{
    return s_wifi_portal_ap_ssid;
}

const char *app_wifi_portal_get_ap_ip(void)
{
    return APP_WIFI_PORTAL_AP_IP;
}
