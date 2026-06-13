# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

kai-sound-tts is a TTS broadcasting system with the pipeline: **Web UI → PHP → Python TTS Bridge → (Volcengine TTS + MQTT) → ESP32 → I2S Speaker**.

### Architecture

| Layer | Location | Tech | Role |
|-------|----------|------|------|
| Web Console | `php-server/public/index.html` | Vanilla HTML/JS/CSS | Send text, control device |
| Orchestrator | `php-server/public/api/tts.php` | PHP 7.4+ / [php-mqtt/client](https://packagist.org/packages/php-mqtt/client) | Split text, pre-generate TTS segments, publish MQTT commands |
| TTS Bridge | `python-vol/tts_bridge_server.py` | Python 3 + requests | Proxy between PHP and Volcengine streaming TTS |
| MQTT Broker | External (Mosquitto) | Mosquitto | Message queue for PHP→ESP32 commands |
| Player | `arduino/sound/sound.ino` | C++ (Arduino) | Subscribe MQTT, fetch HTTP audio stream, I2S output |

### Data Flow

```
Web input → PHP (tts.php)
  → Python (POST /api/tts) → Volcengine streaming TTS
  → Returns stream_url
  → PHP publishes MQTT cmd=tts with url
  → ESP32 subscribes to device/{id}/command
  → ESP32 fetches http://.../tts/{id}.mp3
  → AudioFileSourceBuffer (32KB) → AudioGeneratorMP3/WAV → AudioOutputI2S → MAX98357A → Speaker
```

### PHP Text Segmentation

Long text is split (in `tts_request_handler.php`):
1. By `。！？!?；;` and newlines into sentences
2. Sentences longer than 100 chars are split by `，,、：:`
3. Remaining overlong segments are split by hard character limit
4. Short adjacent sentences are merged up to 70 chars per segment
5. Each segment is pre-generated via Python bridge before sequential MQTT dispatch

### ESP32 State Machine

States managed in `sound.ino`: `idle` → `playing` → `paused` → `idle`. Key behaviors:
- MQTT reconnection is suppressed during audio playback (avoids socket reset mid-stream)
- WiFi events monitored; playback interruption tracked via `wifiDroppedDuringPlayback`
- Fade-out on stop, immediate cleanup on natural finish
- Supports MP3 and WAV decoders only (no HTTPS, no M4A)

## Setup & Running

### Python TTS Bridge

```bash
cd python-vol
pip install -r requirements.txt
python3 tts_bridge_server.py --host 0.0.0.0 --port 9100
# Health check: curl http://127.0.0.1:9100/health
```

### PHP Server

```bash
cd php-server
composer install
# Point web server root to php-server/public/
```

### MQTT

```bash
mosquitto -p 1883 -d
# Or: systemctl start mosquitto
```

### Startup Order

1. Mosquitto
2. Python TTS bridge
3. PHP server (web server with `php-server/public/` as document root)
4. ESP32 (power on)
5. Open web console, send broadcast

## Configuration

All config files contain placeholder values — replace before deploy:

- `arduino/sound/sound.ino`: `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER`, `MQTT_USERNAME/PASSWORD`, `DEVICE_ID`
- `php-server/config.php`: `mqtt.*`, `python_tts.prepare_url`
- `python-vol/tts_bridge_config.py`: `VOLCENGINE_API_KEY`, `VOLCENGINE_RESOURCE_ID`, `VOLCENGINE_SPEAKER`, `TTS_PUBLIC_BASE_URL`

## API Endpoints

### `POST /api/tts.php` — Main TTS request
Body: `{ device_id, text, voice?, format?, speech_rate?, pitch_rate?, volume?, play_immediately? }`
- If `play_immediately=true` and multiple segments: pre-generates all segments, sends MQTT cmd=tts for each, waits for ESP32 to finish before sending next
- If single segment: generates, publishes MQTT, returns immediately

### `POST /api/command.php` — Direct MQTT command
Body: `{ device_id, cmd, params? }`
- Commands: `play` (url), `pause`, `resume`, `stop`, `volume` (value), `tone` (freq, duration), `status`

### `GET /api/tts-status.php?request_id=X` — Poll TTS generation status
Proxies to Python bridge `/api/tts/{id}`.

### Python Bridge (`:9100`)
- `POST /api/tts` — Create TTS task, returns `{ stream_url, request_id, status_url }`
- `GET /tts/{id}.{mp3|wav|pcm}` — Stream audio (used by ESP32)
- `GET /api/tts/{id}` — Poll task status
- `GET /health` — Health check
