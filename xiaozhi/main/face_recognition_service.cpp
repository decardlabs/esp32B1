#include "face_recognition_service.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <list>
#include <new>
#include <string>
#include <vector>

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

namespace {

static constexpr const char *TAG = "FACE_RECOG";
static constexpr const char *FACE_SPIFFS_PARTITION = "storage";
static constexpr const char *FACE_SPIFFS_BASE_PATH = "/spiffs";
static constexpr const char *FACE_DB_PATH = "/spiffs/face.db";
static constexpr const char *FACE_LABEL_PATH = "/spiffs/face_labels.txt";
static constexpr int FACE_INFER_INTERVAL_MS = 420;
static constexpr int FACE_DETECT_WIDTH = 160;
static constexpr int FACE_DETECT_HEIGHT = 120;
static constexpr int FACE_ALERT_STABLE_HITS = 1;
static constexpr int FACE_ALERT_COOLDOWN_MS = 4000;
static constexpr float FACE_RECOGNITION_THRESHOLD = 0.42f;
static constexpr int FACE_RECOGNIZE_INTERVAL_MS = 600;
static constexpr int FACE_RECOGNITION_CACHE_MS = 1800;
static constexpr int FACE_ENROLL_TIMEOUT_MS = 5000;
static constexpr int FACE_ENROLL_MAX_ATTEMPTS = 10;
static constexpr int FACE_ENROLL_RETRY_INTERVAL_MS = 260;
static constexpr int FACE_TRACE_LOG_INTERVAL_MS = 3000;

enum class FaceInputVariant : uint8_t {
    ORIGINAL = 0,
    BYTE_SWAP,
    RB_SWAP,
    BYTE_RB_SWAP,
    COUNT,
};

struct FaceServiceCtx {
    bool mounted = false;
    bool ready = false;
    bool enroll_pending = false;
    int64_t last_infer_us = 0;
    uint32_t infer_count = 0;
    uint32_t no_face_streak = 0;
    HumanFaceDetect *detector = nullptr;
    HumanFaceRecognizer *recognizer = nullptr;
    FaceInputVariant active_variant = FaceInputVariant::ORIGINAL;
    bool variant_locked = false;
    uint16_t *detect_buf = nullptr;
    size_t detect_buf_pixels = 0;
    uint16_t *full_variant_buf = nullptr;
    size_t full_variant_buf_pixels = 0;
    uint16_t stable_slot = 0;
    uint8_t stable_hits = 0;
    uint16_t last_alert_slot = 0;
    int64_t last_alert_us = 0;
    int64_t last_recognize_us = 0;
    uint16_t last_match_slot = 0;
    float last_match_similarity = 0.0f;
    bool last_match_valid = false;
    int64_t last_detect_log_us = 0;
    int64_t last_no_face_log_us = 0;
    int64_t enroll_deadline_us = 0;
    uint8_t enroll_attempts_left = 0;
    int64_t last_enroll_attempt_us = 0;
    face_recognition_snapshot_t snapshot = {};
    std::vector<std::string> labels;
};

static FaceServiceCtx g_face_ctx;
static char g_face_db_path[] = "/spiffs/face.db";

static void face_recognition_copy_snapshot(face_recognition_snapshot_t *snapshot)
{
    if (snapshot != nullptr) {
        *snapshot = g_face_ctx.snapshot;
    }
}

static const char *face_recognition_variant_name(FaceInputVariant variant)
{
    switch (variant) {
        case FaceInputVariant::ORIGINAL:
            return "original";
        case FaceInputVariant::BYTE_SWAP:
            return "byte_swap";
        case FaceInputVariant::RB_SWAP:
            return "rb_swap";
        case FaceInputVariant::BYTE_RB_SWAP:
            return "byte_rb_swap";
        default:
            return "unknown";
    }
}

static inline uint16_t face_recognition_byte_swap(uint16_t pixel)
{
    return (uint16_t)((pixel << 8) | (pixel >> 8));
}

static inline uint16_t face_recognition_rb_swap(uint16_t pixel)
{
    uint16_t red = (pixel >> 11) & 0x1F;
    uint16_t green = (pixel >> 5) & 0x3F;
    uint16_t blue = pixel & 0x1F;

    return (uint16_t)((blue << 11) | (green << 5) | red);
}

static uint16_t face_recognition_transform_pixel(uint16_t pixel, FaceInputVariant variant)
{
    switch (variant) {
        case FaceInputVariant::ORIGINAL:
            return pixel;
        case FaceInputVariant::BYTE_SWAP:
            return face_recognition_byte_swap(pixel);
        case FaceInputVariant::RB_SWAP:
            return face_recognition_rb_swap(pixel);
        case FaceInputVariant::BYTE_RB_SWAP:
            return face_recognition_rb_swap(face_recognition_byte_swap(pixel));
        default:
            return pixel;
    }
}

static void face_recognition_clear_alert(void)
{
    g_face_ctx.snapshot.alert_text[0] = '\0';
}

static void face_recognition_emit_alert(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_face_ctx.snapshot.alert_text, sizeof(g_face_ctx.snapshot.alert_text), fmt, args);
    va_end(args);
    g_face_ctx.snapshot.alert_seq++;
}

