#include "tusb_msc.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "class/msc/msc_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"

static const char *TAG = "USB_MSC";

static sdmmc_card_t *s_card = NULL;
static tusb_msc_write_cb_t s_write_cb = NULL;
static bool s_usb_connected = false;
static bool s_ejected = false;

/* USB background task — runs forever once started. Never stopped/paused
 * so TinyUSB always responds to host requests on the bus. */
static TaskHandle_t s_usb_task = NULL;

/* Static task buffers (§1.1) */
#define USB_TASK_STACK 4096
static StackType_t s_usb_stack[USB_TASK_STACK];
static StaticTask_t s_usb_tcb;

/* USB PHY + TinyUSB stack: initialized once */
static bool s_phy_inited = false;
static usb_phy_handle_t s_phy_hdl = NULL;

/* SCSI command codes */
#define SCSI_CMD_TEST_UNIT_READY         0x00
#define SCSI_CMD_REQUEST_SENSE           0x03
#define SCSI_CMD_INQUIRY                 0x12
#define SCSI_CMD_PREVENT_ALLOW_REMOVAL   0x1E
#define SCSI_CMD_START_STOP_UNIT         0x1B
#define SCSI_CMD_READ_CAPACITY_10        0x25
#define SCSI_CMD_MODE_SENSE_6            0x1A
#define SCSI_CMD_MODE_SENSE_10           0x5A

#define SENSE_KEY_UNIT_ATTENTION    0x06
#define SENSE_KEY_NOT_READY         0x02
#define SENSE_KEY_ILLEGAL_REQUEST   0x05
#define ASC_MEDIUM_NOT_PRESENT      0x3A
#define ASCQ_MEDIUM_NOT_PRESENT     0x00

static void fill_sense_data(uint8_t buf[18], uint8_t key, uint8_t asc, uint8_t ascq)
{
    memset(buf, 0, 18);
    buf[0] = 0x70;
    buf[2] = key;
    buf[7] = 0x0A;
    buf[12] = asc;
    buf[13] = ascq;
}

/* ── USB PHY ─────────────────────────────────────────────────────────── */

static esp_err_t usb_phy_init(void)
{
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .otg_io_conf = NULL,
        .ext_io_conf = NULL,
    };
    return usb_new_phy(&phy_cfg, &s_phy_hdl);
}

/* ── TinyUSB task ────────────────────────────────────────────────────── */

static void usb_device_task(void *arg)
{
    (void)arg;
    uint32_t loop_cnt = 0;

    while (1) {
        loop_cnt++;
        if (loop_cnt % 10000 == 0) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[STACK] task=usbd watermark=%u words", (unsigned)watermark);
        }

        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ── MSC callbacks ───────────────────────────────────────────────────── */

uint8_t tud_msc_get_maxlun_cb(void) { return 1; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "ESP32-S3", 8);
    memcpy(product_id,  "Voice Reader  ", 16);
    memcpy(product_rev, "1.0", 3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (s_ejected) return false;
    return (s_card != NULL);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    if (s_ejected) {
        *block_count = 0;
        *block_size  = 512;
        return;
    }
    *block_count = s_card ? s_card->csd.capacity : 0;
    *block_size  = 512;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun; (void)offset;
    uint32_t num = bufsize / 512;
    if (num == 0) return 0;
    sd_card_lock();
    esp_err_t ret = sdmmc_read_sectors(s_card, buffer, lba, num);
    sd_card_unlock();
    return (ret == ESP_OK) ? (int32_t)bufsize : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)offset;
    uint32_t num = bufsize / 512;
    if (num == 0) return 0;
    sd_card_lock();
    esp_err_t ret = sdmmc_write_sectors(s_card, buffer, lba, num);
    sd_card_unlock();
    if (ret == ESP_OK) {
        if (s_write_cb) s_write_cb();
        return (int32_t)bufsize;
    }
    return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun;
    uint8_t cmd = scsi_cmd[0];

    switch (cmd) {

    case SCSI_CMD_TEST_UNIT_READY:
        return 0;

    case SCSI_CMD_REQUEST_SENSE:
        if (s_ejected) {
            fill_sense_data((uint8_t *)buffer, SENSE_KEY_NOT_READY,
                           ASC_MEDIUM_NOT_PRESENT, ASCQ_MEDIUM_NOT_PRESENT);
            return 18;
        }
        /* Return valid sense data with no error (key=0x00) */
        memset(buffer, 0, 18);
        return 18;

    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;

    case SCSI_CMD_START_STOP_UNIT: {
        bool start = (scsi_cmd[4] & 0x01) != 0;
        bool loej  = (scsi_cmd[4] & 0x02) != 0;
        if (!start && loej) {
            ESP_LOGI(TAG, "Eject requested by host");
            s_ejected = true;
        } else if (start) {
            s_ejected = false;
        }
        return 0;
    }

    case SCSI_CMD_MODE_SENSE_6:
    case SCSI_CMD_MODE_SENSE_10: {
        uint8_t page = scsi_cmd[2] & 0x3F;
        if (page == 0x00 || page == 0x3F) {
            memset(buffer, 0, bufsize);
            return (int32_t)bufsize;
        }
        return -1;
    }

    case SCSI_CMD_INQUIRY:
    case SCSI_CMD_READ_CAPACITY_10:
        return 0;

    default:
        ESP_LOGW(TAG, "Unhandled SCSI cmd: 0x%02x", cmd);
        return -1;
    }
}

