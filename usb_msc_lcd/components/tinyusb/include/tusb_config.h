// tusb_config.h - TinyUSB configuration for ESP32-S3 MSC device
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+

// RHPort number used for device can be defined by the user
// in board.mk. Default to port 0.
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

// RHPort max operational speed can be defined by the user
// in board.mk. Default to Highspeed.
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

// Defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_ESP32S3
#endif

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS           OPT_OS_FREERTOS

// ESP-IDF v5.x uses "freertos/" namespace prefix for FreeRTOS headers
#define CFG_TUSB_OS_INC_PATH  freertos/

// Can be set by the user in top-level sdkconfig
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED       1

// Default is max speed that hardware controller could do with on-chip PHY
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

//--------------------------------------------------------------------
// Device Stack Configuration
//--------------------------------------------------------------------

// Endpoint 0 max packet size
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
// MSC
#define CFG_TUD_MSC             1
#define CFG_TUD_MSC_EP_BUFSIZE  512

//------------- CDC (not used, but TinyUSB needs some default) -------------//
#define CFG_TUD_CDC            0
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64

//------------- HID (not used) -------------//
#define CFG_TUD_HID            0

//------------- MIDI (not used) -------------//
#define CFG_TUD_MIDI           0

//------------- Vendor (not used) -------------//
#define CFG_TUD_VENDOR         0

// Total USB descriptor size (configuration descriptor + MSC descriptor)
#define CFG_TUD_CONFIG_DESC_SIZE (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H
