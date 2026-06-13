#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

// ==================== Network ====================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// The PHP service talks to ESP32 through MQTT.
const char* MQTT_BROKER   = "YOUR_MQTT_HOST";
const int   MQTT_PORT     = 1883;
const char* MQTT_USERNAME = "YOUR_MQTT_USERNAME";
const char* MQTT_PASSWORD = "YOUR_MQTT_PASSWORD";
const char* DEVICE_ID     = "esp32_01";

// ==================== I2S Speaker (MAX98357A) ====================
#define I2S_DOUT 25
#define I2S_BCLK 27
#define I2S_LRC  26

// ==================== Timers ====================
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long WIFI_CHECK_INTERVAL_MS     = 5000;
const unsigned long HEARTBEAT_INTERVAL_MS      = 10000;
const unsigned long PLAYBACK_LOG_INTERVAL_MS   = 2000;
const unsigned long MQTT_DEBUG_INTERVAL_MS     = 2000;
const unsigned long WIFI_RECOVER_TIMEOUT_MS    = 4000;
const uint32_t AUDIO_BUFFER_BYTES              = 32 * 1024;
const float PI_F = 3.14159265f;

// ==================== Audio ====================
AudioGeneratorMP3*         audioMP3  = nullptr;
AudioGeneratorWAV*         audioWAV  = nullptr;
AudioFileSourceHTTPStream* audioFile = nullptr;
// Buffer the HTTP stream locally so short WiFi jitter does not immediately
// turn into audible interruptions.
AudioFileSourceBuffer*     audioBuff = nullptr;
AudioOutputI2S*            audioOut  = nullptr;

// ==================== State ====================
int    volume       = 80;
bool   audioPaused  = false;
String playState    = "idle";
String currentText  = "";
String currentUrl   = "";
String lastError    = "";
String currentCommand = "";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String topicCmd       = "device/" + String(DEVICE_ID) + "/command";
String topicStatus    = "device/" + String(DEVICE_ID) + "/status";
String topicHeartbeat = "device/" + String(DEVICE_ID) + "/heartbeat";

unsigned long lastHeartbeat = 0;
unsigned long lastReconnect = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastPlaybackLog = 0;
unsigned long lastMqttDebugLog = 0;
unsigned long wifiStableSince = 0;
int lastMqttState = 999;
bool wifiReadyForMqtt = false;
bool wifiDroppedDuringPlayback = false;

void reportStatus();
void connectMQTT();
void logNetworkState(const char* prefix);
bool isAudioRunning();

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] Event: STA connected to AP");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiStableSince = millis();
      wifiReadyForMqtt = false;
      Serial.printf("[WiFi] Event: got IP %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      wifiReadyForMqtt = false;
      if (isAudioRunning()) {
        wifiDroppedDuringPlayback = true;
      }
      Serial.println("[WiFi] Event: lost IP");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiReadyForMqtt = false;
      if (isAudioRunning()) {
        wifiDroppedDuringPlayback = true;
      }
      Serial.printf(
        "[WiFi] Event: disconnected reason=%d\n",
        info.wifi_sta_disconnected.reason
      );
      break;
    default:
      break;
  }
}

bool waitForWiFiReconnect(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiStableSince = millis();
    wifiReadyForMqtt = false;
  }
  return WiFi.status() == WL_CONNECTED;
}

void resetMqttTransport(const char* reason) {
  Serial.printf("[MQTT] Reset transport: %s\n", reason);
  mqtt.disconnect();
  wifiClient.stop();
  delay(50);
}

void recoverConnectivityAfterPlayback(const char* reason) {
  Serial.printf("[MQTT] Recover after playback: %s\n", reason);
  logNetworkState("[MQTT] Playback recovery start");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Playback recovery reconnecting WiFi...");
    WiFi.reconnect();
    if (waitForWiFiReconnect(WIFI_RECOVER_TIMEOUT_MS)) {
      Serial.printf("[WiFi] Playback recovery OK: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WiFi] Playback recovery still offline");
    }
  }

  // Reconnect control plane only after playback is done. Doing a hard reset of
  // the transport during streaming is more likely to break the audio path.
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED && wifiReadyForMqtt) {
    resetMqttTransport(reason);
    connectMQTT();
  }

  logNetworkState("[MQTT] Playback recovery end");
}

void fadeOutAudioOutput() {
  if (!audioOut) {
    return;
  }

  const int steps = 6;
  for (int i = steps; i >= 0; --i) {
    float gain = ((float)volume / 100.0f) * ((float)i / (float)steps);
    audioOut->SetGain(gain);
    delay(8);
  }
}

