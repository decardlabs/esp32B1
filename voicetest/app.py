"""Voice TTS Server - FastAPI Backend

Provides TTS synthesis via 讯飞 and 火山引擎, format conversion,
audio file management, and request logging.
"""

import json
import logging
import os
import time
import uuid
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# Try to import user config; fall back to example
try:
    from config import IFLYTEK_CONFIG, VOLCANO_CONFIG, SERVER_CONFIG
except ImportError:
    from config_example import IFLYTEK_CONFIG, VOLCANO_CONFIG, SERVER_CONFIG
    logging.warning("使用 config_example.py（默认配置），请复制为 config.py 并填入 API 凭证")

from tts_engines.iflytek import IflytekTTS
from tts_engines.volcano import VolcanoTTS
from audio_utils import pcm_to_wav, convert_with_ffmpeg, FFMPEG_FORMATS, get_ffmpeg_available

# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("voice-tts")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DOC_DIR = Path(SERVER_CONFIG.get("DOC_DIR", "doc"))
DOC_DIR.mkdir(exist_ok=True)

LOG_DIR = Path(SERVER_CONFIG.get("LOG_DIR", "logs"))
LOG_DIR.mkdir(exist_ok=True)

MAX_LOG_ENTRIES = SERVER_CONFIG.get("MAX_LOG_ENTRIES", 100)

# In-memory log store
request_logs: list[dict] = []

# ---------------------------------------------------------------------------
# TTS Engine initialization
# ---------------------------------------------------------------------------

# Validate configs
iflytek_available = bool(IFLYTEK_CONFIG.get("APP_ID") and IFLYTEK_CONFIG.get("API_KEY"))

# Volcano supports both new console (API_KEY) and old console (ACCESS_TOKEN + APP_ID) auth
volcano_api_key = VOLCANO_CONFIG.get("API_KEY", "")
volcano_access_token = VOLCANO_CONFIG.get("ACCESS_TOKEN", "")
volcano_app_id = VOLCANO_CONFIG.get("APP_ID", "")
volcano_available = bool(volcano_api_key) or bool(volcano_access_token and volcano_app_id)

iflytek_engine: Optional[IflytekTTS] = None
volcano_engine: Optional[VolcanoTTS] = None

if iflytek_available:
    iflytek_engine = IflytekTTS(
        app_id=IFLYTEK_CONFIG["APP_ID"],
        api_key=IFLYTEK_CONFIG["API_KEY"],
        api_secret=IFLYTEK_CONFIG["API_SECRET"],
    )
    logger.info("讯飞 TTS 引擎已初始化")

if volcano_available:
    volcano_engine = VolcanoTTS(
        api_key=volcano_api_key,
        access_token=volcano_access_token,
        app_id=volcano_app_id,
        cluster=VOLCANO_CONFIG.get("CLUSTER", "volcano_tts"),
    )
    logger.info("火山引擎 TTS 已初始化")

# ffmpeg check
ffmpeg_ok = get_ffmpeg_available()
if not ffmpeg_ok:
    logger.warning("ffmpeg 未安装，MP3/OGG/FLAC/Opus 转换不可用")


