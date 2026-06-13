# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# Requires ESP-IDF at ~/esp/esp-idf/ or ~/esp/esp-idf-v5.5.1/
./build.sh                    # full reconfigure + build
idf.py build                  # build only (after sourcing export.sh)
idf.py -p PORT flash          # flash (PORT e.g. /dev/cu.usbmodem*)
idf.py -p PORT monitor        # serial monitor
```

`sdkconfig.defaults` defines the ESP32-S3 target with 16MB flash / 8MB Octal PSRAM.

## Project Architecture

ESP32-S3 firmware: a TTS e-book reader that plays .txt files from an SD card through a speaker, with a 320x480 LCD display.

### State Machine (main.c)

Four states driven by 4 hardware buttons:

| State | Description |
|---|---|
| `ST_FILE_SELECT` | Browse .txt files on SD card (KEY1=next, KEY4=confirm) |
| `ST_READY` | File loaded, waiting to play (KEY4=play) |
| `ST_PLAYING` | TTS speaking sentences sequentially (KEY4=pause) |
| `ST_PAUSED` | Playback paused, resumes from current sentence (KEY4=resume) |

KEY1 in PLAYING/PAUSED returns to file select (stops playback). KEY2/KEY3 adjust volume/speed in all states.

### Module Layering

```
main.c  (state machine, file I/O, text encoding detection, sentence splitting)
  ├── app_tts.c      — TTS via esp-sr (Xiaole voice), I2S → ES8388 audio codec
  ├── app_display.c  — LVGL UI on ST7796 LCD, per-state screens
  ├── app_buttons.c  — Debounced button scan via XL9555 GPIO expander
  ├── sd_card.c      — FAT FS on SPI SD card, mounted at /sdcard
  ├── es8388.c       — Audio codec I2C init, volume, playback start/stop
  ├── lcd_st7796.c   — SPI LCD driver (320×480)
  ├── bsp_spi.c      — SPI2 bus (shared by LCD + SD card)
  ├── i2c_bus.c      — I2C0 bus (shared by XL9555 + ES8388)
  └── xl9555.c       — GPIO expander: keys, speaker enable, beep, LCD control
```

### Key Design Details

- **Shared SPI2 bus**: LCD and SD card share SPI2. The SD card driver uses `host.init_sdspi = false` to avoid re-initializing the already-set-up bus.
- **Shared I2C0 bus**: XL9555 (GPIO expander) and ES8388 (audio codec) share I2C0. Both drivers use mutex locks from `i2c_bus.c`.
- **TTS worker task**: A persistent task (`tts_worker_task`) with a 4-slot work queue. Single speech at a time — `app_tts_speak()` stops any current speech before queueing new work. Callback fires from worker task context.
- **TTS poll task**: polls `app_tts_is_busy()` every 100ms. When busy transitions to idle, advances to the next sentence. Has an idle-count heuristic (3 consecutive idle readings) as fallback.
- **LVGL task**: renders per-state screens when `s_dirty` flag is set. All display state is set synchronously from any task; the LVGL task only reads it.
- **Button debounce**: 20ms scan, 60ms debounce (3 consecutive stable reads). Long press on KEY1 (>3s) detected. Hardware beep feedback disabled by default (`ENABLE_KEY_BEEP 0`) — main.c handles beeps separately.
- **Text encoding**: auto-detects UTF-8 (with/without BOM), UTF-16 LE/BE (by BOM or zero-byte frequency heuristic), and converts to UTF-8. Sentences split on `. ! ? \n` and CJK punctuation (。！？).

### Managed Dependencies

- `espressif/esp-sr` — Chinese TTS engine (Xiaole voice)
- `espressif/esp-dsp` + `espressif/dl_fft` — DSP support
- `espressif/cjson` — JSON parsing
- `lvgl/lvgl` — graphics library

TTS voice data is embedded at build time (`esp_tts_voice_data_xiaole.dat`).

### Pin Mapping (board_pins.h)

| Peripheral | Pins |
|---|---|
| SPI2 (LCD + SD) | MOSI=11, CLK=12, MISO=13 |
| LCD | CS=21, DC=1 |
| SD card | CS=40 |
| I2C0 | SDA=41, SCL=42 |
| I2S0 | MCLK=3, BCLK=46, LRCK=9, DOUT=10 |
| XL9555 | addr 0x20 |
| ES8388 | addr 0x10 |