// ==================== Audio Control ====================
void cleanupAudio(bool gracefulStop) {
  Serial.printf("[Audio] Cleanup start graceful=%s\n", gracefulStop ? "true" : "false");

  if (gracefulStop) {
    fadeOutAudioOutput();
  }

  if (audioMP3) {
    Serial.println("[Audio] Cleanup MP3");
    if (gracefulStop) {
      audioMP3->stop();
    }
    delete audioMP3;
    audioMP3 = nullptr;
  }

  if (audioWAV) {
    Serial.println("[Audio] Cleanup WAV");
    if (gracefulStop) {
      audioWAV->stop();
    }
    delete audioWAV;
    audioWAV = nullptr;
  }

  if (audioOut) {
    Serial.println("[Audio] Cleanup output");
    if (gracefulStop) {
      audioOut->flush();
      delay(20);
      audioOut->stop();
      audioOut->SetGain(0.0f);
      delay(10);
    } else {
      audioOut->SetGain(0.0f);
    }
    delete audioOut;
    audioOut = nullptr;
  }

  if (audioBuff) {
    Serial.println("[Audio] Cleanup buffer");
    delete audioBuff;
    audioBuff = nullptr;
  }

  if (audioFile) {
    Serial.println("[Audio] Cleanup file");
    delete audioFile;
    audioFile = nullptr;
  }

  audioPaused = false;
  playState   = "idle";
  currentUrl  = "";
  currentCommand = "";
  Serial.println("[Audio] Cleanup done");
}

void stopPlayback() {
  cleanupAudio(true);
}

void finishPlayback() {
  cleanupAudio(false);
}

bool isAudioRunning() {
  if (audioMP3 && audioMP3->isRunning()) {
    return true;
  }
  if (audioWAV && audioWAV->isRunning()) {
    return true;
  }
  return false;
}

void logNetworkState(const char* prefix) {
  Serial.printf(
    "%s wifi=%d mqtt_connected=%s mqtt_state=%d heap=%u ip=%s\n",
    prefix,
    WiFi.status(),
    mqtt.connected() ? "true" : "false",
    mqtt.state(),
    ESP.getFreeHeap(),
    WiFi.localIP().toString().c_str()
  );
}

bool playUrl(const char* url) {
  if (!url || strlen(url) == 0) {
    lastError = "empty_url";
    return false;
  }

  String urlStr = String(url);
  urlStr.toLowerCase();

  if (urlStr.startsWith("https://")) {
    Serial.printf("[Audio] Unsupported HTTPS URL: %s\n", url);
    Serial.println("[Audio] Use an http:// MP3/WAV URL, or proxy/convert it on the server.");
    lastError = "unsupported_https";
    return false;
  }

  if (urlStr.endsWith(".m4a") || urlStr.indexOf(".m4a?") >= 0) {
    Serial.printf("[Audio] Unsupported M4A URL: %s\n", url);
    Serial.println("[Audio] This sketch supports MP3 and WAV streams only.");
    lastError = "unsupported_m4a";
    return false;
  }

  stopPlayback();

  audioFile = new AudioFileSourceHTTPStream();
  if (!audioFile->open(url)) {
    Serial.printf("[Audio] Failed to open URL: %s\n", url);
    lastError = "open_url_failed";
    delete audioFile;
    audioFile = nullptr;
    return false;
  }

  audioBuff = new AudioFileSourceBuffer(audioFile, AUDIO_BUFFER_BYTES);
  if (!audioBuff) {
    Serial.println("[Audio] Failed to allocate stream buffer");
    lastError = "buffer_alloc_failed";
    delete audioFile;
    audioFile = nullptr;
    return false;
  }

  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audioOut->SetGain((float)volume / 100.0f);

  bool started = false;
  // Pick decoder from the URL suffix. The server already normalizes the final
  // stream format, so ESP32 only needs MP3/WAV handling here.
  if (urlStr.endsWith(".wav")) {
    audioWAV = new AudioGeneratorWAV();
    started = audioWAV->begin(audioBuff, audioOut);
    Serial.println("[Audio] Decoder: WAV");
  } else {
    audioMP3 = new AudioGeneratorMP3();
    started = audioMP3->begin(audioBuff, audioOut);
    Serial.println("[Audio] Decoder: MP3");
  }

  if (!started) {
    Serial.printf("[Audio] Decoder failed: %s\n", url);
    lastError = "decoder_failed";
    stopPlayback();
    return false;
  }

  audioPaused = false;
  playState   = "playing";
  currentUrl  = url;
  currentCommand = "play";
  lastError   = "";
  wifiDroppedDuringPlayback = false;
  Serial.printf("[Audio] Playing: %s\n", url);
  Serial.printf("[Audio] Stream buffer=%u bytes\n", (unsigned int)AUDIO_BUFFER_BYTES);
  Serial.printf("[Audio] Gain=%.2f Volume=%d\n", (float)volume / 100.0f, volume);
  return true;
}

