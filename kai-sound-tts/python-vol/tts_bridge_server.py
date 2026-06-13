import argparse
import base64
import json
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import requests

from tts_bridge_config import (
    TTS_BRIDGE_HOST,
    TTS_BRIDGE_PORT,
    TTS_CONNECT_TIMEOUT_SECONDS,
    TTS_PUBLIC_BASE_URL,
    TTS_READ_TIMEOUT_SECONDS,
    TTS_REQUEST_TTL_SECONDS,
    VERBOSE_LOGGING,
    VOLCENGINE_API_KEY,
    VOLCENGINE_RESOURCE_ID,
    VOLCENGINE_SPEAKER,
    VOLCENGINE_TTS_URL,
)


MAX_TEXT_LENGTH = 2000


def log(message: str):
    now = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{now}] {message}", flush=True)


class BridgeConfig:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.public_base_url = TTS_PUBLIC_BASE_URL.rstrip("/")
        self.api_key = VOLCENGINE_API_KEY.strip()
        self.resource_id = VOLCENGINE_RESOURCE_ID.strip()
        self.speaker = VOLCENGINE_SPEAKER.strip()
        self.tts_url = VOLCENGINE_TTS_URL.strip()
        self.request_ttl = int(TTS_REQUEST_TTL_SECONDS)
        self.connect_timeout = int(TTS_CONNECT_TIMEOUT_SECONDS)
        self.read_timeout = int(TTS_READ_TIMEOUT_SECONDS)

    def ready(self):
        return bool(self.api_key and self.resource_id and self.speaker and self.tts_url)

    def missing(self):
        missing = []
        if not self.api_key:
            missing.append("VOLCENGINE_API_KEY")
        if not self.resource_id:
            missing.append("VOLCENGINE_RESOURCE_ID")
        if not self.speaker:
            missing.append("VOLCENGINE_SPEAKER")
        if not self.tts_url:
            missing.append("VOLCENGINE_TTS_URL")
        return missing


class TtsRequestStore:
    def __init__(self):
        self._lock = threading.Lock()
        self._items = {}

    def create(self, payload: dict, ttl_seconds: int) -> dict:
        request_id = uuid.uuid4().hex
        now = int(time.time())
        item = {
            "request_id": request_id,
            "payload": payload,
            "created_at": now,
            "expires_at": now + ttl_seconds,
            "status": {
                "stage": "created",
                "message": "request created",
                "chunk_count": 0,
                "audio_bytes": 0,
                "fetch_count": 0,
                "last_client_ip": "",
                "last_fetch_at": None,
                "updated_at": now,
                "first_chunk_at": None,
                "finished_at": None,
                "error": "",
            },
        }
        with self._lock:
            self._cleanup_locked(now)
            self._items[request_id] = item
        return item

    def get(self, request_id: str):
        now = int(time.time())
        with self._lock:
            self._cleanup_locked(now)
            item = self._items.get(request_id)
            if not item:
                return None
            return {
                "request_id": item["request_id"],
                "payload": dict(item["payload"]),
                "created_at": item["created_at"],
                "expires_at": item["expires_at"],
                "status": dict(item["status"]),
            }

    def update_status(self, request_id: str, **changes):
        now = int(time.time())
        with self._lock:
            item = self._items.get(request_id)
            if not item:
                return
            status = item["status"]
            status.update(changes)
            status["updated_at"] = now

    def touch_client_fetch(self, request_id: str, client_ip: str):
        now = int(time.time())
        with self._lock:
            item = self._items.get(request_id)
            if not item:
                return
            status = item["status"]
            status["fetch_count"] = int(status.get("fetch_count", 0)) + 1
            status["last_client_ip"] = client_ip
            status["last_fetch_at"] = now
            status["updated_at"] = now

    def _cleanup_locked(self, now: int):
        expired = [key for key, value in self._items.items() if value["expires_at"] <= now]
        for key in expired:
            del self._items[key]


def clamp_int(value, minimum: int, maximum: int, default: int) -> int:
    try:
        value = int(value)
    except (TypeError, ValueError):
        value = default
    return max(minimum, min(maximum, value))


