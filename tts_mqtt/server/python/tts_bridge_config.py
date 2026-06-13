# Volcengine TTS Authentication
VOLCENGINE_APP_ID = "YOUR_APP_ID"
VOLCENGINE_ACCESS_TOKEN = "YOUR_ACCESS_TOKEN"
VOLCENGINE_RESOURCE_ID = "seed-tts-2.0"
VOLCENGINE_SPEAKER = "zh_female_vv_uranus_bigtts"

# Volcengine HTTP unidirectional streaming TTS endpoint.
VOLCENGINE_TTS_URL = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"

# Public URL that ESP32 can access (MUST be ESP32 reachable, NOT 127.0.0.1)
TTS_PUBLIC_BASE_URL = "http://YOUR_SERVER_IP:9100"

TTS_BRIDGE_HOST = "0.0.0.0"
TTS_BRIDGE_PORT = 9100
TTS_REQUEST_TTL_SECONDS = 600
TTS_CONNECT_TIMEOUT_SECONDS = 10
TTS_READ_TIMEOUT_SECONDS = 120
VERBOSE_LOGGING = True

# Backward compat
VOLCENGINE_API_KEY = VOLCENGINE_ACCESS_TOKEN
