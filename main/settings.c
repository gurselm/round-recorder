#include "settings.h"

#include <inttypes.h>
#include <stdatomic.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NS "recorder"
#define SETTLE_US 1000000

static atomic_int s_gain = CONFIG_REC_MIC_GAIN_DB / 3 * 3;
static atomic_int s_volume = CONFIG_REC_VOLUME;
static atomic_int s_screen_off = CONFIG_REC_SCREEN_OFF_S;
static atomic_int s_sounds = CONFIG_REC_SOUNDS;
static atomic_int s_utc_offset;
static atomic_int s_brightness = 100;
static atomic_int s_aod = 1;          /* the ring stays, dim, with the screen off */
static atomic_int s_tap_wake;
static atomic_int s_intro_seen;
static atomic_int s_tour_seen;
static uint32_t s_boot;             /* this boot's number, counted in NVS */
static atomic_llong s_changed_at;   /* 0 when nothing waits to be written */

static void changed(void);

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void load(nvs_handle_t h, const char *key, atomic_int *v, int lo, int hi)
{
    int32_t got;
    if (nvs_get_i32(h, key, &got) == ESP_OK) {
        atomic_store(v, clamp((int)got, lo, hi));
    }
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is ours alone (partitions.csv), so starting it
         * over loses only our settings. */
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load(h, "gain", &s_gain, 0, 36);
        load(h, "volume", &s_volume, 0, 100);
        load(h, "screen_off", &s_screen_off, 5, 600);
        load(h, "sounds", &s_sounds, 0, 1);
        load(h, "utc_offset", &s_utc_offset, -86400, 86400);
        load(h, "brightness", &s_brightness, 10, 100);
        load(h, "aod", &s_aod, 0, 1);
        load(h, "tap_wake", &s_tap_wake, 0, 1);
        load(h, "intro_seen", &s_intro_seen, 0, 1);
        load(h, "tour", &s_tour_seen, 0, 1);
        nvs_close(h);
    }
    /* Counted at once: a save's time since boot means something only with
     * the boot it was in. */
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot", &s_boot);
        s_boot++;
        nvs_set_u32(h, "boot", s_boot);
        nvs_commit(h);
        nvs_close(h);
    }
    atomic_store(&s_gain, atomic_load(&s_gain) / 3 * 3);
    ESP_LOGI(TAG, "mic gain %d dB, volume %d, screen off after %d s, sounds %s, ring %s, tap to wake %s, boot %" PRIu32,
             atomic_load(&s_gain), atomic_load(&s_volume), atomic_load(&s_screen_off),
             atomic_load(&s_sounds) ? "on" : "off", atomic_load(&s_aod) ? "on" : "off",
             atomic_load(&s_tap_wake) ? "on" : "off", s_boot);
    return ESP_OK;
}

bool settings_sounds(void)
{
    return atomic_load(&s_sounds);
}

uint32_t settings_boot(void)
{
    return s_boot;
}

bool settings_aod(void)
{
    return atomic_load(&s_aod);
}

void settings_set_aod(bool on)
{
    if (atomic_exchange(&s_aod, on) != on) {
        changed();
    }
}

bool settings_tap_wake(void)
{
    return atomic_load(&s_tap_wake);
}

void settings_set_tap_wake(bool on)
{
    if (atomic_exchange(&s_tap_wake, on) != on) {
        changed();
    }
}

bool settings_intro_seen(void)
{
    return atomic_load(&s_intro_seen);
}

void settings_set_intro_seen(void)
{
    if (!atomic_exchange(&s_intro_seen, 1)) {
        changed();
    }
}

bool settings_tour_seen(void)
{
    return atomic_load(&s_tour_seen);
}

void settings_set_tour_seen(void)
{
    if (!atomic_exchange(&s_tour_seen, 1)) {
        changed();
    }
}

int settings_brightness(void)
{
    return atomic_load(&s_brightness);
}

void settings_set_brightness(int pct)
{
    pct = clamp(pct, 10, 100);
    if (atomic_exchange(&s_brightness, pct) != pct) {
        changed();
    }
}

int settings_utc_offset(void)
{
    return atomic_load(&s_utc_offset);
}

void settings_set_utc_offset(int s)
{
    s = clamp(s, -86400, 86400);
    if (atomic_exchange(&s_utc_offset, s) != s) {
        changed();
    }
}

int settings_mic_gain_db(void)
{
    return atomic_load(&s_gain);
}

int settings_volume(void)
{
    return atomic_load(&s_volume);
}

int settings_screen_off_s(void)
{
    return atomic_load(&s_screen_off);
}

static void changed(void)
{
    int64_t now = esp_timer_get_time();
    atomic_store(&s_changed_at, now ? now : 1);
}

void settings_set_mic_gain_db(int db)
{
    db = clamp(db, 0, 36) / 3 * 3;
    if (atomic_exchange(&s_gain, db) != db) {
        audio_set_mic_gain(db);
        changed();
    }
}

void settings_set_volume(int volume)
{
    volume = clamp(volume, 0, 100);
    if (atomic_exchange(&s_volume, volume) != volume) {
        audio_set_volume(volume);
        changed();
    }
}

void settings_set_screen_off_s(int s)
{
    s = clamp(s, 5, 600);
    if (atomic_exchange(&s_screen_off, s) != s) {
        changed();
    }
}

void settings_set_sounds(bool on)
{
    if (atomic_exchange(&s_sounds, on) != on) {
        changed();
    }
}

void settings_commit(void)
{
    int64_t at = atomic_load(&s_changed_at);
    if (!at || esp_timer_get_time() - at < SETTLE_US ||
        !atomic_compare_exchange_strong(&s_changed_at, &at, 0)) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_i32(h, "gain", atomic_load(&s_gain));
        nvs_set_i32(h, "volume", atomic_load(&s_volume));
        nvs_set_i32(h, "screen_off", atomic_load(&s_screen_off));
        nvs_set_i32(h, "sounds", atomic_load(&s_sounds));
        nvs_set_i32(h, "utc_offset", atomic_load(&s_utc_offset));
        nvs_set_i32(h, "brightness", atomic_load(&s_brightness));
        nvs_set_i32(h, "aod", atomic_load(&s_aod));
        nvs_set_i32(h, "tap_wake", atomic_load(&s_tap_wake));
        nvs_set_i32(h, "intro_seen", atomic_load(&s_intro_seen));
        nvs_set_i32(h, "tour", atomic_load(&s_tour_seen));
        err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not saved: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved: mic gain %d dB, volume %d, screen off after %d s, sounds %s", atomic_load(&s_gain),
                 atomic_load(&s_volume), atomic_load(&s_screen_off), atomic_load(&s_sounds) ? "on" : "off");
    }
}