static bool face_recognition_prepare_detect_image(const void *rgb565_data,
                                                  uint16_t src_width,
                                                  uint16_t src_height,
                                                  FaceInputVariant variant,
                                                  dl::image::img_t *img)
{
    const uint16_t *src = static_cast<const uint16_t *>(rgb565_data);
    uint16_t dst_width = (src_width > FACE_DETECT_WIDTH) ? FACE_DETECT_WIDTH : src_width;
    uint16_t dst_height = (src_height > FACE_DETECT_HEIGHT) ? FACE_DETECT_HEIGHT : src_height;
    size_t pixel_count = (size_t)dst_width * dst_height;

    if ((src == nullptr) || (img == nullptr) || (dst_width == 0) || (dst_height == 0)) {
        return false;
    }

    if ((g_face_ctx.detect_buf == nullptr) || (g_face_ctx.detect_buf_pixels < pixel_count)) {
        heap_caps_free(g_face_ctx.detect_buf);
        g_face_ctx.detect_buf = static_cast<uint16_t *>(
            heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_face_ctx.detect_buf == nullptr) {
            g_face_ctx.detect_buf = static_cast<uint16_t *>(
                heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_8BIT));
        }
        g_face_ctx.detect_buf_pixels = (g_face_ctx.detect_buf != nullptr) ? pixel_count : 0;
    }

    if (g_face_ctx.detect_buf == nullptr) {
        ESP_LOGE(TAG,
                 "detect buffer alloc failed, pixels=%u, internal_free=%u, largest=%u",
                 (unsigned)pixel_count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }

    for (uint16_t y = 0; y < dst_height; ++y) {
        uint32_t src_y = ((uint32_t)y * src_height) / dst_height;

        for (uint16_t x = 0; x < dst_width; ++x) {
            uint32_t src_x = ((uint32_t)x * src_width) / dst_width;
            uint16_t pixel = src[src_y * src_width + src_x];
            g_face_ctx.detect_buf[(size_t)y * dst_width + x] = face_recognition_transform_pixel(pixel, variant);
        }
    }

    img->data = g_face_ctx.detect_buf;
    img->width = dst_width;
    img->height = dst_height;
    img->pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;
    return true;
}

static std::list<dl::detect::result_t> face_recognition_scale_results(const std::list<dl::detect::result_t> &src_results,
                                                                      uint16_t src_width,
                                                                      uint16_t src_height,
                                                                      uint16_t dst_width,
                                                                      uint16_t dst_height)
{
    std::list<dl::detect::result_t> scaled_results;

    for (const dl::detect::result_t &item : src_results) {
        dl::detect::result_t scaled = item;

        if (scaled.box.size() >= 4) {
            scaled.box[0] = (item.box[0] * (int)dst_width) / (int)src_width;
            scaled.box[1] = (item.box[1] * (int)dst_height) / (int)src_height;
            scaled.box[2] = (item.box[2] * (int)dst_width) / (int)src_width;
            scaled.box[3] = (item.box[3] * (int)dst_height) / (int)src_height;
            scaled.limit_box(dst_width, dst_height);
        }

        for (size_t i = 0; i < scaled.keypoint.size(); ++i) {
            if ((i % 2U) == 0U) {
                scaled.keypoint[i] = (item.keypoint[i] * (int)dst_width) / (int)src_width;
            } else {
                scaled.keypoint[i] = (item.keypoint[i] * (int)dst_height) / (int)src_height;
            }
        }
        scaled.limit_keypoint(dst_width, dst_height);
        scaled_results.push_back(scaled);
    }

    return scaled_results;
}

static bool face_recognition_prepare_full_image(const void *rgb565_data,
                                                uint16_t width,
                                                uint16_t height,
                                                FaceInputVariant variant,
                                                dl::image::img_t *img)
{
    const uint16_t *src = static_cast<const uint16_t *>(rgb565_data);
    size_t pixel_count = (size_t)width * height;

    if ((src == nullptr) || (img == nullptr) || (width == 0) || (height == 0)) {
        return false;
    }

    img->width = width;
    img->height = height;
    img->pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

    if (variant == FaceInputVariant::ORIGINAL) {
        img->data = const_cast<void *>(rgb565_data);
        return true;
    }

    if ((g_face_ctx.full_variant_buf == nullptr) || (g_face_ctx.full_variant_buf_pixels < pixel_count)) {
        heap_caps_free(g_face_ctx.full_variant_buf);
        g_face_ctx.full_variant_buf = static_cast<uint16_t *>(
            heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_face_ctx.full_variant_buf == nullptr) {
            g_face_ctx.full_variant_buf = static_cast<uint16_t *>(
                heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_8BIT));
        }
        g_face_ctx.full_variant_buf_pixels = (g_face_ctx.full_variant_buf != nullptr) ? pixel_count : 0;
    }

    if (g_face_ctx.full_variant_buf == nullptr) {
        ESP_LOGE(TAG,
                 "full image buffer alloc failed, pixels=%u, internal_free=%u, largest=%u",
                 (unsigned)pixel_count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        g_face_ctx.full_variant_buf[i] = face_recognition_transform_pixel(src[i], variant);
    }

    img->data = g_face_ctx.full_variant_buf;
    return true;
}