void setPaused(bool paused) {
  if (!audioOut || playState == "idle") {
    return;
  }

  audioPaused = paused;
  playState   = paused ? "paused" : "playing";
  audioOut->SetGain(paused ? 0.0f : (float)volume / 100.0f);
  Serial.printf("[Audio] %s\n", paused ? "Paused" : "Resumed");
}

void playTestTone(uint16_t freq = 1000, uint16_t durationMs = 1200) {
  stopPlayback();

  AudioOutputI2S testOut;
  testOut.SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  testOut.SetRate(44100);
  testOut.SetChannels(2);
  testOut.SetGain((float)volume / 100.0f);

  if (!testOut.begin()) {
    Serial.println("[Audio] Test tone I2S begin failed");
    lastError = "tone_i2s_begin_failed";
    return;
  }

  const uint32_t sampleRate = 44100;
  const uint32_t totalSamples = (sampleRate * durationMs) / 1000;
  int16_t sample[2];

  Serial.println("[Audio] Playing test tone");
  for (uint32_t i = 0; i < totalSamples; i++) {
    int16_t v = (int16_t)(sin((2.0f * PI_F * freq * i) / sampleRate) * 12000);
    sample[0] = v;
    sample[1] = v;
    while (!testOut.ConsumeSample(sample)) {
      delay(1);
    }
  }

  testOut.flush();
  testOut.stop();
  playState = "idle";
  lastError = "";
  Serial.println("[Audio] Test tone done");
}

// ==================== WiFi ====================
void setupWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] Failed, restarting...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("\n[WiFi] OK");
  Serial.printf("  IP  : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  RSSI: %d dBm\n", WiFi.RSSI());
  Serial.println("  Sleep: disabled");
  wifiStableSince = millis();
  wifiReadyForMqtt = false;
}

void checkWiFi(unsigned long now) {
  // Only allow MQTT reconnect after WiFi has been up for a short moment.
  // This avoids reconnect attempts while the station is still settling.
  if (!wifiReadyForMqtt && WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0) {
    if (now - wifiStableSince >= 1500) {
      wifiReadyForMqtt = true;
      Serial.printf("[WiFi] Stable for MQTT: %s\n", WiFi.localIP().toString().c_str());
    }
  }

  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) {
    return;
  }
  lastWifiCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost, reconnecting...");
    wifiReadyForMqtt = false;
    WiFi.reconnect();
  }
}

// ==================== MQTT ====================
void publishOfflineStatus() {
  JsonDocument doc;
  doc["online"] = false;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topicStatus.c_str(), payload.c_str(), true);
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || !wifiReadyForMqtt || WiFi.localIP()[0] == 0) {
    Serial.printf(
      "[MQTT] Connect skipped wifi=%d ready=%s ip=%s\n",
      WiFi.status(),
      wifiReadyForMqtt ? "true" : "false",
      WiFi.localIP().toString().c_str()
    );
    return;
  }

  Serial.printf("[MQTT] Connecting to %s:%d...\n", MQTT_BROKER, MQTT_PORT);

  JsonDocument willDoc;
  willDoc["online"] = false;
  String willPayload;
  serializeJson(willDoc, willPayload);

  bool ok = mqtt.connect(
    DEVICE_ID,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    topicStatus.c_str(),
    1,
    true,
    willPayload.c_str()
  );

  if (!ok) {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
    lastReconnect = millis();
    return;
  }

  Serial.println("[MQTT] OK");
  mqtt.subscribe(topicCmd.c_str(), 1);
  Serial.printf("  Subscribed: %s\n", topicCmd.c_str());
  reportStatus();
}

