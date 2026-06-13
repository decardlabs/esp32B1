# CLAUDE.md

> 文档同步版本：v2.1.0（2026-06-13）

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 Voice Reader — a USB Mass Storage + Chinese TTS reader device with cloud TTS support. When connected via USB it appears as a U-disk (TF card storage). In reading mode it reads `.txt` files aloud via I2S → ES8388 audio codec, with a TFT display (LVGL) showing the text. Supports both local (esp_tts) and cloud (Doubao/Volcano Engine TTS via WiFi) speech synthesis with automatic fallback.

## Build & Flash

```bash
# Source ESP-IDF environment (v5.5.1 in this project)
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh

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

## ESP-IDF v5.5.1 SMP FreeRTOS Quirks

- **`StackType_t` is `uint8_t`**: In ESP-IDF v5.5.1 with SMP FreeRTOS, `StackType_t` is defined as `uint8_t` because `configSTACK_DEPTH_TYPE = uint32_t` (stack depth in bytes). Array sizes match byte counts: `StackType_t stack[4096]` = 4096 bytes, and `xTaskCreateStatic(..., 4096, ...)` also expects 4096 bytes. **Do NOT multiply by `sizeof(StackType_t)`**.
- **`xTaskCreateStaticPinnedToCore`**: Exists but is a wrapper around `xTaskCreateStatic` when `CONFIG_FREERTOS_SMP` is not set. Prefer bare `xTaskCreateStatic` to avoid confusion.
- **`vTaskCoreAffinitySet`**: Only available when `CONFIG_FREERTOS_SMP` is enabled. Not available in this project's config.

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
| [main.c](main/main.c) | State machine (`MODE_MAIN_MENU` / `MODE_USB_COPY` / `MODE_READING`), sentence splitting, button dispatch, file scanning |
| [app_buttons.c](main/app_buttons.c) | Dedicated FreeRTOS task scanning XL9555 keys with debounce (20ms interval) |
| [app_display.c](main/app_display.c) | LVGL UI: mode indicator, filename, scrollable text, progress bar, custom Heiti font renderer. LVGL runs in a dedicated FreeRTOS task |
| [app_tts.c](main/app_tts.c) | Esp-TTS engine (voice: Xiaole) + persistent worker task with callback-chaining, I2S → ES8388 output, beep via XL9555 |
| [app_cloud_tts.c](main/app_cloud_tts.c) | Cloud TTS via Volcano Engine HTTP API (Doubao), SSE streaming → I2S playback, shares I2S with local TTS |
| [app_wifi.c](main/app_wifi.c) | WiFi station mode init, connect with stored credentials, IP event callback |
| [app_config.c](main/app_config.c) | `config.ini` parser on SD card: WiFi SSID/pass, cloud TTS credentials, voice selection |
| [app_oom.c](main/app_oom.c) | OOM graceful degradation: monitors internal heap, escalates through 6 levels (buffer-halve → TTS fallback → stop-and-alert) |
| [lv_font_xiaozhi_cn_16.c](main/lv_font_xiaozhi_cn_16.c) | Custom Heiti 16px 2bpp CJK font bitmap for LVGL |
| [tusb_msc.c](main/tusb_msc.c) | TinyUSB MSC device: exposes TF card as USB drive. Callback on host write |

### Key Design Details

- **Shared SPI2 bus**: LCD and SD card share SPI2_HOST. The bus is initialized first by [main.c](main/main.c) with a large `max_transfer_sz` (LCD-compatible), then SD card and LCD add their devices on top.
- **State machine**: Three states — `MODE_MAIN_MENU` (default on boot), `MODE_USB_COPY`, and `MODE_READING`. Main menu lets user choose USB mode (KEY1), local TTS reading (KEY2), or cloud TTS reading (KEY3). Long press KEY1 (3s) in reading mode returns to menu.
- **Cloud TTS automatic fallback**: If cloud TTS HTTP request fails, falls back to local TTS transparently. Cloud playback uses a dedicated temporary task, not the TTS worker queue.
- **Punctuation-based sentence splitting**: Chinese text is split by 。！？，：；、 and ASCII `. ! ? , \n` into up to 256 sentences, with a 72-byte max chunk limit to keep TTS input short.
- **LVGL CJK font**: Custom Heiti 16px 2bpp (`lv_font_xiaozhi_cn_16`) enables clear Chinese rendering with low flash usage. LVGL draw buffer allocated from internal DMA-capable RAM (no PSRAM fallback — DMA does not support PSRAM).
- **OOM graceful degradation**: `app_oom` monitors internal heap free size and largest block. On consecutive violations, escalates through 6 levels: buffer half → TTS prebuf shrink → disable UI FX → throttle → local TTS fallback → stop and alert.
- **WiFi auto-connect**: WiFi credentials loaded from `config.ini` on SD card. Connection initiated on `STA_START` event. IP acquisition triggers system-ready prompt via cloud TTS.
- **USB task suspend/resume**: When leaving USB mode (`return_to_main_menu()`), the USB task is suspended via `vTaskSuspend()` to prevent `tud_task()` from racing during mode switches. Resumed on re-entry.
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
- Console: UART0 (external USB-UART bridge), NOT USB-JTAG
- TWDT: enabled (15s timeout), PANIC disabled (warns only)
- INT WDT: 1200ms

## Known Bugs & Workarounds

### I2C XL9555 Transient Failure After Heavy I2C Activity
The I2C communication with the XL9555 GPIO expander may transiently fail (`ESP_ERR_INVALID_STATE`) during boot when called immediately after ES8388 init (also on I2C0). The root cause is unclear — possibly the I2C bus needs settling time after back-to-back transactions.
- **Workaround**: `app_buttons_init()` retried up to 3 times with 20ms delay in [main.c](main/main.c).
- If all retries fail, the device boots without buttons (graceful degradation).

### USB MSC Console Disconnect
When TinyUSB MSC is initialized (`tusb_msc_init()`), it takes over the USB OTG peripheral. On boards where USB-JTAG shares the same USB port, the serial monitor disconnects. The console via external UART (UART0 on GPIO 43/44) continues to work.
- **Monitor**: After entering USB mode, re-run `idf.py -p PORT monitor` after exiting USB mode.
- **Debug hint**: Consider using a USB-UART adapter on GPIO 43/44 for simultaneous console + USB MSC.

### USB Mode Switch Race Condition
The USB task (`usbd`, priority 5) can preempt the button event task (priority 2) during mode switches, creating a race with `tud_task()` internal state.
- **Fix**: `tusb_msc_suspend()`/`tusb_msc_resume()` — USB task is suspended when leaving USB mode (`return_to_main_menu()`) and resumed when re-entering (`enter_usb_mode()`). This prevents `tud_task()` from running during mode transitions.
- **Guard**: KEY1 mode switch guard (250ms debounce) prevents double-presses.

### LVGL Task Stack Size
The linter-added mutex-protected snapshot logic introduced large local buffers (`char text[2048]`, `char filename[64]`) in the LVGL task, causing stack overflow with the original 4096-byte stack.
- **Fix**: LVGL task stack increased to 8192 bytes.
- `StackType_t` is `uint8_t`, so `xTaskCreate(..., 8192, ...)` allocates exactly 8192 bytes.
- Current watermark: ~5320 words (~21KB peak usage on 8KB seems wrong — verify).

### Display Mutex Race
A linter-added `s_state_mutex` in `app_display.c` caused intermittent crashes during rapid mode switching. The mutex was unnecessary because:
- LVGL task (highest user priority, 5) is the only task calling LVGL APIs.
- Setter functions (lower priority tasks) only write to global variables, never call LVGL APIs.
- Data races on `s_text`/`s_mode` are benign (single-copy atomic on 32-bit).
- **Fix**: Reverted to direct global variable access (no mutex).
