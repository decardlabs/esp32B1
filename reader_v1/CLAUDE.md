# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 Voice Reader — a USB Mass Storage + Chinese TTS reader device. When connected via USB it appears as a U-disk (TF card storage). In reading mode it reads `.txt` files aloud via I2S → ES8388 audio codec, with a TFT display (LVGL) showing the text.

## Build & Flash

```bash
# Source ESP-IDF environment (v5.5.1 in this project)
source ~/esp/esp-idf-v5.5.1/export.sh

# Configure (fetches LVGL, esp-sr dependencies)
idf.py reconfigure

# Build
idf.py build

# Flash
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial|usbserial' | head -n 1)
idf.py -p "$PORT" flash

# Monitor serial output
idf.py -p "$PORT" monitor

# Full rebuild from clean
idf.py fullclean && idf.py reconfigure && idf.py build
```

## Architecture

All source code lives flat in `main/` — no subdirectories. There are no tests.

### Hardware Abstraction Layer

| File | Purpose |
|------|---------|
| [board_pins.h](main/board_pins.h) | GPIO pin definitions, I2C addresses, I2S pins, display resolution |
| [i2c_bus.c](main/i2c_bus.c) | I2C0 master bus init (XL9555 + ES8388 share this bus) |
| [xl9555.c](main/xl9555.c) | GPIO expander: key reading, LCD reset/backlight, speaker enable |
| [es8388.c](main/es8388.c) | I2S audio codec init, volume, playback start/stop |
| [lcd_st7796.c](main/lcd_st7796.c) | ST7796 SPI TFT init and frame buffer send (320x480) |
| [bsp_spi.c](main/bsp_spi.c) | SPI2 bus init with large DMA transfer size + bus lock (shared by LCD and SD card) |
| [sd_card.c](main/sd_card.c) | SD card SPI init → FATFS mount → VFS at `/sdcard` |

### Application Layer

| File | Purpose |
|------|---------|
| [main.c](main/main.c) | State machine (`MODE_USB_MSC` / `MODE_READING`), sentence splitting, button dispatch |
| [app_buttons.c](main/app_buttons.c) | Dedicated FreeRTOS task scanning XL9555 keys with debounce (20ms interval) |
| [app_display.c](main/app_display.c) | LVGL UI: mode indicator, filename, scrollable text, progress bar, custom Heiti font renderer. LVGL runs in a dedicated FreeRTOS task |
| [app_tts.c](main/app_tts.c) | Esp-TTS engine (voice: Xiaole) + persistent worker task with callback-chaining, I2S → ES8388 output, beep via XL9555 |
| [lv_font_xiaozhi_cn_16.c](main/lv_font_xiaozhi_cn_16.c) | Custom Heiti 16px 2bpp CJK font bitmap for LVGL |
| [tusb_msc.c](main/tusb_msc.c) | TinyUSB MSC device: exposes TF card as USB drive. Callback on host write |

### Key Design Details

- **Shared SPI2 bus**: LCD and SD card share SPI2_HOST. The bus is initialized first by [main.c](main/main.c) with a large `max_transfer_sz` (LCD-compatible), then SD card and LCD add their devices on top.
- **State machine**: Three states — `MODE_MAIN_MENU` (default on boot), `MODE_USB_COPY`, and `MODE_READING`. Main menu lets user choose USB mode (KEY1) or reading mode (KEY2). Long press KEY1 (3s) in reading mode returns to menu.
- **Punctuation-based sentence splitting**: Chinese text is split by 。！？，：；、 and ASCII `. ! ? , \n` into up to 256 sentences, with a 72-byte max chunk limit to keep TTS input short.
- **LVGL CJK font**: Custom Heiti 16px 2bpp (`lv_font_xiaozhi_cn_16`) enables clear Chinese rendering with low flash usage. LVGL memory allocated from PSRAM.
- **Volume & Speed**: ES8388 volume range is -48 dB (mute) to 0 dB (max). Currently set to -12 dB in [app_tts.c](main/app_tts.c) init. Speed range is 0 (slowest) to 5 (fastest), default 2.
- **Button long press threshold**: 3000ms, defined in [app_buttons.h](main/app_buttons.h).

### Dependencies (managed_components/)

| Component | Version | Purpose |
|-----------|---------|---------|
| lvgl/lvgl | 8.4.0 | TFT UI framework |
| espressif/esp-sr | 2.4.6 | TTS engine (esp_tts, voice xiaole) |

### Key Configuration (sdkconfig.defaults)

- CPU: 240MHz, FreeRTOS tick 100Hz
- PSRAM: 8MB Octal, used via `MALLOC_CAP_SPIRAM`
- Flash: 16MB
- LVGL: 16-bit color depth, CJK font enabled
- Partition table: single large app (no OTA)
- TinyUSB: enabled for MSC device role