void checkMQTT(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiReadyForMqtt = false;
    return;
  }

  if (!wifiReadyForMqtt || WiFi.localIP()[0] == 0) {
    return;
  }

  if (!mqtt.connected()) {
    if (mqtt.state() != lastMqttState) {
      Serial.printf("[MQTT] Disconnected, state=%d\n", mqtt.state());
      if (isAudioRunning()) {
        Serial.println("[MQTT] Warning: MQTT lost during audio playback");
      }
      lastMqttState = mqtt.state();
    }

    // During audio playback we prefer keeping the current HTTP stream alive
    // instead of aggressively resetting sockets just to recover MQTT faster.
    if (isAudioRunning()) {
      if (now - lastMqttDebugLog >= MQTT_DEBUG_INTERVAL_MS) {
        lastMqttDebugLog = now;
        logNetworkState("[MQTT] Playback monitor (mqtt offline, hold reconnect)");
      }
      return;
    }

    if (now - lastReconnect >= MQTT_RECONNECT_INTERVAL_MS) {
      lastReconnect = now;
      Serial.println("[MQTT] Reconnect attempt...");
      logNetworkState("[MQTT] Before reconnect");
      resetMqttTransport("periodic_reconnect");
      connectMQTT();
    }
    return;
  }

  if (lastMqttState != 0) {
    lastMqttState = 0;
    Serial.println("[MQTT] Connected and loop active");
  }

  mqtt.loop();

  if (now - lastMqttDebugLog >= MQTT_DEBUG_INTERVAL_MS) {
    lastMqttDebugLog = now;
    if (isAudioRunning()) {
      logNetworkState("[MQTT] Playback monitor");
    }
  }
}

// ==================== Upload Status ====================
void reportStatus() {
  if (!mqtt.connected()) {
    return;
  }

  JsonDocument doc;
  doc["device_id"]    = DEVICE_ID;
  doc["online"]       = true;
  doc["ip"]           = WiFi.localIP().toString();
  doc["wifi_ssid"]    = WIFI_SSID;
  doc["wifi_rssi"]    = WiFi.RSSI();
  doc["volume"]       = volume;
  doc["play_state"]   = playState;
  doc["progress"]     = 0;
  doc["current_text"] = currentText;
  doc["current_url"]  = currentUrl;
  doc["last_error"]   = lastError;
  doc["uptime_ms"]    = millis();
  doc["free_heap"]    = ESP.getFreeHeap();

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topicStatus.c_str(), payload.c_str(), true);
  Serial.printf("[MQTT] Status -> %s\n", payload.c_str());
}

void sendHeartbeat() {
  if (!mqtt.connected()) {
    return;
  }

  JsonDocument doc;
  doc["wifi_rssi"]  = WiFi.RSSI();
  doc["volume"]     = volume;
  doc["play_state"] = playState;
  doc["free_heap"]  = ESP.getFreeHeap();

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topicHeartbeat.c_str(), payload.c_str());
}

// ==================== Helpers ====================
JsonObject commandParams(JsonDocument& doc) {
  if (doc["params"].is<JsonObject>()) {
    return doc["params"].as<JsonObject>();
  }
  return doc.as<JsonObject>();
}

// ==================== Command Handling ====================
void executeCommand(const char* cmd, JsonObject params) {
  Serial.printf("[Cmd] %s\n", cmd);
  String paramsPayload;
  serializeJson(params, paramsPayload);
  Serial.printf("[Cmd] Params: %s\n", paramsPayload.c_str());

  if (strcmp(cmd, "play") == 0) {
    const char* url  = params["url"]  | "";

    if (strlen(url) > 0) {
      if (playState == "playing" || playState == "paused") {
        Serial.printf("[Cmd] Interrupt current %s and switch to new play URL\n", currentCommand.c_str());
      }
      currentText = "";
      if (playUrl(url)) {
        currentCommand = "play";
      }
    } else if (audioPaused) {
      setPaused(false);
    }
  } else if (strcmp(cmd, "pause") == 0) {
    setPaused(true);
  } else if (strcmp(cmd, "resume") == 0) {
    setPaused(false);
  } else if (strcmp(cmd, "stop") == 0) {
    stopPlayback();
    currentText = "";
    Serial.println("[Cmd] Stopped");
  } else if (strcmp(cmd, "volume") == 0) {
    int val = params["value"] | volume;
    volume = constrain(val, 0, 100);
    if (audioOut && !audioPaused) {
      audioOut->SetGain((float)volume / 100.0f);
    }
    Serial.printf("[Cmd] Volume: %d\n", volume);
  } else if (strcmp(cmd, "tone") == 0) {
    int freq = params["freq"] | 1000;
    int duration = params["duration"] | 1200;
    playTestTone((uint16_t)freq, (uint16_t)duration);
  } else if (strcmp(cmd, "tts") == 0) {
    const char* url = params["url"] | "";
    const char* textPreview = params["text_preview"] | "";

    if (strlen(url) == 0) {
      lastError = "tts_url_missing";
      Serial.println("[Cmd] TTS url missing");
    } else {
      if (playState == "playing" || playState == "paused") {
        Serial.printf("[Cmd] Interrupt current %s and switch to new TTS\n", currentCommand.c_str());
      }
      currentText = textPreview;
      if (playUrl(url)) {
        currentCommand = "tts";
      }
      Serial.printf("[Cmd] TTS url: %s\n", url);
      Serial.printf("[Cmd] TTS preview: %s\n", textPreview);
    }
  } else if (strcmp(cmd, "status") == 0) {
    // Explicit status refresh.
  } else {
    Serial.printf("[Cmd] Unknown: %s\n", cmd);
  }

  reportStatus();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] %s -> %u bytes\n", topic, length);
  Serial.print("[MQTT] Payload: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.write(payload[i]);
  }
  Serial.println();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT] JSON error: %s\n", err.c_str());
    return;
  }

  const char* cmd = doc["cmd"] | "";
  if (strlen(cmd) == 0) {
    Serial.println("[MQTT] Missing cmd");
    return;
  }

  JsonObject params = commandParams(doc);
  executeCommand(cmd, params);
}

