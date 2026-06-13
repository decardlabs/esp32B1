#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Parse WAV header and return sample rate + PCM data offset.
 * Returns true if valid PCM WAV, false on error. */
bool wav_parse_header(const uint8_t *data, size_t len,
                      uint32_t *sample_rate, size_t *data_offset);

/* Start fetching a URL as a streaming audio source.
 * Calls on_pcm(chunk, len) for each PCM data chunk received.
 * Calls on_finish(error_msg) when complete or on error (NULL = success).
 * Returns immediately; the fetch runs in its own task.
 */
esp_err_t http_stream_start(const char *url,
                            void (*on_pcm)(const uint8_t *data, size_t len),
                            void (*on_finish)(const char *error));

/* Abort an active stream fetch */
void http_stream_abort(void);

/* Returns true if a fetch task is currently running */
bool http_stream_is_busy(void);

#endif /* HTTP_STREAM_H */
