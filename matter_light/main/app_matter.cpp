#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <app_driver.h>
#include <app_matter.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <esp_partition.h>

/* LVGL display functions (C-linkage for C++ callers) */
extern "C" {
void lcd_lvgl_show_pairing_info(const char *qr_code_str, const char *manual_code);
void lcd_lvgl_show_operational(void);
void lcd_lvgl_update_status(const char *status);
void lcd_lvgl_update_light_state(bool on);
}

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "APP_MATTER";

uint16_t light_endpoint_id = 0;
static char s_manual_code[128] = {0};
static char s_qr_code[512] = {0};

/* Device event callback */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        lcd_lvgl_show_operational();
        lcd_lvgl_update_status("Ready");
        break;

    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP address changed");
        lcd_lvgl_update_status("Ready");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Fail-safe timer expired");
        lcd_lvgl_update_status("Commissioning Failed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    default:
        break;
    }
}

/* Identification callback */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification: type=%u effect=%u", type, effect_id);
    app_driver_identify();
    return ESP_OK;
}

/* Attribute update callback */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        app_driver_set_power(val->val.b);
        lcd_lvgl_update_light_state(val->val.b);
    }

    return ESP_OK;
}

static void app_matter_get_setup_payloads(void)
{
    uint16_t discriminator_val = 0;
    uint32_t passcode = 0;

    /* Get commissioning data from the platform provider */
    chip::DeviceLayer::CommissionableDataProvider *provider = chip::DeviceLayer::GetCommissionableDataProvider();
    if (!provider) {
        ESP_LOGE(TAG, "No CommissionableDataProvider available");
        snprintf(s_manual_code, sizeof(s_manual_code), "N/A");
        return;
    }

    if (provider->GetSetupDiscriminator(discriminator_val) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Using default discriminator");
        discriminator_val = 3840; /* 0xF00, standard test value */
    }
    if (provider->GetSetupPasscode(passcode) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Using default passcode");
        passcode = 20202021; /* Standard test passcode */
    }

    ESP_LOGI(TAG, "Discriminator=%u, Passcode=%u", discriminator_val, passcode);

    /* Build SetupPayload (inherits PayloadContents) */
    chip::SetupPayload payload;
    payload.version = 0;
    payload.vendorID = 0xFFF1;
    payload.productID = 0x8000;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.rendezvousInformation.SetValue(chip::RendezvousInformationFlags(
        chip::to_underlying(chip::RendezvousInformationFlag::kBLE) |
        chip::to_underlying(chip::RendezvousInformationFlag::kSoftAP)));
    payload.discriminator.SetLongValue(discriminator_val);
    payload.setUpPINCode = passcode;

    /* Generate manual pairing code (uses PayloadContents base) */
    {
        chip::ManualSetupPayloadGenerator generator(payload);
        std::string manual_code;
        if (generator.payloadDecimalStringRepresentation(manual_code) == CHIP_NO_ERROR) {
            strncpy(s_manual_code, manual_code.c_str(), sizeof(s_manual_code) - 1);
            ESP_LOGI(TAG, "Manual code: %s", s_manual_code);
        }
    }

    /* Generate QR code payload string (uses SetupPayload) */
    {
        chip::QRCodeSetupPayloadGenerator generator(payload);
        std::string qr_string;
        if (generator.payloadBase38Representation(qr_string) == CHIP_NO_ERROR) {
            strncpy(s_qr_code, qr_string.c_str(), sizeof(s_qr_code) - 1);
            ESP_LOGI(TAG, "QR: %s", s_qr_code);
        }
    }
}

esp_err_t app_matter_start(void)
{
    /* Create Matter node */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    /* Configure On/Off Light endpoint */
    on_off_light::config_t light_config;
    light_config.on_off.on_off = false;
    light_config.on_off_lighting.start_up_on_off = nullptr;

    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "Failed to create on/off light endpoint");
        return ESP_FAIL;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "On/Off Light endpoint_id=%d", light_endpoint_id);

    /* Start Matter stack */
    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed err=%d", err);
        return err;
    }

    /* Wait a moment for commissioning window to open, then get payloads */
    vTaskDelay(pdMS_TO_TICKS(1000));
    app_matter_get_setup_payloads();

    /* Check if already commissioned (has fabric entries) */
    uint8_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    if (fabric_count > 0) {
        ESP_LOGI(TAG, "Already commissioned (%u fabrics), showing operational screen", fabric_count);
        lcd_lvgl_show_operational();
        lcd_lvgl_update_status("Ready");
    } else if (strlen(s_qr_code) > 0 || strlen(s_manual_code) > 0) {
        lcd_lvgl_show_pairing_info(s_qr_code, s_manual_code);
        lcd_lvgl_update_status("Ready for pairing");
    }

    ESP_LOGI(TAG, "Matter stack started");
    return ESP_OK;
}

void app_matter_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing Matter fabric data");
    nvs_flash_deinit();

    /* Directly erase the NVS partition (contains Matter fabric data) */
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (nvs) {
        esp_partition_erase_range(nvs, 0, nvs->size);
        ESP_LOGW(TAG, "NVS partition erased");
    }

    /* Also erase NVS keys partition if encrypted */
    const esp_partition_t *nvs_keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    if (nvs_keys) {
        esp_partition_erase_range(nvs_keys, 0, nvs_keys->size);
    }

    esp_restart();
}

const char *app_matter_get_manual_code(void)
{
    return s_manual_code;
}

const char *app_matter_get_qr_code(void)
{
    return s_qr_code;
}
