#ifndef __XIAOZHI_UI_STATUS_H__
#define __XIAOZHI_UI_STATUS_H__

#include <stdbool.h>
#include <stdint.h>

#define XIAOZHI_UI_TEXT_LEN  256
#define XIAOZHI_UI_NOTE_LEN  128
#define XIAOZHI_UI_WIFI_LEN  64
#define XIAOZHI_UI_IP_LEN    32
#define XIAOZHI_UI_BIND_LEN  64

typedef enum {
    XIAOZHI_UI_PHASE_BOOT = 0,
    XIAOZHI_UI_PHASE_CONNECTING,
    XIAOZHI_UI_PHASE_READY,
    XIAOZHI_UI_PHASE_LISTENING,
    XIAOZHI_UI_PHASE_THINKING,
    XIAOZHI_UI_PHASE_SPEAKING,
    XIAOZHI_UI_PHASE_DISCONNECTED,
    XIAOZHI_UI_PHASE_ERROR,
} xiaozhi_ui_phase_t;

typedef enum {
    XIAOZHI_UI_VIEW_SETUP = 0,
    XIAOZHI_UI_VIEW_VOICE,
    XIAOZHI_UI_VIEW_CAMERA,
} xiaozhi_ui_view_t;

typedef struct {
    xiaozhi_ui_phase_t phase;
    xiaozhi_ui_view_t view;
    bool wifi_connected;
    bool ws_ready;
    uint32_t playback_rate;
    uint32_t frame_duration_ms;
    int ws_version;
    char note[XIAOZHI_UI_NOTE_LEN];
    char stt_text[XIAOZHI_UI_TEXT_LEN];
    char light_text[XIAOZHI_UI_TEXT_LEN];
    char llm_text[XIAOZHI_UI_TEXT_LEN];
    char tts_text[XIAOZHI_UI_TEXT_LEN];
    char wifi_ssid[XIAOZHI_UI_WIFI_LEN];
    char wifi_ip[XIAOZHI_UI_IP_LEN];
    char binding_text[XIAOZHI_UI_BIND_LEN];
} xiaozhi_ui_snapshot_t;

void xiaozhi_ui_status_init(void);
void xiaozhi_ui_status_set_phase(xiaozhi_ui_phase_t phase);
void xiaozhi_ui_status_set_view(xiaozhi_ui_view_t view);
void xiaozhi_ui_status_set_connect_state(bool wifi_connected, bool ws_ready);
void xiaozhi_ui_status_set_audio_info(uint32_t playback_rate, uint32_t frame_duration_ms, int ws_version);
void xiaozhi_ui_status_set_note(const char *note);
void xiaozhi_ui_status_set_stt_text(const char *text);
void xiaozhi_ui_status_set_light_text(const char *text);
void xiaozhi_ui_status_set_llm_text(const char *text);
void xiaozhi_ui_status_set_tts_text(const char *text);
void xiaozhi_ui_status_set_network_info(const char *ssid, const char *ip);
void xiaozhi_ui_status_set_binding_text(const char *text);
xiaozhi_ui_view_t xiaozhi_ui_status_get_view(void);
void xiaozhi_ui_status_get_snapshot(xiaozhi_ui_snapshot_t *snapshot);

#endif