static void face_recognition_set_status(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_face_ctx.snapshot.status, sizeof(g_face_ctx.snapshot.status), fmt, args);
    va_end(args);
}

static bool face_recognition_snapshot_changed(const face_recognition_snapshot_t &before)
{
    if (before.ready != g_face_ctx.snapshot.ready ||
        before.face_detected != g_face_ctx.snapshot.face_detected ||
        before.recognized != g_face_ctx.snapshot.recognized ||
        before.recognized_slot != g_face_ctx.snapshot.recognized_slot ||
        before.enrolled_count != g_face_ctx.snapshot.enrolled_count ||
        before.box_count != g_face_ctx.snapshot.box_count ||
        before.alert_seq != g_face_ctx.snapshot.alert_seq) {
        return true;
    }

    if ((strcmp(before.status, g_face_ctx.snapshot.status) != 0) ||
        (strcmp(before.alert_text, g_face_ctx.snapshot.alert_text) != 0)) {
        return true;
    }

    return false;
}

static std::string face_recognition_make_label(size_t index)
{
    return std::string("成员") + std::to_string(index + 1);
}

static esp_err_t face_recognition_save_labels(void)
{
    FILE *fp = fopen(FACE_LABEL_PATH, "w");

    if (fp == nullptr) {
        ESP_LOGE(TAG, "open label file for write failed");
        return ESP_FAIL;
    }

    for (const std::string &label : g_face_ctx.labels) {
        fprintf(fp, "%s\n", label.c_str());
    }

    fclose(fp);
    return ESP_OK;
}

static void face_recognition_load_labels(void)
{
    FILE *fp = fopen(FACE_LABEL_PATH, "r");
    char line[64];

    g_face_ctx.labels.clear();
    if (fp == nullptr) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != nullptr) {
        size_t len = strlen(line);

        while ((len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r'))) {
            line[--len] = '\0';
        }

        if (line[0] != '\0') {
            g_face_ctx.labels.emplace_back(line);
        }
    }

    fclose(fp);
}

static void face_recognition_sync_labels(void)
{
    int count = 0;

    if (g_face_ctx.recognizer != nullptr) {
        count = g_face_ctx.recognizer->get_num_feats();
    }

    if (count < 0) {
        count = 0;
    }

    if ((int)g_face_ctx.labels.size() < count) {
        while ((int)g_face_ctx.labels.size() < count) {
            g_face_ctx.labels.emplace_back(face_recognition_make_label(g_face_ctx.labels.size()));
        }
        face_recognition_save_labels();
    } else if ((int)g_face_ctx.labels.size() > count) {
        g_face_ctx.labels.resize((size_t)count);
        face_recognition_save_labels();
    }
}

static const char *face_recognition_get_label(uint16_t slot)
{
    static char fallback_label[32];
    size_t index = (slot > 0) ? (size_t)(slot - 1) : 0;

    if ((slot > 0) && (index < g_face_ctx.labels.size())) {
        return g_face_ctx.labels[index].c_str();
    }

    snprintf(fallback_label, sizeof(fallback_label), "成员%u", (unsigned)slot);
    return fallback_label;
}

static void face_recognition_fill_boxes(const std::list<dl::detect::result_t> &detect_res)
{
    int box_index = 0;
    int max_area = -1;
    int primary_index = -1;

    g_face_ctx.snapshot.box_count = 0;
    memset(g_face_ctx.snapshot.boxes, 0, sizeof(g_face_ctx.snapshot.boxes));

    for (const dl::detect::result_t &item : detect_res) {
        if ((box_index >= FACE_RECOGNITION_MAX_BOXES) || (item.box.size() < 4)) {
            break;
        }

        g_face_ctx.snapshot.boxes[box_index].left = item.box[0];
        g_face_ctx.snapshot.boxes[box_index].top = item.box[1];
        g_face_ctx.snapshot.boxes[box_index].right = item.box[2];
        g_face_ctx.snapshot.boxes[box_index].bottom = item.box[3];
        g_face_ctx.snapshot.boxes[box_index].score = item.score;
        g_face_ctx.snapshot.boxes[box_index].primary = false;

        if (item.box_area() > max_area) {
            max_area = item.box_area();
            primary_index = box_index;
        }

        box_index++;
    }

    g_face_ctx.snapshot.box_count = box_index;
    if ((primary_index >= 0) && (primary_index < box_index)) {
        g_face_ctx.snapshot.boxes[primary_index].primary = true;
    }
}

