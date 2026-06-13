# Voice TTS - Configuration
# Copy this file to config.py and fill in your API credentials

# ========================================
# 讯飞 (iFlytek) TTS Configuration
# ========================================
# Get these from https://www.xfyun.cn/service/tts
IFLYTEK_CONFIG = {
    "APP_ID": "",       # 应用 ID
    "API_KEY": "",      # API 密钥
    "API_SECRET": "",   # API 密钥
}

# ========================================
# 火山引擎 (Volcano Engine) TTS Configuration
# ========================================
# Get these from https://console.volcengine.com/speech
#
# 新版控制台（推荐）：只需 API_KEY
#   新版控制台的 API Key 是 UUID 格式，例如 "423199ee-f156-411b-84d6-ff2469c54a34"
#
# 旧版控制台：需要 ACCESS_TOKEN + APP_ID + CLUSTER
#
VOLCANO_CONFIG = {
    # 新版控制台（推荐）
    "API_KEY": "",              # UUID 格式的 API Key

    # 旧版控制台（二选一）
    "ACCESS_TOKEN": "",         # Access Token
    "APP_ID": "",               # 应用 ID
    "CLUSTER": "volcano_tts",   # Cluster 名称
}

# ========================================
# Server Configuration
# ========================================
SERVER_CONFIG = {
    "HOST": "0.0.0.0",
    "PORT": 8000,
    "DOC_DIR": "doc",          # Input text and audio files storage
    "LOG_DIR": "logs",         # Log files directory
    "MAX_LOG_ENTRIES": 100,    # Max log entries kept in memory
}
