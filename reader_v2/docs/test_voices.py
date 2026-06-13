#!/usr/bin/env python3
"""Test Volcano Engine TTS with config.ini config and play audio aloud."""

import base64
import json
import os
import struct
import sys
import urllib.request
import ssl
import subprocess
import tempfile

# ── Read config from config.ini ─────────────────────────────────────────────

def read_config(path):
    cfg = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                cfg[key.strip()] = val.strip()
    except FileNotFoundError:
        pass
    return cfg

# Try loading from desktop first, then /sdcard
config = {}
for p in [
    os.path.expanduser("~/Desktop/config.ini"),
    "/sdcard/config.ini",
    "config.ini",
]:
    if os.path.exists(p):
        config = read_config(p)
        print(f"Loaded config from: {p}")
        break

if not config:
    print("No config.ini found, using defaults from doc")
    config = {}

API_KEY = config.get("TTS_API_KEY") or config.get("TTS_APP_KEY") or ""
RESOURCE_ID = config.get("TTS_RESOURCE_ID", "seed-tts-2.0")
VOICE = config.get("TTS_VOICE", "zh_female_vv_uranus_bigtts")

# ── TTS request ─────────────────────────────────────────────────────────────

API_URL = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
TEXT = "你好，系统已经准备就绪。现在正在进行语音测试。"

def synthesize(text, voice, api_key, resource_id, sample_rate=24000):
    body = json.dumps({
        "user": {"uid": "anonymous"},
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": voice,
            "model": "seed-tts-2.0-standard",
            "audio_params": {
                "format": "pcm",
                "sample_rate": sample_rate,
            },
        },
        "request_id": "test_py_001",
    }).encode("utf-8")

    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream, application/json",
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": resource_id,
    }

    req = urllib.request.Request(API_URL, data=body, headers=headers, method="POST")
    ctx = ssl.create_default_context()
    resp = urllib.request.urlopen(req, timeout=30, context=ctx)

    raw = resp.read().decode("utf-8", errors="replace")
    audio_chunks = []

    for raw_line in raw.split("\n"):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("data:"):
            line = line[5:].strip()
        if line == "[DONE]":
            break
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        code = event.get("code", 0)
        if code not in (0, 20000000):
            print(f"ERROR: code={code}: {event.get('message')}")
            return None
        data = event.get("data", "")
        if isinstance(data, str) and data:
            audio_chunks.append(base64.b64decode(data))
        if code == 20000000:
            break

    if not audio_chunks:
        print("ERROR: No audio data returned")
        return None

    return b"".join(audio_chunks)


def save_wav(pcm_data, sample_rate, filename):
    """Add WAV header to PCM data and save."""
    num_channels = 1
    bits_per_sample = 16
    byte_rate = sample_rate * num_channels * bits_per_sample // 8
    block_align = num_channels * bits_per_sample // 8
    data_size = len(pcm_data)

    with open(filename, "wb") as f:
        # RIFF header
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        # fmt chunk
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))  # chunk size
        f.write(struct.pack("<H", 1))   # PCM format
        f.write(struct.pack("<H", num_channels))
        f.write(struct.pack("<I", sample_rate))
        f.write(struct.pack("<I", byte_rate))
        f.write(struct.pack("<H", block_align))
        f.write(struct.pack("<H", bits_per_sample))
        # data chunk
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(pcm_data)

    return filename


# ── Main ────────────────────────────────────────────────────────────────────

SAMPLE_RATE = 24000  # Fixed per docs

print("=" * 60)
print(f"Volcano Engine TTS Test")
print(f"  API Key:   {API_KEY[:12]}...{API_KEY[-4:] if len(API_KEY) > 16 else ''}")
print(f"  Resource:  {RESOURCE_ID}")
print(f"  Voice:     {VOICE}")
print(f"  Text:      '{TEXT}'")
print(f"  Rate:      {SAMPLE_RATE}Hz")
print("=" * 60)

pcm_data = synthesize(TEXT, VOICE, API_KEY, RESOURCE_ID, SAMPLE_RATE)

if pcm_data is None:
    sys.exit(1)

duration = len(pcm_data) / (SAMPLE_RATE * 2)
print(f"\nReceived {len(pcm_data)} bytes PCM ({duration:.1f}s)")

# Save to WAV and play
wav_file = os.path.join(tempfile.gettempdir(), "tts_test_output.wav")
save_wav(pcm_data, SAMPLE_RATE, wav_file)
print(f"Saved WAV: {wav_file}")

# Play with afplay (macOS built-in)
print("Playing audio now...")
subprocess.run(["afplay", wav_file])
print("Done.")