static esp_err_t face_recognition_mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = FACE_SPIFFS_BASE_PATH,
        .partition_label = FACE_SPIFFS_PARTITION,
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    size_t total = 0;
    size_t used = 0;
    esp_err_t ret;

    if (g_face_ctx.mounted) {
        return ESP_OK;
    }

    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    g_face_ctx.mounted = true;
    ret = esp_spiffs_info(FACE_SPIFFS_PARTITION, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "spiffs mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }

    return ESP_OK;
}

static void face_recognition_reset_snapshot(void)
{
    memset(&g_face_ctx.snapshot, 0, sizeof(g_face_ctx.snapshot));
}

} // namespace

extern "C" esp_err_t face_recognition_service_init(void)
{
    esp_err_t ret;

    if (g_face_ctx.ready) {
        return ESP_OK;
    }

    face_recognition_reset_snapshot();
    g_face_ctx.snapshot.ready = false;
    face_recognition_set_status("人脸识别初始化中");

    ret = face_recognition_mount_storage();
    if (ret != ESP_OK) {
        face_recognition_set_status("存储初始化失败，无法使用人脸识别");
        return ret;
    }

    g_face_ctx.detector = new (std::nothrow) HumanFaceDetect();
    g_face_ctx.recognizer = new (std::nothrow) HumanFaceRecognizer(g_face_db_path,
                                                                   HumanFaceFeat::model_type_t::MFN_S8_V1,
                                                                   FACE_RECOGNITION_THRESHOLD,
                                                                   1);

    if ((g_face_ctx.detector == nullptr) || (g_face_ctx.recognizer == nullptr)) {
        delete g_face_ctx.detector;
        delete g_face_ctx.recognizer;
        g_face_ctx.detector = nullptr;
        g_face_ctx.recognizer = nullptr;
        face_recognition_set_status("人脸模型加载失败");
        return ESP_ERR_NO_MEM;
    }

    g_face_ctx.ready = true;
    g_face_ctx.snapshot.ready = true;
    g_face_ctx.last_infer_us = 0;
    g_face_ctx.infer_count = 0;
    g_face_ctx.no_face_streak = 0;
    g_face_ctx.enroll_pending = false;
    g_face_ctx.active_variant = FaceInputVariant::ORIGINAL;
    g_face_ctx.variant_locked = false;
    g_face_ctx.stable_slot = 0;
    g_face_ctx.stable_hits = 0;
    g_face_ctx.last_alert_slot = 0;
    g_face_ctx.last_alert_us = 0;
    g_face_ctx.last_recognize_us = 0;
    g_face_ctx.last_match_slot = 0;
    g_face_ctx.last_match_similarity = 0.0f;
    g_face_ctx.last_match_valid = false;
    g_face_ctx.last_detect_log_us = 0;
    g_face_ctx.last_no_face_log_us = 0;
    g_face_ctx.enroll_deadline_us = 0;
    g_face_ctx.enroll_attempts_left = 0;
    g_face_ctx.last_enroll_attempt_us = 0;
    face_recognition_clear_alert();

    face_recognition_load_labels();
    face_recognition_sync_labels();
    g_face_ctx.snapshot.enrolled_count = g_face_ctx.recognizer->get_num_feats();

    if (g_face_ctx.snapshot.enrolled_count > 0) {
        face_recognition_set_status("人脸识别已就绪，已录入 %d 人", g_face_ctx.snapshot.enrolled_count);
    } else {
        face_recognition_set_status("人脸识别已就绪，按 KEY1 录入当前人脸");
    }

    ESP_LOGI(TAG, "face recognition ready, enrolled=%d", g_face_ctx.snapshot.enrolled_count);
    return ESP_OK;
}

