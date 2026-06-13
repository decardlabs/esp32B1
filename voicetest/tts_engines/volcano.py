"""火山引擎 (Volcano Engine) TTS Engine Integration

Uses HTTP unidirectional streaming API:
  POST https://openspeech.bytedance.com/api/v3/tts/unidirectional

Auth: X-Api-Key + X-Api-Resource-Id (new console)
Response: SSE (text/event-stream) with base64-encoded audio chunks
"""

import base64
import json
import logging
import uuid
from typing import Optional

import requests

from .base import TTSBase

logger = logging.getLogger(__name__)

# seed-tts-2.0 (豆包语音合成模型2.0) 音色
DEFAULT_VOICES = [
    {"id": "zh_female_vv_uranus_bigtts", "name": "薇薇", "gender": "female", "description": "2.0 标准女声"},
    {"id": "zh_male_aha_mars_bigtts", "name": "阿浩", "gender": "male", "description": "2.0 标准男声"},
    {"id": "zh_female_xiaolei_mars_bigtts", "name": "晓蕾", "gender": "female", "description": "2.0 温柔女声"},
]

ENGINE_FORMATS = [
    {"id": "wav", "label": "WAV", "description": "原生支持，兼容性好", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "mp3", "label": "MP3", "description": "原生支持，文件小", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "pcm", "label": "PCM", "description": "原始 PCM 数据", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "opus", "label": "Opus", "description": "原生支持（ogg_opus）", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "ogg", "label": "OGG", "description": "需安装 ffmpeg", "needs_conversion": True, "requires_ffmpeg": True},
    {"id": "flac", "label": "FLAC", "description": "需安装 ffmpeg", "needs_conversion": True, "requires_ffmpeg": True},
]

FORMAT_MAP = {"wav": "wav", "mp3": "mp3", "pcm": "pcm", "opus": "ogg_opus", "ogg": "wav", "flac": "wav"}


class VolcanoTTS(TTSBase):
    """火山引擎 TTS (HTTP unidirectional streaming / SSE)."""

    def __init__(self, api_key: str = "", access_token: str = "", app_id: str = "", cluster: str = "volcano_tts"):
        self.api_key = api_key
        self.access_token = access_token
        self.app_id = app_id
        self.api_url = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
        self.api_resource_id = "seed-tts-2.0"

    def synthesize(self, text: str, audio_format: str, voice: Optional[str] = None) -> bytes:
        """Synthesize text via 火山引擎 HTTP unidirectional streaming API.

        POST JSON → receive SSE stream with base64-encoded audio chunks.
        """
        voice_type = voice or "zh_female_vv_uranus_bigtts"
        api_format = FORMAT_MAP.get(audio_format, "mp3")
        request_id = uuid.uuid4().hex

        # Build headers
        if self.api_key:
            headers = {
                "Content-Type": "application/json",
                "Accept": "text/event-stream, application/json",
                "X-Api-Key": self.api_key,
                "X-Api-Resource-Id": self.api_resource_id,
            }
        else:
            headers = {
                "Content-Type": "application/json",
                "Accept": "text/event-stream, application/json",
                "X-Api-App-Id": self.app_id,
                "X-Api-Access-Key": self.access_token,
                "X-Api-Resource-Id": self.api_resource_id,
            }

        # Build request body
        body = {
            "user": {"uid": "anonymous"},
            "namespace": "BidirectionalTTS",
            "req_params": {
                "text": text,
                "speaker": voice_type,
                "model": "seed-tts-2.0-standard",
                "audio_params": {
                    "format": api_format,
                    "sample_rate": 24000,
                },
            },
            "request_id": request_id,
        }

        logger.info("火山引擎请求: voice=%s format=%s text_len=%d", voice_type, api_format, len(text))

        response = requests.post(
            self.api_url,
            headers=headers,
            json=body,
            stream=True,
            timeout=(10, 60),
        )

        if response.status_code >= 400:
            raise RuntimeError(
                f"火山引擎 HTTP {response.status_code}: {response.text[:500]}"
            )

        # Parse SSE response
        audio_chunks = []
        for raw_line in response.iter_lines(decode_unicode=True):
            if not raw_line:
                continue
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

            code = event.get("code")
            msg = event.get("message", "")

            if code is not None and code not in (0, 20000000):
                raise RuntimeError(f"火山引擎错误 (code={code}): {msg}")

            data = event.get("data")
            if isinstance(data, str) and data:
                try:
                    audio_chunks.append(base64.b64decode(data))
                except Exception:
                    pass

            if code == 20000000:
                break

        if not audio_chunks:
            raise RuntimeError("未收到音频数据")

        audio_data = b"".join(audio_chunks)
        logger.info("火山引擎合成成功: %d bytes", len(audio_data))
        return audio_data

    @staticmethod
    def get_supported_formats() -> list[dict]:
        return list(ENGINE_FORMATS)

    @staticmethod
    def get_voices() -> list[dict]:
        return list(DEFAULT_VOICES)
