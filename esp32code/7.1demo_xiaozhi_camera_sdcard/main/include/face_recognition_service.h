#ifndef __FACE_RECOGNITION_SERVICE_H__
#define __FACE_RECOGNITION_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_RECOGNITION_MAX_BOXES   4
#define FACE_RECOGNITION_STATUS_LEN  128
#define FACE_RECOGNITION_ALERT_LEN   64

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
    float score;
    bool primary;
} face_recognition_box_t;

typedef struct {
    bool ready;
    bool face_detected;
    bool recognized;
    uint16_t recognized_slot;
    float similarity;
    int enrolled_count;
    int box_count;
    uint32_t alert_seq;
    char status[FACE_RECOGNITION_STATUS_LEN];
    char alert_text[FACE_RECOGNITION_ALERT_LEN];
    face_recognition_box_t boxes[FACE_RECOGNITION_MAX_BOXES];
} face_recognition_snapshot_t;

esp_err_t face_recognition_service_init(void);
void face_recognition_service_deinit(void);
bool face_recognition_service_is_ready(void);
void face_recognition_service_get_snapshot(face_recognition_snapshot_t *snapshot);
void face_recognition_service_request_enroll(face_recognition_snapshot_t *snapshot, bool *status_changed);
esp_err_t face_recognition_service_clear_all(face_recognition_snapshot_t *snapshot, bool *status_changed);
bool face_recognition_service_process_frame(void *rgb565_data,
                                            uint16_t width,
                                            uint16_t height,
                                            face_recognition_snapshot_t *snapshot,
                                            bool *status_changed);

#ifdef __cplusplus
}
#endif

#endif
