from abc import ABC, abstractmethod
from typing import Optional


class TTSBase(ABC):
    """Abstract base class for TTS engine integrations."""

    @abstractmethod
    def synthesize(self, text: str, audio_format: str, voice: Optional[str] = None) -> bytes:
        """Synthesize text to audio.

        Args:
            text: Text to synthesize
            audio_format: Desired audio format (pcm, wav, mp3, etc.)
            voice: Voice type/name

        Returns:
            Raw audio data as bytes
        """
        ...

    @staticmethod
    @abstractmethod
    def get_supported_formats() -> list[dict]:
        """Return list of supported audio formats with metadata.

        Each entry: {"id": "wav", "label": "WAV", "description": "...",
                     "needs_conversion": False, "requires_ffmpeg": False}
        """
        ...

    @staticmethod
    @abstractmethod
    def get_voices() -> list[dict]:
        """Return list of available voices.

        Each entry: {"id": "xiaoyan", "name": "小燕", "gender": "female", "description": "..."}
        """
        ...