def _add_log(entry: dict):
    """Add a log entry to the in-memory store and persist to disk."""
    request_logs.insert(0, entry)
    if len(request_logs) > MAX_LOG_ENTRIES:
        request_logs.pop()

    # Also write to daily log file
    log_file = LOG_DIR / f"tts_{datetime.now().strftime('%Y%m%d')}.log"
    try:
        with open(log_file, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception as e:
        logger.error("写入日志文件失败: %s", e)


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------

class TTSRequest(BaseModel):
    text: str
    engine: str = "iflytek"  # "iflytek" or "volcano"
    format: str = "wav"
    voice: Optional[str] = None


class TTSResponse(BaseModel):
    success: bool
    message: str = ""
    file_url: str = ""
    filename: str = ""
    file_size: int = 0
    duration_ms: float = 0
    request_id: str = ""


# ---------------------------------------------------------------------------
# FastAPI lifespan
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Voice TTS Server 启动")
    logger.info("  讯飞 TTS: %s", "✅ 已配置" if iflytek_available else "❌ 未配置")
    logger.info("  火山引擎: %s", "✅ 已配置" if volcano_available else "❌ 未配置")
    logger.info("  ffmpeg:   %s", "✅ 可用" if ffmpeg_ok else "❌ 不可用")
    logger.info("  doc 目录: %s", DOC_DIR.resolve())
    yield
    logger.info("Voice TTS Server 关闭")


# ---------------------------------------------------------------------------
# Create FastAPI app
# ---------------------------------------------------------------------------

app = FastAPI(title="Voice TTS", version="1.0.0", lifespan=lifespan)


# ---------------------------------------------------------------------------
# API Routes
# ---------------------------------------------------------------------------

@app.get("/api/status")
def get_status():
    """Return server status and available engines."""
    return {
        "status": "ok",
        "engines": {
            "iflytek": {
                "available": iflytek_available,
                "formats": IflytekTTS.get_supported_formats(),
                "voices": IflytekTTS.get_voices(),
            },
            "volcano": {
                "available": volcano_available,
                "formats": VolcanoTTS.get_supported_formats(),
                "voices": VolcanoTTS.get_voices(),
            },
        },
        "ffmpeg_available": ffmpeg_ok,
        "formats_all": {
            "iflytek": IflytekTTS.get_supported_formats(),
            "volcano": VolcanoTTS.get_supported_formats(),
        },
    }


@app.get("/api/voices")
def get_voices(engine: str = Query("iflytek", description="Engine name")):
    """Get available voices for the specified engine."""
    if engine == "iflytek":
        return {"engine": "iflytek", "voices": IflytekTTS.get_voices()}
    elif engine == "volcano":
        return {"engine": "volcano", "voices": VolcanoTTS.get_voices()}
    raise HTTPException(status_code=400, detail=f"不支持的引擎: {engine}")


@app.get("/api/formats")
def get_formats(engine: str = Query("iflytek", description="Engine name")):
    """Get supported audio formats for the specified engine."""
    if engine == "iflytek":
        formats = IflytekTTS.get_supported_formats()
    elif engine == "volcano":
        formats = VolcanoTTS.get_supported_formats()
    else:
        raise HTTPException(status_code=400, detail=f"不支持的引擎: {engine}")

    # Filter out formats that need ffmpeg if not available
    if not ffmpeg_ok:
        formats = [f for f in formats if not f["requires_ffmpeg"]]

    return {"engine": engine, "formats": formats}


@app.post("/api/tts", response_model=TTSResponse)
def synthesize(req: TTSRequest):
    """Synthesize text to speech."""
    request_id = str(uuid.uuid4())[:8]
    start_time = time.time()

    # Validate
    if not req.text.strip():
        raise HTTPException(status_code=400, detail="文本不能为空")

    if len(req.text) > 5000:
        raise HTTPException(status_code=400, detail="文本过长，请限制在 5000 字符以内")

    # Select engine
    engine_name = req.engine.lower()
    if engine_name == "iflytek":
        if not iflytek_available:
            raise HTTPException(status_code=400, detail="讯飞 TTS 未配置，请检查 config.py")
        engine = iflytek_engine
        engine_voices = IflytekTTS.get_voices()
    elif engine_name == "volcano":
        if not volcano_available:
            raise HTTPException(status_code=400, detail="火山引擎 TTS 未配置，请检查 config.py")
        engine = volcano_engine
        engine_voices = VolcanoTTS.get_voices()
    else:
        raise HTTPException(status_code=400, detail=f"不支持的引擎: {engine_name}")

    # Validate format for engine
    supported = engine.get_supported_formats()
    supported_ids = {f["id"] for f in supported}
    if not ffmpeg_ok:
        supported_ids = {f["id"] for f in supported if not f["requires_ffmpeg"]}

    target_format = req.format.lower()
    if target_format not in supported_ids:
        raise HTTPException(
            status_code=400,
            detail=f"引擎 {engine_name} 不支持格式: {target_format}",
        )

    # Validate voice
    voice_id = req.voice or None
    if voice_id:
        valid_voice_ids = {v["id"] for v in engine_voices}
        if voice_id not in valid_voice_ids:
            logger.warning("音色 %s 不在预设列表中，仍尝试调用", voice_id)

    # Synthesize
    try:
        raw_audio = engine.synthesize(req.text, target_format, voice=voice_id)
    except Exception as e:
        elapsed = (time.time() - start_time) * 1000
        log_entry = {
            "time": datetime.now().isoformat(),
            "request_id": request_id,
            "engine": engine_name,
            "format": target_format,
            "voice": voice_id or "default",
            "text_preview": req.text[:80],
            "status": "error",
            "error": str(e),
            "elapsed_ms": round(elapsed, 1),
        }
        _add_log(log_entry)
        raise HTTPException(status_code=500, detail=f"TTS 合成失败: {e}")

    # Convert format if needed
    needs_conversion = any(
        f["id"] == target_format and f["needs_conversion"]
        for f in supported
    )

    if needs_conversion:
        if target_format == "wav":
            # PCM → WAV: pure Python
            audio_bytes = pcm_to_wav(raw_audio)
        elif target_format in FFMPEG_FORMATS:
            # PCM → other: needs ffmpeg
            converted = convert_with_ffmpeg(raw_audio, target_format)
            if converted is None:
                raise HTTPException(status_code=500, detail=f"格式转换失败 (ffmpeg 不可用)")
            audio_bytes = converted
        else:
            audio_bytes = raw_audio
    else:
        audio_bytes = raw_audio

    # Determine file extension
    ext_map = {
        "pcm": "pcm", "wav": "wav", "mp3": "mp3",
        "ogg": "ogg", "flac": "flac", "opus": "opus",
    }
    ext = ext_map.get(target_format, "bin")

    # Save files to doc/
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    audio_filename = f"{timestamp}_{request_id}.{ext}"
    text_filename = f"{timestamp}_{request_id}.txt"

    audio_path = DOC_DIR / audio_filename
    text_path = DOC_DIR / text_filename

    # Write audio file
    audio_path.write_bytes(audio_bytes)
    file_size = len(audio_bytes)

    # Write input text file
    text_path.write_text(req.text, encoding="utf-8")

    elapsed_ms = (time.time() - start_time) * 1000

    # Build response
    file_url = f"/doc/{audio_filename}"
    response = TTSResponse(
        success=True,
        message="合成成功",
        file_url=file_url,
        filename=audio_filename,
        file_size=file_size,
        duration_ms=round(elapsed_ms, 1),
        request_id=request_id,
    )

    # Log
    log_entry = {
        "time": datetime.now().isoformat(),
        "request_id": request_id,
        "engine": engine_name,
        "format": target_format,
        "voice": voice_id or "default",
        "text_preview": req.text[:80],
        "status": "success",
        "file": audio_filename,
        "file_size": file_size,
        "elapsed_ms": round(elapsed_ms, 1),
    }
    _add_log(log_entry)

    return response


@app.get("/api/logs")
def get_logs(limit: int = Query(50, description="Number of log entries")):
    """Return recent request logs."""
    return {"logs": request_logs[:limit]}


@app.get("/api/doc")
def list_doc_files():
    """List audio files in the doc directory."""
    files = []
    for f in sorted(DOC_DIR.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        if f.suffix in (".wav", ".mp3", ".pcm", ".ogg", ".flac", ".opus"):
            files.append({
                "name": f.name,
                "url": f"/doc/{f.name}",
                "size": f.stat().st_size,
                "modified": datetime.fromtimestamp(f.stat().st_mtime).isoformat(),
            })
    return {"files": files}


# ---------------------------------------------------------------------------
# Static file serving
# ---------------------------------------------------------------------------

# Mount doc/ for audio file access
app.mount("/doc", StaticFiles(directory=str(DOC_DIR)), name="doc")

# Serve frontend static files
static_dir = Path(__file__).parent / "static"
static_dir.mkdir(exist_ok=True)
app.mount("/", StaticFiles(directory=str(static_dir), html=True), name="static")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import uvicorn

    host = SERVER_CONFIG.get("HOST", "0.0.0.0")
    port = SERVER_CONFIG.get("PORT", 8000)

    print(f"🌐 Voice TTS Server: http://localhost:{port}")
    print(f"📁 Doc 目录: {DOC_DIR.resolve()}")

    uvicorn.run("app:app", host=host, port=port, reload=True)