extern "C" void face_recognition_service_deinit(void)
{
    delete g_face_ctx.detector;
    delete g_face_ctx.recognizer;
    g_face_ctx.detector = nullptr;
    g_face_ctx.recognizer = nullptr;
    g_face_ctx.ready = false;
    g_face_ctx.enroll_pending = false;
    g_face_ctx.last_infer_us = 0;
    g_face_ctx.active_variant = FaceInputVariant::ORIGINAL;
    g_face_ctx.variant_locked = false;
    g_face_ctx.stable_slot = 0;
    g_face_ctx.stable_hits = 0;
    g_face_ctx.last_alert_slot = 0;
    g_face_ctx.last_alert_us = 0;
    g_face_ctx.last_recognize_us = 0;
    g_face_ctx.last_match_slot = 0;
    g_face_ctx.last_match_similarity = 0.0f;
    g_face_ctx.last_match_valid = false;
    g_face_ctx.last_detect_log_us = 0;
    g_face_ctx.last_no_face_log_us = 0;
    g_face_ctx.enroll_deadline_us = 0;
    g_face_ctx.enroll_attempts_left = 0;
    g_face_ctx.last_enroll_attempt_us = 0;
    heap_caps_free(g_face_ctx.detect_buf);
    g_face_ctx.detect_buf = nullptr;
    g_face_ctx.detect_buf_pixels = 0;
    heap_caps_free(g_face_ctx.full_variant_buf);
    g_face_ctx.full_variant_buf = nullptr;
    g_face_ctx.full_variant_buf_pixels = 0;
    g_face_ctx.labels.clear();

    if (g_face_ctx.mounted) {
        esp_vfs_spiffs_unregister(FACE_SPIFFS_PARTITION);
        g_face_ctx.mounted = false;
    }

    face_recognition_reset_snapshot();
    face_recognition_set_status("人脸识别未启动");
}

extern "C" bool face_recognition_service_is_ready(void)
{
    return g_face_ctx.ready;
}

extern "C" void face_recognition_service_get_snapshot(face_recognition_snapshot_t *snapshot)
{
    face_recognition_copy_snapshot(snapshot);
}

extern "C" void face_recognition_service_request_enroll(face_recognition_snapshot_t *snapshot, bool *status_changed)
{
    face_recognition_snapshot_t before = g_face_ctx.snapshot;

    if (!g_face_ctx.ready) {
        if (status_changed != nullptr) {
            *status_changed = false;
        }
        face_recognition_copy_snapshot(snapshot);
        return;
    }

    g_face_ctx.enroll_pending = true;
    g_face_ctx.enroll_deadline_us = esp_timer_get_time() + (FACE_ENROLL_TIMEOUT_MS * 1000LL);
    g_face_ctx.enroll_attempts_left = FACE_ENROLL_MAX_ATTEMPTS;
    g_face_ctx.last_enroll_attempt_us = 0;
    face_recognition_set_status("录入中，请保持正对镜头");

    if (status_changed != nullptr) {
        *status_changed = face_recognition_snapshot_changed(before);
    }
    face_recognition_copy_snapshot(snapshot);
}

extern "C" esp_err_t face_recognition_service_clear_all(face_recognition_snapshot_t *snapshot, bool *status_changed)
{
    face_recognition_snapshot_t before = g_face_ctx.snapshot;
    esp_err_t ret;

    if (!g_face_ctx.ready || (g_face_ctx.recognizer == nullptr)) {
        if (status_changed != nullptr) {
            *status_changed = false;
        }
        face_recognition_copy_snapshot(snapshot);
        return ESP_ERR_INVALID_STATE;
    }

    ret = g_face_ctx.recognizer->clear_all_feats();
    if (ret == ESP_OK) {
        g_face_ctx.labels.clear();
        face_recognition_save_labels();
        g_face_ctx.snapshot.face_detected = false;
        g_face_ctx.snapshot.recognized = false;
        g_face_ctx.snapshot.recognized_slot = 0;
        g_face_ctx.snapshot.similarity = 0.0f;
        g_face_ctx.snapshot.enrolled_count = 0;
        g_face_ctx.snapshot.box_count = 0;
        g_face_ctx.stable_slot = 0;
        g_face_ctx.stable_hits = 0;
        g_face_ctx.last_alert_slot = 0;
        g_face_ctx.last_alert_us = 0;
        g_face_ctx.last_recognize_us = 0;
        g_face_ctx.last_match_slot = 0;
        g_face_ctx.last_match_similarity = 0.0f;
        g_face_ctx.last_match_valid = false;
        g_face_ctx.last_detect_log_us = 0;
        g_face_ctx.last_no_face_log_us = 0;
        g_face_ctx.enroll_deadline_us = 0;
        g_face_ctx.enroll_attempts_left = 0;
        g_face_ctx.last_enroll_attempt_us = 0;
        face_recognition_clear_alert();
        face_recognition_set_status("人脸库已清空，按 KEY1 可重新录入");
    } else {
        face_recognition_set_status("清空人脸库失败");
    }

    if (status_changed != nullptr) {
        *status_changed = face_recognition_snapshot_changed(before);
    }
    face_recognition_copy_snapshot(snapshot);
    return ret;
}

