"""Audio format conversion utilities.

Provides functions to convert between audio formats.
Native WAV support via standard library; requires ffmpeg for MP3/OGG/FLAC/Opus.
"""

import io
import logging
import struct
import subprocess
import wave
from typing import Optional

logger = logging.getLogger(__name__)


def _check_ffmpeg() -> bool:
    """Check if ffmpeg is available on the system."""
    try:
        subprocess.run(
            ["ffmpeg", "-version"],
            capture_output=True,
            timeout=5,
        )
        return True
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False


def pcm_to_wav(pcm_data: bytes, sample_rate: int = 16000, channels: int = 1, sample_width: int = 2) -> bytes:
    """Convert PCM raw data to WAV format (pure Python, no ffmpeg needed).

    Args:
        pcm_data: Raw PCM audio data
        sample_rate: Sample rate in Hz (default 16000 for 讯飞)
        channels: Number of channels (default 1 for mono)
        sample_width: Sample width in bytes (default 2 for 16-bit)

    Returns:
        WAV file as bytes
    """
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data)
    return buf.getvalue()


def convert_with_ffmpeg(
    audio_data: bytes,
    target_format: str,
    sample_rate: int = 16000,
    channels: int = 1,
) -> Optional[bytes]:
    """Convert audio data using ffmpeg.

    Args:
        audio_data: Input audio data
        target_format: Target format (mp3, ogg, flac, opus)
        sample_rate: Input sample rate
        channels: Input number of channels

    Returns:
        Converted audio data, or None if ffmpeg not available
    """
    if not _check_ffmpeg():
        logger.warning("ffmpeg 未安装，无法转换格式: %s", target_format)
        return None

    try:
        # Determine input format (WAV header or raw PCM)
        # If it starts with "RIFF", it's already WAV
        if audio_data[:4] == b"RIFF":
            input_args = ["-f", "wav"]
        else:
            input_args = ["-f", "s16le", "-ar", str(sample_rate), "-ac", str(channels)]

        codec_map = {
            "mp3": "libmp3lame",
            "ogg": "libvorbis",
            "flac": "flac",
            "opus": "libopus",
        }
        codec = codec_map.get(target_format)

        # Container format (extension)
        ext_map = {
            "mp3": "mp3",
            "ogg": "ogg",
            "flac": "flac",
            "opus": "opus",
        }
        ext = ext_map.get(target_format, target_format)

        cmd = (
            ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"]
            + input_args
            + ["-i", "pipe:0"]
            + (["-c:a", codec] if codec else [])
            + ["-f", ext, "pipe:1"]
        )

        result = subprocess.run(cmd, input=audio_data, capture_output=True, timeout=60)

        if result.returncode != 0:
            logger.error("ffmpeg 转换失败: %s", result.stderr.decode())
            return None

        return result.stdout

    except subprocess.TimeoutExpired:
        logger.error("ffmpeg 转换超时")
        return None
    except FileNotFoundError:
        logger.error("ffmpeg 未找到")
        return None


def get_ffmpeg_available() -> bool:
    """Check ffmpeg availability and cache the result."""
    return _check_ffmpeg()


FFMPEG_FORMATS = {"mp3", "ogg", "flac", "opus"}