def normalize_request(data: dict, config: BridgeConfig) -> dict:
    text = str(data.get("text", "")).strip()
    if not text:
        raise ValueError("text is required")
    if len(text) > MAX_TEXT_LENGTH:
        raise ValueError(f"text too long, max {MAX_TEXT_LENGTH} characters")

    audio_format = str(data.get("format", "mp3")).strip().lower() or "mp3"
    if audio_format not in ("mp3", "wav", "pcm"):
        raise ValueError("format must be one of mp3, wav, pcm")

    # Volcengine voices are tightly coupled to the selected resource id.
    # Keep the project default voice unless the caller is already passing a
    # Volcengine-style speaker name from the same family.
    incoming_voice = str(data.get("voice", "")).strip()
    voice = config.speaker
    if incoming_voice.startswith("zh_"):
        voice = incoming_voice

    return {
        "device_id": str(data.get("device_id", "esp32_01")).strip() or "esp32_01",
        "text": text,
        "voice": voice,
        "format": audio_format,
        "sample_rate": clamp_int(data.get("sample_rate", 24000), 8000, 48000, 24000),
        "volume": clamp_int(data.get("volume", 80), 0, 100, 80),
        "speech_rate": clamp_int(data.get("speech_rate", 0), -100, 100, 0),
        "pitch_rate": clamp_int(data.get("pitch_rate", 0), -100, 100, 0),
        "play_immediately": bool(data.get("play_immediately", True)),
    }


class StreamingSynthesizer:
    def __init__(self, config: BridgeConfig, payload: dict, store: TtsRequestStore, request_id: str):
        self.config = config
        self.payload = payload
        self.store = store
        self.request_id = request_id
        self.queue = []
        self.cv = threading.Condition()
        self.done = False
        self.error = None
        self.started = False
        self.started_at = time.time()

    def start(self):
        if self.started:
            return
        self.started = True
        if VERBOSE_LOGGING:
            log(
                f"[volc][{self.request_id}] create "
                f"device={self.payload['device_id']} voice={self.payload['voice']} "
                f"format={self.payload['format']} sample_rate={self.payload['sample_rate']} "
                f"text_len={len(self.payload['text'])}"
            )
        threading.Thread(target=self._run, daemon=True).start()

    def _push(self, chunk: bytes):
        now = int(time.time())
        with self.cv:
            self.queue.append(chunk)
            self.cv.notify_all()
        request = self.store.get(self.request_id)
        chunk_count = 1
        audio_bytes = len(chunk)
        first_chunk_at = now
        if request:
            chunk_count = int(request["status"].get("chunk_count", 0)) + 1
            audio_bytes = int(request["status"].get("audio_bytes", 0)) + len(chunk)
            first_chunk_at = request["status"].get("first_chunk_at") or now
        self.store.update_status(
            self.request_id,
            stage="streaming",
            message="audio chunk received",
            chunk_count=chunk_count,
            audio_bytes=audio_bytes,
            first_chunk_at=first_chunk_at,
        )
        if VERBOSE_LOGGING:
            elapsed_ms = int((time.time() - self.started_at) * 1000)
            log(
                f"[volc][{self.request_id}] chunk={chunk_count} size={len(chunk)}B "
                f"total={audio_bytes}B elapsed={elapsed_ms}ms"
            )

    def _finish(self, error: str = None):
        status = {
            "stage": "error" if error else "finished",
            "message": error or "stream finished",
            "finished_at": int(time.time()),
            "error": error or "",
        }
        self.store.update_status(self.request_id, **status)
        if VERBOSE_LOGGING:
            request = self.store.get(self.request_id)
            chunk_count = 0
            audio_bytes = 0
            if request:
                chunk_count = int(request["status"].get("chunk_count", 0))
                audio_bytes = int(request["status"].get("audio_bytes", 0))
            elapsed_ms = int((time.time() - self.started_at) * 1000)
            if error:
                log(
                    f"[volc][{self.request_id}] error "
                    f"chunks={chunk_count} total={audio_bytes}B elapsed={elapsed_ms}ms "
                    f"detail={error}"
                )
            else:
                log(
                    f"[volc][{self.request_id}] finished "
                    f"chunks={chunk_count} total={audio_bytes}B elapsed={elapsed_ms}ms"
                )
        with self.cv:
            self.error = error
            self.done = True
            self.cv.notify_all()

    def _build_request(self) -> dict:
        reqid = uuid.uuid4().hex
        # This payload matches the current Volcengine unidirectional streaming API.
        return {
            "user": {
                "uid": self.payload["device_id"],
            },
            "namespace": "BidirectionalTTS",
            "req_params": {
                "text": self.payload["text"],
                "speaker": self.payload["voice"],
                "model": "seed-tts-2.0-standard",
                "audio_params": {
                    "format": self.payload["format"],
                    "sample_rate": self.payload["sample_rate"],
                },
            },
            "request_id": reqid,
        }

    def _iter_audio_chunks(self, response: requests.Response):
        # Volcengine returns line-based streaming events. Each data line contains
        # a JSON envelope, and the actual audio chunk is base64-encoded in data.
        for raw_line in response.iter_lines(decode_unicode=True):
            if not raw_line:
                continue

            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("data:"):
                line = line[5:].strip()
            if line == "[DONE]":
                return

            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue

            code = event.get("code")
            message = str(event.get("message", ""))

            if code not in (0, 20000000, None):
                raise RuntimeError(f"volcengine code={code} message={message}")

            data = event.get("data")
            if isinstance(data, str) and data:
                yield base64.b64decode(data)
                continue

            if code == 20000000:
                return

    def _run(self):
        headers = {
            "Content-Type": "application/json",
            "Accept": "text/event-stream, application/json",
            "X-Api-Key": self.config.api_key,
            "X-Api-Resource-Id": self.config.resource_id,
        }

        try:
            self.store.update_status(self.request_id, stage="requesting", message="requesting volcengine stream")
            if VERBOSE_LOGGING:
                log(f"[volc][{self.request_id}] POST {self.config.tts_url}")
            response = requests.post(
                self.config.tts_url,
                headers=headers,
                json=self._build_request(),
                stream=True,
                timeout=(self.config.connect_timeout, self.config.read_timeout),
            )
        except Exception as exc:
            self._finish(str(exc))
            return

        if response.status_code >= 400:
            body = response.text[:1000]
            self._finish(f"volcengine http {response.status_code}: {body}")
            return

        try:
            self.store.update_status(self.request_id, stage="opened", message="stream opened")
            if VERBOSE_LOGGING:
                log(
                    f"[volc][{self.request_id}] response status={response.status_code} "
                    f"content_type={response.headers.get('Content-Type', '')}"
                )
            has_audio = False
            for chunk in self._iter_audio_chunks(response):
                has_audio = True
                self._push(chunk)
            if not has_audio:
                self._finish("no audio chunk received from volcengine")
                return
            self._finish()
        except Exception as exc:
            self._finish(str(exc))
        finally:
            response.close()

    def wait_first_chunk(self, timeout: float = 15.0):
        deadline = time.time() + timeout
        with self.cv:
            while True:
                if self.queue:
                    return self.queue.pop(0)
                if self.done:
                    if self.error:
                        raise RuntimeError(self.error)
                    raise RuntimeError("tts finished without audio data")
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise RuntimeError("waiting for first audio chunk timed out")
                self.cv.wait(timeout=min(0.5, remaining))

    def iter_remaining(self):
        with self.cv:
            while True:
                while self.queue:
                    yield self.queue.pop(0)
                if self.done:
                    if self.error:
                        raise RuntimeError(self.error)
                    return
                self.cv.wait(timeout=0.5)