extern "C" bool face_recognition_service_process_frame(void *rgb565_data,
                                                        uint16_t width,
                                                        uint16_t height,
                                                        face_recognition_snapshot_t *snapshot,
                                                        bool *status_changed)
{
    face_recognition_snapshot_t before = g_face_ctx.snapshot;
    int64_t now_us = esp_timer_get_time();
    dl::image::img_t detect_img;
    dl::image::img_t full_img;
    std::list<dl::detect::result_t> scaled_detect_res;
    std::list<dl::detect::result_t> full_detect_res;
    const std::list<dl::detect::result_t> *detect_res = nullptr;
    FaceInputVariant detect_variant = g_face_ctx.active_variant;
    bool enroll_attempt_due = false;
    bool need_feature_pass = false;

    if (status_changed != nullptr) {
        *status_changed = false;
    }

    if (!g_face_ctx.ready || (g_face_ctx.detector == nullptr) || (g_face_ctx.recognizer == nullptr) ||
        (rgb565_data == nullptr) || (width == 0) || (height == 0)) {
        face_recognition_copy_snapshot(snapshot);
        return false;
    }

    if (!g_face_ctx.enroll_pending &&
        (g_face_ctx.last_infer_us != 0) &&
        ((now_us - g_face_ctx.last_infer_us) < (FACE_INFER_INTERVAL_MS * 1000LL))) {
        face_recognition_copy_snapshot(snapshot);
        return true;
    }

    face_recognition_clear_alert();
    if (g_face_ctx.enroll_pending && (g_face_ctx.enroll_deadline_us != 0) && (now_us > g_face_ctx.enroll_deadline_us)) {
        g_face_ctx.enroll_pending = false;
        g_face_ctx.enroll_deadline_us = 0;
        g_face_ctx.enroll_attempts_left = 0;
        g_face_ctx.last_enroll_attempt_us = 0;
        face_recognition_set_status("录入超时，请保持正脸后重试");
    }

    if (!face_recognition_prepare_detect_image(rgb565_data, width, height, detect_variant, &detect_img)) {
        face_recognition_set_status("人脸识别缓冲分配失败");
        face_recognition_copy_snapshot(snapshot);
        return false;
    }
    detect_res = &g_face_ctx.detector->run(detect_img);

    if (detect_res->empty() && !g_face_ctx.variant_locked) {
        for (uint8_t variant_index = 1; variant_index < (uint8_t)FaceInputVariant::COUNT; ++variant_index) {
            FaceInputVariant candidate = static_cast<FaceInputVariant>(variant_index);

            if (!face_recognition_prepare_detect_image(rgb565_data, width, height, candidate, &detect_img)) {
                break;
            }

            detect_res = &g_face_ctx.detector->run(detect_img);
            if (!detect_res->empty()) {
                detect_variant = candidate;
                break;
            }
        }
    }

    g_face_ctx.infer_count++;
    g_face_ctx.last_infer_us = now_us;
    if (!detect_res->empty() && (!g_face_ctx.variant_locked || (detect_variant != g_face_ctx.active_variant))) {
        g_face_ctx.active_variant = detect_variant;
        g_face_ctx.variant_locked = true;
        ESP_LOGI(TAG, "face input calibrated: variant=%s", face_recognition_variant_name(detect_variant));
    }
    scaled_detect_res = face_recognition_scale_results(*detect_res, detect_img.width, detect_img.height, width, height);
    g_face_ctx.snapshot.face_detected = !scaled_detect_res.empty();
    g_face_ctx.snapshot.recognized = false;
    g_face_ctx.snapshot.recognized_slot = 0;
    g_face_ctx.snapshot.similarity = 0.0f;
    g_face_ctx.snapshot.enrolled_count = g_face_ctx.recognizer->get_num_feats();
    face_recognition_fill_boxes(scaled_detect_res);
    enroll_attempt_due = g_face_ctx.enroll_pending &&
                         ((g_face_ctx.last_enroll_attempt_us == 0) ||
                          ((now_us - g_face_ctx.last_enroll_attempt_us) >= (FACE_ENROLL_RETRY_INTERVAL_MS * 1000LL)));
    need_feature_pass = !detect_res->empty() &&
                        (enroll_attempt_due ||
                         ((g_face_ctx.snapshot.enrolled_count > 0) &&
                          ((g_face_ctx.last_recognize_us == 0) ||
                           ((now_us - g_face_ctx.last_recognize_us) >= (FACE_RECOGNIZE_INTERVAL_MS * 1000LL)))));

    if (need_feature_pass &&
        !face_recognition_prepare_full_image(rgb565_data, width, height, detect_variant, &full_img)) {
        face_recognition_set_status("人脸识别特征缓冲分配失败");
        face_recognition_copy_snapshot(snapshot);
        return false;
    }

    if (detect_res->empty()) {
        g_face_ctx.no_face_streak++;
        g_face_ctx.stable_slot = 0;
        g_face_ctx.stable_hits = 0;
        g_face_ctx.last_match_valid = false;
        g_face_ctx.last_match_slot = 0;
        g_face_ctx.last_match_similarity = 0.0f;
        if (g_face_ctx.enroll_pending) {
            face_recognition_set_status("录入中，请将人脸保持在画面中央");
        }
        if ((g_face_ctx.no_face_streak == 1) ||
            ((g_face_ctx.last_no_face_log_us == 0) ||
             ((now_us - g_face_ctx.last_no_face_log_us) >= (FACE_TRACE_LOG_INTERVAL_MS * 1000LL)))) {
            g_face_ctx.last_no_face_log_us = now_us;
            ESP_LOGW(TAG,
                     "no face detected, infer=%u, streak=%u, variant=%s, size=%ux%u, locked=%d, internal_free=%u, largest=%u",
                     (unsigned)g_face_ctx.infer_count,
                     (unsigned)g_face_ctx.no_face_streak,
                     face_recognition_variant_name(detect_variant),
                     (unsigned)detect_img.width,
                     (unsigned)detect_img.height,
                     g_face_ctx.variant_locked ? 1 : 0,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    } else {
        const dl::detect::result_t &first = detect_res->front();
        g_face_ctx.no_face_streak = 0;
        if ((g_face_ctx.last_detect_log_us == 0) ||
            ((now_us - g_face_ctx.last_detect_log_us) >= (FACE_TRACE_LOG_INTERVAL_MS * 1000LL))) {
            g_face_ctx.last_detect_log_us = now_us;
            ESP_LOGI(TAG,
                     "face detected, infer=%u, count=%u, variant=%s, size=%ux%u, box=[%d,%d,%d,%d], score=%.2f",
                     (unsigned)g_face_ctx.infer_count,
                     (unsigned)detect_res->size(),
                     face_recognition_variant_name(detect_variant),
                     (unsigned)detect_img.width,
                     (unsigned)detect_img.height,
                     first.box[0],
                     first.box[1],
                     first.box[2],
                     first.box[3],
                     first.score);
        }
    }

    if (g_face_ctx.enroll_pending) {
        if (detect_res->empty()) {
            if ((g_face_ctx.enroll_deadline_us == 0) || (now_us > g_face_ctx.enroll_deadline_us)) {
                g_face_ctx.enroll_pending = false;
                g_face_ctx.enroll_deadline_us = 0;
                g_face_ctx.enroll_attempts_left = 0;
                g_face_ctx.last_enroll_attempt_us = 0;
                face_recognition_set_status("录入超时，请保持正脸后重试");
            }
        } else if (!need_feature_pass) {
            face_recognition_set_status("录入中，请保持稳定");
        } else if (g_face_ctx.recognizer->enroll(full_img, scaled_detect_res) == ESP_OK) {
            g_face_ctx.last_enroll_attempt_us = now_us;
            g_face_ctx.enroll_pending = false;
            g_face_ctx.enroll_deadline_us = 0;
            g_face_ctx.enroll_attempts_left = 0;
            g_face_ctx.last_enroll_attempt_us = 0;
            g_face_ctx.snapshot.enrolled_count = g_face_ctx.recognizer->get_num_feats();
            g_face_ctx.labels.emplace_back(face_recognition_make_label(g_face_ctx.labels.size()));
            face_recognition_save_labels();
            face_recognition_sync_labels();
            face_recognition_set_status("录入成功：%s，当前共 %d 人",
                                        face_recognition_get_label((uint16_t)g_face_ctx.snapshot.enrolled_count),
                                        g_face_ctx.snapshot.enrolled_count);
        } else {
            g_face_ctx.last_enroll_attempt_us = now_us;
            full_detect_res = g_face_ctx.detector->run(full_img);
            if (!full_detect_res.empty() && (g_face_ctx.recognizer->enroll(full_img, full_detect_res) == ESP_OK)) {
                g_face_ctx.enroll_pending = false;
                g_face_ctx.enroll_deadline_us = 0;
                g_face_ctx.enroll_attempts_left = 0;
                g_face_ctx.last_enroll_attempt_us = 0;
                g_face_ctx.snapshot.enrolled_count = g_face_ctx.recognizer->get_num_feats();
                g_face_ctx.labels.emplace_back(face_recognition_make_label(g_face_ctx.labels.size()));
                face_recognition_save_labels();
                face_recognition_sync_labels();
                face_recognition_fill_boxes(full_detect_res);
                face_recognition_set_status("录入成功：%s，当前共 %d 人",
                                            face_recognition_get_label((uint16_t)g_face_ctx.snapshot.enrolled_count),
                                            g_face_ctx.snapshot.enrolled_count);
                ESP_LOGI(TAG,
                         "face refine enroll ok, count=%u, size=%ux%u",
                         (unsigned)full_detect_res.size(),
                         (unsigned)full_img.width,
                         (unsigned)full_img.height);
            } else {
                if (g_face_ctx.enroll_attempts_left > 0) {
                    g_face_ctx.enroll_attempts_left--;
                }
                if ((g_face_ctx.enroll_attempts_left == 0) ||
                    ((g_face_ctx.enroll_deadline_us != 0) && (now_us > g_face_ctx.enroll_deadline_us))) {
                    g_face_ctx.enroll_pending = false;
                    g_face_ctx.enroll_deadline_us = 0;
                    g_face_ctx.enroll_attempts_left = 0;
                    g_face_ctx.last_enroll_attempt_us = 0;
                    face_recognition_set_status("录入失败，请保持单人正脸后重试");
                } else {
                    face_recognition_set_status("录入中，请保持正脸并稍等");
                }
            }
        }
    } else if (!detect_res->empty()) {
        if (g_face_ctx.snapshot.enrolled_count > 0) {
            if (need_feature_pass) {
                std::vector<dl::recognition::result_t> result;

                result = g_face_ctx.recognizer->recognize(full_img, scaled_detect_res);
                g_face_ctx.last_recognize_us = now_us;

                if (result.empty()) {
                    full_detect_res = g_face_ctx.detector->run(full_img);
                    if (!full_detect_res.empty()) {
                        result = g_face_ctx.recognizer->recognize(full_img, full_detect_res);
                        if (!result.empty()) {
                            face_recognition_fill_boxes(full_detect_res);
                            ESP_LOGI(TAG,
                                     "face refine ok, count=%u, size=%ux%u",
                                     (unsigned)full_detect_res.size(),
                                     (unsigned)full_img.width,
                                     (unsigned)full_img.height);
                        } else {
                            ESP_LOGW(TAG, "face refine missed, fallback to scaled landmarks");
                        }
                    }
                }

                if (!result.empty()) {
                    uint16_t slot = result.front().id;

                    g_face_ctx.snapshot.recognized = true;
                    g_face_ctx.snapshot.recognized_slot = slot;
                    g_face_ctx.snapshot.similarity = result.front().similarity;
                    g_face_ctx.last_match_valid = true;
                    g_face_ctx.last_match_slot = slot;
                    g_face_ctx.last_match_similarity = result.front().similarity;
                    face_recognition_set_status("识别到：%s  相似度 %.2f",
                                                face_recognition_get_label(slot),
                                                result.front().similarity);
                    if (g_face_ctx.stable_slot == slot) {
                        if (g_face_ctx.stable_hits < 0xFF) {
                            g_face_ctx.stable_hits++;
                        }
                    } else {
                        g_face_ctx.stable_slot = slot;
                        g_face_ctx.stable_hits = 1;
                    }
                    if ((g_face_ctx.stable_hits >= FACE_ALERT_STABLE_HITS) &&
                        ((g_face_ctx.last_alert_slot != slot) ||
                         ((now_us - g_face_ctx.last_alert_us) >= (FACE_ALERT_COOLDOWN_MS * 1000LL)))) {
                        g_face_ctx.last_alert_slot = slot;
                        g_face_ctx.last_alert_us = now_us;
                        face_recognition_emit_alert("检测到%s", face_recognition_get_label(slot));
                    }
                } else {
                    g_face_ctx.last_match_valid = false;
                    g_face_ctx.last_match_slot = 0;
                    g_face_ctx.last_match_similarity = 0.0f;
                    g_face_ctx.stable_slot = 0;
                    g_face_ctx.stable_hits = 0;
                    face_recognition_set_status("检测到人脸，未匹配已录入成员");
                }
            } else if (g_face_ctx.last_match_valid &&
                       ((now_us - g_face_ctx.last_recognize_us) < (FACE_RECOGNITION_CACHE_MS * 1000LL))) {
                g_face_ctx.snapshot.recognized = true;
                g_face_ctx.snapshot.recognized_slot = g_face_ctx.last_match_slot;
                g_face_ctx.snapshot.similarity = g_face_ctx.last_match_similarity;
                face_recognition_set_status("识别到：%s  相似度 %.2f",
                                            face_recognition_get_label(g_face_ctx.last_match_slot),
                                            g_face_ctx.last_match_similarity);
            } else {
                face_recognition_set_status("检测到人脸，识别中");
            }
        } else {
            g_face_ctx.stable_slot = 0;
            g_face_ctx.stable_hits = 0;
            face_recognition_set_status("检测到人脸，按 KEY1 可录入当前人脸");
        }
    } else if (g_face_ctx.snapshot.enrolled_count > 0) {
        g_face_ctx.stable_slot = 0;
        g_face_ctx.stable_hits = 0;
        face_recognition_set_status("未检测到人脸，已录入 %d 人", g_face_ctx.snapshot.enrolled_count);
    } else {
        g_face_ctx.stable_slot = 0;
        g_face_ctx.stable_hits = 0;
        face_recognition_set_status("未检测到人脸，按 KEY1 可录入当前人脸");
    }

    if (status_changed != nullptr) {
        *status_changed = face_recognition_snapshot_changed(before);
    }
    face_recognition_copy_snapshot(snapshot);
    return true;
}
