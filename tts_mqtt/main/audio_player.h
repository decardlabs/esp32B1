#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Audio playback states */
typedef enum {
    AUDIO_IDLE,
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_ERROR,
} audio_state_t;

/* Callback when a stream URL finishes playing (naturally or error) */
typedef void (*audio_stopped_cb_t)(void);

esp_err_t audio_player_init(void);
esp_err_t audio_player_set_volume(int vol_db);   /* -96 to 0 */
int audio_player_get_volume(void);
audio_state_t audio_player_get_state(void);
const char *audio_player_get_state_str(void);

/* Feed PCM data into the player's ring buffer from HTTP fetcher task */
void audio_player_feed_pcm(const uint8_t *data, size_t len);

/* Start playing a WAV stream (caller feeds PCM via feed_pcm after header parse) */
esp_err_t audio_player_start_stream(uint32_t sample_rate, audio_stopped_cb_t on_stopped);

/* Signal end of PCM stream (EOF) */
void audio_player_finish_stream(void);

/* Controls: pause, resume, stop */
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_stop(void);

/* Play a test tone directly */
void audio_player_play_tone(uint16_t freq_hz, uint16_t duration_ms);

#endif /* AUDIO_PLAYER_H */
