"""讯飞 (iFlytek) TTS Engine Integration

Uses the WebSocket API: wss://tts-api.xfyun.cn/v2/tts

Authentication: HMAC-SHA256 signature with API key/secret
Protocol: Send JSON frame, receive audio chunks via WebSocket
"""

import base64
import hashlib
import hmac
import json
import ssl
from datetime import datetime
from time import mktime
from typing import Optional
from urllib.parse import urlencode
from wsgiref.handlers import format_date_time

import websocket

from .base import TTSBase

DEFAULT_VOICES = [
    {"id": "x4_xiaoyan", "name": "小燕", "gender": "female", "description": "标准女声"},
    {"id": "x4_xiaofeng", "name": "小峰", "gender": "male", "description": "标准男声"},
    {"id": "x4_xiaowei", "name": "小薇", "gender": "female", "description": "温柔女声"},
    {"id": "x4_xiaolin", "name": "小林", "gender": "male", "description": "亲和男声"},
    {"id": "x4_xiaomei", "name": "小美", "gender": "female", "description": "甜美女声"},
    {"id": "x4_xiaogang", "name": "小刚", "gender": "male", "description": "浑厚男声"},
]

ENGINE_FORMATS = [
    {"id": "mp3", "label": "MP3", "description": "直接输出，兼容性好", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "wav", "label": "WAV", "description": "PCM 转 WAV，无需 ffmpeg", "needs_conversion": True, "requires_ffmpeg": False},
    {"id": "pcm", "label": "PCM", "description": "原始 PCM 数据", "needs_conversion": False, "requires_ffmpeg": False},
    {"id": "ogg", "label": "OGG", "description": "需安装 ffmpeg", "needs_conversion": True, "requires_ffmpeg": True},
    {"id": "flac", "label": "FLAC", "description": "需安装 ffmpeg", "needs_conversion": True, "requires_ffmpeg": True},
    {"id": "opus", "label": "Opus", "description": "需安装 ffmpeg", "needs_conversion": True, "requires_ffmpeg": True},
]


class IflytekTTS(TTSBase):
    """讯飞 TTS engine implementation."""

    def __init__(self, app_id: str, api_key: str, api_secret: str):
        self.app_id = app_id
        self.api_key = api_key
        self.api_secret = api_secret
        self.base_url = "wss://tts-api.xfyun.cn/v2/tts"

    def _build_auth_url(self) -> str:
        """Build WebSocket URL with authentication parameters.

        Matches official 讯飞 TTS v2 demo: uses ws-api.xfyun.cn as host
        in the signature and datetime.now() with format_date_time.
        """
        now = datetime.now()
        date = format_date_time(mktime(now.timetuple()))

        # Build signature string
        signature_origin = f"host: ws-api.xfyun.cn\ndate: {date}\nGET /v2/tts HTTP/1.1"

        # HMAC-SHA256 sign
        signature = hmac.new(
            self.api_secret.encode("utf-8"),
            signature_origin.encode("utf-8"),
            hashlib.sha256,
        ).digest()
        signature_base64 = base64.b64encode(signature).decode("utf-8")

        # Build authorization string
        authorization_origin = (
            f'api_key="{self.api_key}", '
            f'algorithm="hmac-sha256", '
            f'headers="host date request-line", '
            f'signature="{signature_base64}"'
        )
        authorization = base64.b64encode(authorization_origin.encode("utf-8")).decode("utf-8")

        # Build URL
        params = {
            "authorization": authorization,
            "date": date,
            "host": "ws-api.xfyun.cn",
        }
        return f"{self.base_url}?{urlencode(params)}"

    @staticmethod
    def _get_aue(format_id: str) -> str:
        """Map our format ID to 讯飞's `aue` parameter."""
        mapping = {
            "mp3": "lame",
            "wav": "raw",  # Get PCM, we'll convert to WAV
            "pcm": "raw",
            "ogg": "raw",
            "flac": "raw",
            "opus": "raw",
        }
        return mapping.get(format_id, "raw")

    def synthesize(self, text: str, audio_format: str, voice: Optional[str] = None) -> bytes:
        """Synthesize text using 讯飞 TTS WebSocket API.

        Connects, sends request, receives audio chunks, returns complete audio.

        Args:
            text: Text to synthesize
            audio_format: Target format id (mp3, wav, pcm, etc.)
            voice: Voice id (e.g., xiaoyan, xiaofeng)

        Returns:
            Complete audio bytes in the requested format
        """
        url = self._build_auth_url()
        aue = self._get_aue(audio_format)
        voice_name = voice or "x4_xiaoyan"

        audio_chunks = []
        errors = []

        def on_message(ws, message):
            try:
                data = json.loads(message)
                code = data.get("code", -1)
                if code != 0:
                    error_msg = data.get("message", f"Error code: {code}")
                    errors.append(f"讯飞 API 错误: {error_msg}")
                    ws.close()
                    return

                audio_base64 = data.get("data", {}).get("audio", "")
                if audio_base64:
                    audio_chunks.append(base64.b64decode(audio_base64))

                status = data.get("data", {}).get("status", 0)
                if status == 2:
                    ws.close()

            except json.JSONDecodeError:
                errors.append("讯飞 API 返回非 JSON 数据")
                ws.close()

        def on_error(ws, err):
            errors.append(f"WebSocket 错误: {err}")

        def on_close(ws, close_status_code, close_msg):
            pass

        def on_open(ws):
            business = {
                "aue": aue,
                "auf": "audio/L16;rate=16000",
                "vcn": voice_name,
                "speed": 50,
                "volume": 50,
                "pitch": 50,
                "tte": "utf8",
            }
            data = {
                "common": {"app_id": self.app_id},
                "business": business,
                "data": {
                    "status": 2,
                    "text": base64.b64encode(text.encode("utf-8")).decode("utf-8"),
                },
            }
            ws.send(json.dumps(data))

        ws = websocket.WebSocketApp(
            url,
            on_message=on_message,
            on_error=on_error,
            on_close=on_close,
            on_open=on_open,
        )

        ws.run_forever(sslopt={"cert_reqs": ssl.CERT_NONE})

        if errors:
            raise RuntimeError("; ".join(errors))

        if not audio_chunks:
            raise RuntimeError("未收到音频数据")

        audio_data = b"".join(audio_chunks)
        return audio_data

    @staticmethod
    def get_supported_formats() -> list[dict]:
        return list(ENGINE_FORMATS)

    @staticmethod
    def get_voices() -> list[dict]:
        return list(DEFAULT_VOICES)
