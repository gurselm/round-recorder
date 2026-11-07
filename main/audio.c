#include "audio.h"

#include <math.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

static const char *TAG = "audio";

#define CHANNELS 2
#define HPF_HZ 80.0f

static esp_codec_dev_handle_t s_spk, s_mic;
static int16_t s_in[AUDIO_CHUNK * CHANNELS];
static int16_t s_out[AUDIO_CHUNK * CHANNELS];
static float s_hpf_a, s_hpf_x1, s_hpf_y1;

esp_err_t audio_init(int mic_gain_db, int volume)
{
    /* As muse-gadget-sdk's 1.75C board does: both codecs on the BSP's
     * duplex I2S, stereo at 16 kHz, the two mics averaged. */
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "codec init");
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_RATE,
        .channel = CHANNELS,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open mic");
    audio_set_volume(volume);
    audio_set_mic_gain(mic_gain_db);
    audio_speaker(false);   /* opening the speaker turned the amp on */

    float rc = 1.0f / (2.0f * (float)M_PI * HPF_HZ);
    float dt = 1.0f / AUDIO_RATE;
    s_hpf_a = rc / (rc + dt);
    ESP_LOGI(TAG, "ready: %d Hz, mic gain %d dB, volume %d", AUDIO_RATE, mic_gain_db / 3 * 3, volume);
    return ESP_OK;
}

void audio_set_mic_gain(int db)
{
    /* ES7210 steps are 3 dB, and esp_codec_dev rounds 33 down to 30; the
     * next real step up is 34.5. */
    db = db / 3 * 3;
    esp_codec_dev_set_in_gain(s_mic, db == 33 ? 34.5f : (float)db);
}

void audio_set_volume(int volume)
{
    esp_codec_dev_set_out_vol(s_spk, volume);
}

esp_err_t audio_read(int16_t *mono, size_t frames)
{
    if (frames > AUDIO_CHUNK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (esp_codec_dev_read(s_mic, s_in, frames * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < frames; i++) {
        float x = 0.5f * ((float)s_in[2 * i] + (float)s_in[2 * i + 1]);
        float y = s_hpf_a * (s_hpf_y1 + x - s_hpf_x1);
        s_hpf_x1 = x;
        s_hpf_y1 = y;
        mono[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
    }
    return ESP_OK;
}

esp_err_t audio_write(const int16_t *mono, size_t frames)
{
    if (frames > AUDIO_CHUNK) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < frames; i++) {
        s_out[2 * i] = mono[i];
        s_out[2 * i + 1] = mono[i];
    }
    return esp_codec_dev_write(s_spk, s_out, frames * CHANNELS * sizeof(int16_t)) == ESP_CODEC_DEV_OK ? ESP_OK
                                                                                                    : ESP_FAIL;
}

void audio_speaker(bool on)
{
    gpio_set_level(BSP_POWER_AMP_IO, on);
}

float audio_dbfs(const int16_t *pcm, size_t n)
{
    if (!n) {
        return -100.0f;
    }
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)pcm[i] * pcm[i];
    }
    float db = 10.0f * log10f((float)(acc / n / (32768.0 * 32768.0)) + 1e-10f);
    return db < -100.0f ? -100.0f : db;
}