void tud_mount_cb(void)
{
    s_usb_connected = true;
    s_ejected = false;  /* reset eject on host reconnect */
    ESP_LOGI(TAG, "USB mounted");
}

void tud_umount_cb(void)  { s_usb_connected = false; ESP_LOGI(TAG, "USB unmounted"); }
void tud_suspend_cb(bool r) { s_usb_connected = false; (void)r; }
void tud_resume_cb(void)  { s_usb_connected = true; }

/* ── USB descriptors (MSC only) ──────────────────────────────────────── */

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
    .idProduct       = 0x4003,
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
    (char const[]){ 0x09, 0x04 },
    "Espressif",
    "Voice Reader USB Drive",
    "VR001",
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t i)
{
    (void)i;
    return s_desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];
    uint8_t len;
    if (index == 0) {
        desc[0] = 0x0409; len = 1;
    } else {
        if (index >= sizeof(s_desc_strings) / sizeof(s_desc_strings[0])) return NULL;
        const char *str = s_desc_strings[index];
        len = (uint8_t)strlen(str);
        if (len > 31) len = 31;
        desc[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * len + 2));
        for (uint8_t i = 0; i < len; i++) desc[i + 1] = (uint16_t)str[i];
    }
    return desc;
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t tusb_msc_init(sdmmc_card_t *card)
{
    s_card = card;
    s_ejected = false;

    /* PHY and TinyUSB stack: init once */
    if (!s_phy_inited) {
        ESP_RETURN_ON_ERROR(usb_phy_init(), TAG, "USB PHY init");
        tusb_init();
        s_phy_inited = true;
        ESP_LOGI(TAG, "TinyUSB initialized (MSC only)");
    }

    /* Create USB task once; it runs forever (static, pinned to Core 1 per §1.1, §2.2) */
    if (s_usb_task == NULL) {
        s_usb_task = xTaskCreateStatic(usb_device_task, "usbd", USB_TASK_STACK,
                                        NULL, 5, s_usb_stack, &s_usb_tcb);
        if (s_usb_task == NULL) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

void tusb_msc_set_write_callback(tusb_msc_write_cb_t cb)
{
    s_write_cb = cb;
}

void tusb_msc_reset_eject(void)
{
    s_ejected = false;
}

void tusb_msc_disconnect(void)
{
    /* Force D+ low → host sees device disconnect.
     * Safe to call even if already disconnected. */
    tud_disconnect();
    s_usb_connected = false;
    s_ejected = false;
    ESP_LOGI(TAG, "USB disconnected (forced)");
}

void tusb_msc_connect(void)
{
    /* Re-enable D+ pull-up → host will detect new device and enumerate.
     * Safe to call even if already connected (no-op in TinyUSB). */
    tud_connect();
    ESP_LOGI(TAG, "USB connect requested (waiting for host enumeration)");
}

bool tusb_msc_is_connected(void)
{
    return s_usb_connected;
}

void tusb_msc_suspend(void)
{
    if (s_usb_task != NULL) {
        vTaskSuspend(s_usb_task);
    }
}

void tusb_msc_resume(void)
{
    if (s_usb_task != NULL) {
        vTaskResume(s_usb_task);
    }
}
