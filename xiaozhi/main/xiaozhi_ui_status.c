#include <string.h>
#include "bsp_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "xiaozhi_ui_status.h"

typedef struct {
    SemaphoreHandle_t mutex;
    xiaozhi_ui_snapshot_t snapshot;
} xiaozhi_ui_status_ctx_t;

static xiaozhi_ui_status_ctx_t g_xiaozhi_ui_status = {0};

static void xiaozhi_ui_status_copy_text(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void xiaozhi_ui_status_lock(void)
{
    if (g_xiaozhi_ui_status.mutex != NULL) {
        xSemaphoreTake(g_xiaozhi_ui_status.mutex, portMAX_DELAY);
    }
}

static void xiaozhi_ui_status_unlock(void)
{
    if (g_xiaozhi_ui_status.mutex != NULL) {
        xSemaphoreGive(g_xiaozhi_ui_status.mutex);
    }
}

void xiaozhi_ui_status_init(void)
{
    if (g_xiaozhi_ui_status.mutex == NULL) {
        g_xiaozhi_ui_status.mutex = xSemaphoreCreateMutex();
    }

    xiaozhi_ui_status_lock();
    memset(&g_xiaozhi_ui_status.snapshot, 0, sizeof(g_xiaozhi_ui_status.snapshot));
    g_xiaozhi_ui_status.snapshot.phase = XIAOZHI_UI_PHASE_BOOT;
    g_xiaozhi_ui_status.snapshot.view = XIAOZHI_UI_VIEW_SETUP;
    g_xiaozhi_ui_status.snapshot.playback_rate = BSP_AUDIO_SAMPLE_RATE;
    g_xiaozhi_ui_status.snapshot.frame_duration_ms = BSP_AUDIO_FRAME_DURATION_MS;
    g_xiaozhi_ui_status.snapshot.ws_version = 1;
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.note,
                                sizeof(g_xiaozhi_ui_status.snapshot.note),
                                "正在启动小智语音助手");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.stt_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.stt_text),
                                "按住按键开始说话");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.light_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.light_text),
                                "已关闭");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.llm_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.llm_text),
                                "识别后的内容会显示在这里");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.tts_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.tts_text),
                                "小智的语音回复会显示在这里");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.wifi_ssid,
                                sizeof(g_xiaozhi_ui_status.snapshot.wifi_ssid),
                                "未连接");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.wifi_ip,
                                sizeof(g_xiaozhi_ui_status.snapshot.wifi_ip),
                                "--");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.binding_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.binding_text),
                                "查询中");
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_phase(xiaozhi_ui_phase_t phase)
{
    xiaozhi_ui_status_lock();
    g_xiaozhi_ui_status.snapshot.phase = phase;
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_view(xiaozhi_ui_view_t view)
{
    xiaozhi_ui_status_lock();
    g_xiaozhi_ui_status.snapshot.view = view;
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_connect_state(bool wifi_connected, bool ws_ready)
{
    xiaozhi_ui_status_lock();
    g_xiaozhi_ui_status.snapshot.wifi_connected = wifi_connected;
    g_xiaozhi_ui_status.snapshot.ws_ready = ws_ready;
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_audio_info(uint32_t playback_rate, uint32_t frame_duration_ms, int ws_version)
{
    xiaozhi_ui_status_lock();
    g_xiaozhi_ui_status.snapshot.playback_rate = playback_rate;
    g_xiaozhi_ui_status.snapshot.frame_duration_ms = frame_duration_ms;
    g_xiaozhi_ui_status.snapshot.ws_version = ws_version;
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_note(const char *note)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.note,
                                sizeof(g_xiaozhi_ui_status.snapshot.note),
                                note);
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_stt_text(const char *text)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.stt_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.stt_text),
                                text);
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_light_text(const char *text)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.light_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.light_text),
                                text);
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_llm_text(const char *text)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.llm_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.llm_text),
                                text);
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_tts_text(const char *text)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.tts_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.tts_text),
                                text);
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_network_info(const char *ssid, const char *ip)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.wifi_ssid,
                                sizeof(g_xiaozhi_ui_status.snapshot.wifi_ssid),
                                ((ssid != NULL) && (ssid[0] != '\0')) ? ssid : "未连接");
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.wifi_ip,
                                sizeof(g_xiaozhi_ui_status.snapshot.wifi_ip),
                                ((ip != NULL) && (ip[0] != '\0')) ? ip : "--");
    xiaozhi_ui_status_unlock();
}

void xiaozhi_ui_status_set_binding_text(const char *text)
{
    xiaozhi_ui_status_lock();
    xiaozhi_ui_status_copy_text(g_xiaozhi_ui_status.snapshot.binding_text,
                                sizeof(g_xiaozhi_ui_status.snapshot.binding_text),
                                ((text != NULL) && (text[0] != '\0')) ? text : "已绑定");
    xiaozhi_ui_status_unlock();
}

xiaozhi_ui_view_t xiaozhi_ui_status_get_view(void)
{
    xiaozhi_ui_view_t view = XIAOZHI_UI_VIEW_SETUP;

    xiaozhi_ui_status_lock();
    view = g_xiaozhi_ui_status.snapshot.view;
    xiaozhi_ui_status_unlock();

    return view;
}

void xiaozhi_ui_status_get_snapshot(xiaozhi_ui_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    xiaozhi_ui_status_lock();
    memcpy(snapshot, &g_xiaozhi_ui_status.snapshot, sizeof(*snapshot));
    xiaozhi_ui_status_unlock();
}
