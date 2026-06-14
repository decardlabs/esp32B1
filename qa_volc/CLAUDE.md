# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Golden Rules

- **Never make unfounded assumptions.** Before acting on a hypothesis (TLS config, register values, API behavior, timing issues), verify it against: (1) the actual code in this repo, (2) official SDK/library documentation (ESP-IDF docs, mbedtls, ES8388 datasheet), (3) working Python reference implementations in `tools/` or `auc_python/`. If you cannot verify, ask the user.
- **When in doubt about an API protocol, test it in Python first.** The `tools/demo_qa_loop.py` and `tools/verify_api.py` scripts can run on macOS with `requests` installed. Only port to C/ESP32 after confirming the protocol works from Python.

## Project Overview

qa_volc is an ESP32-S3 firmware for a voice Q&A device using Volcengine (火山引擎) APIs. Hardware: ESP32-S3 + ES8388 audio codec + ST7796 LCD (320x480) + SD card + WS2812 LEDs.

**Pipeline:** KEY3 press → I2S audio capture (24kHz stereo) → on-the-fly conversion to 16kHz mono → PSRAM buffer → LVGL task writes WAV to SD card → ASR (Volcengine Flash ASR) → LLM (Volcengine Ark Chat Completions API /v3/chat/completions) → display answer on LVGL UI.

## Build & Flash

```bash
# Full build + flash + monitor
cd /Users/macairm5/Documents/esp32/qa_volc && bash -c 'source ~/esp/esp-idf/export.sh && idf.py -p /dev/cu.usbmodem101 flash monitor'

# Build only
cd /Users/macairm5/Documents/esp32/qa_volc && bash -c 'source ~/esp/esp-idf/export.sh && idf.py build'

# Monitor only (after flash)
cd /Users/macairm5/Documents/esp32/qa_volc && bash -c 'source ~/esp/esp-idf/export.sh && idf.py -p /dev/cu.usbmodem101 monitor'

# Reconfigure after sdkconfig.defaults changes
rm -f sdkconfig && idf.py reconfigure
```

## Python Verification (on Mac)

Always verify API protocol changes in Python first before porting to C:

```bash
source /tmp/qa_venv/bin/activate && cd /Users/macairm5/Documents/esp32/qa_volc/tools

# Full TTS→ASR→LLM→TTS pipeline demo
python3 demo_qa_loop.py -c config.ini

# Individual API verification
python3 verify_api.py -c config.ini
```

The Python venv needs `requests` installed (`pip install requests`).

## Architecture

### Task Structure (FreeRTOS)

| Task | Stack | Priority | Core | Description |
|------|-------|----------|------|-------------|
| `qa_lvgl_task` | 8192 | 5 | 1 | LVGL UI + SPI SD card I/O + audio save bridge |
| `volc_asr` | 8192 | 5 | any | Volcengine Flash ASR: base64→HTTP→parse result |
| `volc_llm` | 16384 | 5 | any | Volcengine Ark Chat Completions: SSE streaming |
| `audio_capture` | 8192 | 5 | any | KEY3 triggered I2S capture + real-time 24k→16k conversion |
| `ws2812_task` | 6144 | 10 | any | WS2812 LED strip control |

### Data Flow

```
KEY3 press
  → audio_capture: I2S read (24kHz stereo) → stereo-to-mono → 3:2 downsample → PSRAM buffer
  → LVGL task (via queue + semaphore): save WAV to SD card → volc_asr_submit_data()
  → ASR task: base64 encode → HTTPS POST → parse → volc_llm_submit()
  → LLM task: HTTPS POST with SSE streaming → parse delta tokens → qa_ui_add_assistant_msg()
  → LVGL task: display tokens on screen
```

### Key Design Decisions

- **SPI bus conflict avoidance**: SD card (sdspi) and LCD (SPI2) share the same SPI bus. All SD card I/O (WAV write) is done inside the LVGL task via queue messages, since LVGL owns the SPI bus. ASR receives audio data in-memory (PSRAM buffer), avoiding SD card reads.
- **Audio conversion**: I2S delivers 24kHz stereo interleaved data. The capture loop converts to 16kHz mono on-the-fly: average L+R channels, then downsample 3:2.
- **Large buffer handling**: cJSON can't handle 85KB+ base64 strings. The ASR JSON request body is built with snprintf directly, not cJSON.
- **LLM API**: Uses Chat Completions format (`messages` array) via `/api/v3/chat/completions` (auto-converted from `/api/v3/responses` in config). SSE parsing handles both Chat Completions (`choices[0].delta.content`) and Responses API (`response.output_text.delta`) formats.
- **PSRAM for large allocations**: All audio buffers (>64KB) use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.

### Config (SD Card)

The device reads `config.ini` from the SD card (`/sdcard/config.ini`). Required keys:
- `WIFI_SSID`, `WIFI_PASS`
- `ASR_API_KEY`, `ASR_RESOURCE_ID` (default: `volc.seedasr.auc`)
- `LLM_API_KEY`, `LLM_MODEL`, `LLM_ENDPOINT`

The SDK uses a custom INI parser (`config_parser.c`) compatible with ESP32's fatfs.

### TLS Configuration

The endpoint `ark.cn-beijing.volces.com` requires TLS 1.3 with `TLS_AES_256_GCM_SHA384`. ESP-IDF defaults to TLS 1.2 only. The `sdkconfig.defaults` must explicitly enable TLS 1.3 (`CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`). The sdkconfig file cannot be just regenerated from defaults alone — TLS 1.3 may need to be written directly to `sdkconfig` if defaults don't take effect.

### SDK Config Key Settings

- `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` — PSRAM via `heap_caps_malloc()`
- `CONFIG_LV_FONT_FMT_TXT_LARGE=y` — Chinese font support
- `CONFIG_ESP_TLS_INSECURE=y`, `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` — skip cert verify (dev)
- `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` — required for Ark endpoint

Note: `idf.py reconfigure` doesn't reliably pick up new defaults. Changes to `sdkconfig.defaults` may require `rm -f sdkconfig` before `idf.py reconfigure`, or write the config directly to `sdkconfig`.

### Hardware Pin Mapping

- I2S: MCLK=3, BCLK=46, WS=9, DOUT=10, DIN=14
- LCD: SPI2 bus
- ES8388 codec: I2C address 0x10
- KEY3 (record): on xl9555 GPIO expander
- TF card: SPI mode (sdspi)
