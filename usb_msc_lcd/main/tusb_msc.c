#include "tusb_msc.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "class/msc/msc_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "USB_MSC";

static sdmmc_card_t *s_card = NULL;

/* ── USB PHY ─────────────────────────────────────────────────────────── */

static esp_err_t usb_phy_init(void)
{
    usb_phy_handle_t phy_hdl;
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .otg_io_conf = NULL, /* Use default internal GPIOs (DP=20, DM=19) */
        .ext_io_conf = NULL,
    };
    return usb_new_phy(&phy_cfg, &phy_hdl);
}

/* ── TinyUSB task ────────────────────────────────────────────────────── */

static void usb_device_task(void *arg)
{
    while (1) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ── TinyUSB MSC callbacks (override weak stubs in TinyUSB) ──────────── */

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1; /* Single logical unit: the TF card */
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "ESP32-S3", 8);
    memcpy(product_id,  "USB Mass Storag", 16);
    memcpy(product_rev, "1.0", 3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return (s_card != NULL);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    /* csd.capacity is the number of 512-byte blocks for SDHC/SDXC */
    *block_count = s_card->csd.capacity;
    *block_size  = 512;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    esp_err_t ret = sdmmc_read_sectors(s_card, buffer, lba, num_sectors);
    if (ret == ESP_OK) {
        return (int32_t)bufsize;
    }
    ESP_LOGE(TAG, "read10 lba=%lu sectors=%lu failed", (unsigned long)lba, (unsigned long)num_sectors);
    return -1; /* STALL */
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)offset;

    uint32_t num_sectors = bufsize / 512;
    if (num_sectors == 0) return 0;

    esp_err_t ret = sdmmc_write_sectors(s_card, buffer, lba, num_sectors);
    if (ret == ESP_OK) {
        return (int32_t)bufsize;
    }
    ESP_LOGE(TAG, "write10 lba=%lu sectors=%lu failed", (unsigned long)lba, (unsigned long)num_sectors);
    return -1; /* STALL */
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                         void *buffer, uint16_t bufsize)
{
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;

    /* Return 0 to let TinyUSB handle unsupported commands */
    return 0;
}

/* ── USB device descriptor ───────────────────────────────────────────── */

#define EPNUM_MSC           1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum { EDPT_MSC_OUT = 0x01, EDPT_MSC_IN = 0x81 };

static tusb_desc_device_t const s_desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB          = 0x0200,
    .bDeviceClass    = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor        = 0x303A,
    .idProduct       = 0x4002,
    .bcdDevice       = 0x0100,
    .iManufacturer   = 0x01,
    .iProduct        = 0x02,
    .iSerialNumber   = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const s_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static char const *s_desc_strings[] = {
    (char const[]){ 0x09, 0x04 }, /* 0: LANG = English */
    "Espressif",                   /* 1: Manufacturer */
    "TF Card USB Drive",           /* 2: Product */
    "123456",                      /* 3: Serial */
};

/* ── Descriptor callbacks ────────────────────────────────────────────── */

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];
    uint8_t len;

    if (index == 0) {
        desc[0] = 0x0409; /* English */
        len = 1;
    } else {
        if (index >= sizeof(s_desc_strings) / sizeof(s_desc_strings[0])) {
            return NULL;
        }
        const char *str = s_desc_strings[index];
        len = (uint8_t)strlen(str);
        if (len > 31) len = 31;

        desc[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * len + 2));
        for (uint8_t i = 0; i < len; i++) {
            desc[i + 1] = (uint16_t)str[i];
        }
    }
    return desc;
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t tusb_msc_init(sdmmc_card_t *card)
{
    s_card = card;

    ESP_RETURN_ON_ERROR(usb_phy_init(), TAG, "USB PHY init");

    tusb_init();
    ESP_LOGI(TAG, "TinyUSB initialized");

    /* Create USB device task for tud_task() processing */
    BaseType_t ret = xTaskCreate(usb_device_task, "usbd", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "USB task create failed");

    ESP_LOGI(TAG, "USB MSC ready — connect USB to see TF card as a drive");
    return ESP_OK;
}
