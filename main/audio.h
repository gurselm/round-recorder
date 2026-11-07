#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_RATE 16000
#define AUDIO_CHUNK 320   /* 20 ms */

/* Opens the ES7210 mics and the ES8311 speaker at 16 kHz, amp off. */
esp_err_t audio_init(int mic_gain_db, int volume);

/* Either may be called from any task, while recording or playing. */
void audio_set_mic_gain(int db);   /* rounded down to 3 dB steps */
void audio_set_volume(int volume); /* 0 to 100 */

/* Mono from both mics, high-passed at 80 Hz; up to AUDIO_CHUNK frames,
 * waits for them. One task only. */
esp_err_t audio_read(int16_t *mono, size_t frames);

/* Up to AUDIO_CHUNK frames to the speaker. One task only. */
esp_err_t audio_write(const int16_t *mono, size_t frames);

/* The speaker amp: off except while playing, as it draws power and hisses. */
void audio_speaker(bool on);

float audio_dbfs(const int16_t *pcm, size_t n);