def make_handler(config: BridgeConfig, store: TtsRequestStore):
    class Handler(BaseHTTPRequestHandler):
        server_version = "VolcengineTtsBridge/1.0"

        def log_message(self, format, *args):
            print("[%s] %s" % (self.log_date_time_string(), format % args))

        def _send_json(self, status: int, payload: dict):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        def _read_json(self):
            content_length = int(self.headers.get("Content-Length", "0") or "0")
            raw = self.rfile.read(content_length) if content_length > 0 else b""
            try:
                return json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid json: {exc.msg}") from exc

        def _audio_content_type(self, audio_format: str) -> str:
            if audio_format == "wav":
                return "audio/wav"
            if audio_format == "pcm":
                return "application/octet-stream"
            return "audio/mpeg"

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "service": "volcengine-tts-bridge",
                        "ready": config.ready(),
                        "missing": config.missing(),
                        "public_base_url": config.public_base_url,
                        "tts_url": config.tts_url,
                        "speaker": config.speaker,
                        "resource_id": config.resource_id,
                    },
                )
                return

            if parsed.path.startswith("/api/tts/"):
                request_id = parsed.path.split("/")[-1]
                request_item = store.get(request_id)
                if not request_item:
                    self._send_json(404, {"ok": False, "error": "request not found or expired"})
                    return
                self._send_json(
                    200,
                    {
                        "ok": True,
                        "request_id": request_id,
                        "device_id": request_item["payload"]["device_id"],
                        "text_preview": request_item["payload"]["text"][:60],
                        "voice_used": request_item["payload"]["voice"],
                        "format": request_item["payload"]["format"],
                        "status": request_item["status"],
                    },
                )
                return

            if not parsed.path.startswith("/tts/") or not parsed.path.endswith(".mp3") and not parsed.path.endswith(".wav") and not parsed.path.endswith(".pcm"):
                self._send_json(404, {"ok": False, "error": "not found"})
                return

            request_id = parsed.path.split("/")[-1].split(".")[0]
            request_item = store.get(request_id)
            if not request_item:
                self._send_json(404, {"ok": False, "error": "request not found or expired"})
                return
            if not config.ready():
                self._send_json(500, {"ok": False, "error": "missing volcengine config", "missing": config.missing()})
                return

            payload = request_item["payload"]
            streamer = StreamingSynthesizer(config, payload, store, request_id)
            # `/tts/...` is the bridge point between the device and Volcengine:
            # ESP32 connects here once, and this handler forwards the upstream
            # audio stream chunk by chunk as soon as data arrives.
            store.update_status(request_id, stage="connecting_client", message="esp32 or browser connected")
            store.touch_client_fetch(request_id, self.client_address[0])
            if VERBOSE_LOGGING:
                log(
                    f"[bridge][{request_id}] client={self.client_address[0]} "
                    f"format={payload['format']} voice={payload['voice']} "
                    f"fetch_count={store.get(request_id)['status'].get('fetch_count', 0)}"
                )

            self.send_response(200)
            self.send_header("Content-Type", self._audio_content_type(payload["format"]))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            try:
                self.wfile.flush()
            except Exception:
                pass

            streamer.start()

            try:
                first_chunk = streamer.wait_first_chunk()
                self.wfile.write(first_chunk)
                self.wfile.flush()
                for chunk in streamer.iter_remaining():
                    self.wfile.write(chunk)
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                if VERBOSE_LOGGING:
                    log(f"[bridge][{request_id}] client disconnected early")
                pass
            except Exception as exc:
                log(f"[bridge][{request_id}] streaming error: {exc}")

        def do_POST(self):
            parsed = urlparse(self.path)
            if parsed.path != "/api/tts":
                self._send_json(404, {"ok": False, "error": "not found"})
                return

            try:
                data = self._read_json()
                payload = normalize_request(data, config)
            except ValueError as exc:
                self._send_json(400, {"ok": False, "error": str(exc)})
                return

            item = store.create(payload, config.request_ttl)
            request_id = item["request_id"]
            stream_url = f"{config.public_base_url}/tts/{request_id}.{payload['format']}"
            status_url = f"{config.public_base_url}/api/tts/{request_id}"
            if VERBOSE_LOGGING:
                log(
                    f"[bridge][{request_id}] accepted "
                    f"device={payload['device_id']} voice={payload['voice']} "
                    f"format={payload['format']} text_len={len(payload['text'])} "
                    f"text_preview={payload['text'][:40]!r}"
                )
            self._send_json(
                200,
                {
                    "ok": True,
                    "request_id": request_id,
                    "stream_url": stream_url,
                    "format": payload["format"],
                    "expires_at": item["expires_at"],
                    "device_id": payload["device_id"],
                    "text_preview": payload["text"][:60],
                    "voice_used": payload["voice"],
                    "status_url": status_url,
                    "ready": config.ready(),
                    "missing": config.missing(),
                    "provider": "volcengine",
                },
            )
            if VERBOSE_LOGGING:
                log(
                    f"[bridge][{request_id}] issued "
                    f"stream_url={stream_url} status_url={status_url}"
                )

    return Handler


def parse_args():
    parser = argparse.ArgumentParser(description="Volcengine TTS bridge server")
    parser.add_argument("--host", default=TTS_BRIDGE_HOST)
    parser.add_argument("--port", type=int, default=TTS_BRIDGE_PORT)
    return parser.parse_args()


def main():
    args = parse_args()
    config = BridgeConfig(args.host, args.port)
    store = TtsRequestStore()
    handler = make_handler(config, store)

    server = ThreadingHTTPServer((args.host, args.port), handler)
    log("Volcengine TTS bridge running")
    log(f"listen: http://{args.host}:{args.port}")
    log(f"public: {config.public_base_url}")
    log(f"tts_url: {config.tts_url}")
    if not config.ready():
        log("missing config: " + ", ".join(config.missing()))
    server.serve_forever()


if __name__ == "__main__":
    main()