// ==================== Arduino ====================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("============================");
  Serial.println("  ESP32 Sound Box");
  Serial.printf("  Device: %s\n", DEVICE_ID);
  Serial.printf("  I2S: DOUT=%d LRC=%d BCLK=%d\n", I2S_DOUT, I2S_LRC, I2S_BCLK);
  Serial.println("============================");

  setupWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(15);
  mqtt.setBufferSize(2048);

  connectMQTT();

  unsigned long now = millis();
  lastHeartbeat = now;
  lastWifiCheck = now;
}

void loop() {
  unsigned long now = millis();

  if (audioMP3 && audioMP3->isRunning()) {
    if (audioBuff) {
      // Feed the stream buffer proactively so the decoder sees a steadier
      // byte stream even when WiFi arrives in small bursts.
      audioBuff->loop();
    }
    if (now - lastPlaybackLog >= PLAYBACK_LOG_INTERVAL_MS) {
      Serial.printf(
        "[Audio] MP3 streaming... heap=%u paused=%s url=%s\n",
        ESP.getFreeHeap(),
        audioPaused ? "true" : "false",
        currentUrl.c_str()
      );
      if (audioBuff) {
        Serial.printf("[Audio] Buffer fill=%u/%u\n", (unsigned int)audioBuff->getFillLevel(), (unsigned int)AUDIO_BUFFER_BYTES);
      }
      lastPlaybackLog = now;
    }
    if (!audioMP3->loop()) {
      if (wifiDroppedDuringPlayback) {
        Serial.println("[Audio] MP3 interrupted by WiFi/network drop");
        lastError = "stream_interrupted";
      } else {
        Serial.println("[Audio] MP3 done");
      }
      finishPlayback();
      if (!mqtt.connected()) {
        Serial.println("[MQTT] Audio finished, MQTT is offline, reconnect will continue in loop");
      } else {
        Serial.println("[MQTT] Audio finished, MQTT still online");
      }
      recoverConnectivityAfterPlayback("mp3_done");
      reportStatus();
      wifiDroppedDuringPlayback = false;
    }
  }

  if (audioWAV && audioWAV->isRunning()) {
    if (audioBuff) {
      audioBuff->loop();
    }
    if (now - lastPlaybackLog >= PLAYBACK_LOG_INTERVAL_MS) {
      Serial.printf(
        "[Audio] WAV streaming... heap=%u paused=%s url=%s\n",
        ESP.getFreeHeap(),
        audioPaused ? "true" : "false",
        currentUrl.c_str()
      );
      if (audioBuff) {
        Serial.printf("[Audio] Buffer fill=%u/%u\n", (unsigned int)audioBuff->getFillLevel(), (unsigned int)AUDIO_BUFFER_BYTES);
      }
      lastPlaybackLog = now;
    }
    if (!audioWAV->loop()) {
      if (wifiDroppedDuringPlayback) {
        Serial.println("[Audio] WAV interrupted by WiFi/network drop");
        lastError = "stream_interrupted";
      } else {
        Serial.println("[Audio] WAV done");
      }
      finishPlayback();
      if (!mqtt.connected()) {
        Serial.println("[MQTT] Audio finished, MQTT is offline, reconnect will continue in loop");
      } else {
        Serial.println("[MQTT] Audio finished, MQTT still online");
      }
      recoverConnectivityAfterPlayback("wav_done");
      reportStatus();
      wifiDroppedDuringPlayback = false;
    }
  }

  checkWiFi(now);
  checkMQTT(now);

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = now;
  }
}
