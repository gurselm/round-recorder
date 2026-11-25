#include "batt_log.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "recorder.h"

static const char *TAG = "batt_log";

/* The last 64 KB of the spare flash: a header, then 8-byte entries, each
 * written once into erased flash (sectors erased as the log reaches them).
 * Entries from an older log further on are told apart by their session. */
#define REGION 65536u
#define SECTOR 4096u
#define ENTRY 8u
#define HEAD_TAG 0xB6
#define ENTRY_TAG 0xB7
#define EVERY_US 60000000LL   /* an entry a minute */

static const esp_partition_t *s_part;
static uint32_t s_base, s_max;   /* where the log starts; how many entries fit */
static uint8_t s_session;
static uint32_t s_count;         /* entries in this log */
static uint32_t s_last_minute;
static uint8_t s_last_flags;
static bool s_active;            /* logging: off USB since it started or carried on */
static int64_t s_t0, s_last_us;  /* minute 0; the last entry */
static uint8_t s_seen;           /* screen and recording flags since the last entry */

static bool read_entry(uint32_t i, uint8_t e[ENTRY])
{
    return i < s_max && esp_partition_read(s_part, s_base + ENTRY * (1 + i), e, ENTRY) == ESP_OK &&
           e[0] == ENTRY_TAG && e[1] == s_session;
}

static void append(uint32_t minute, uint32_t mv, int pct, uint8_t flags)
{
    if (s_count >= s_max) {
        return;   /* full: over 130 hours */
    }
    uint32_t off = s_base + ENTRY * (1 + s_count);
    if (off % SECTOR == 0 && esp_partition_erase_range(s_part, off, SECTOR) != ESP_OK) {
        return;
    }
    uint8_t e[ENTRY] = {
        ENTRY_TAG, s_session, (uint8_t)(pct < 0 ? 0 : pct), flags,
        (uint8_t)mv, (uint8_t)(mv >> 8), (uint8_t)minute, (uint8_t)(minute >> 8),
    };
    if (esp_partition_write(s_part, off, e, ENTRY) == ESP_OK) {
        s_count++;
        s_last_minute = minute;
        s_last_flags = flags;
    }
}

esp_err_t batt_log_init(void)
{
    uint32_t off, len;
    if (!recorder_spare(&s_part, &off, &len) || len < REGION) {
        ESP_LOGW(TAG, "no room for the battery log");
        return ESP_ERR_NO_MEM;
    }
    s_base = (off + len - REGION) / SECTOR * SECTOR;
    if (s_base < off) {
        s_base += SECTOR;
    }
    s_max = REGION / ENTRY - 1;
    uint8_t head[ENTRY];
    if (esp_partition_read(s_part, s_base, head, ENTRY) == ESP_OK && head[0] == HEAD_TAG) {
        s_session = head[1];
        uint8_t e[ENTRY];
        while (read_entry(s_count, e)) {
            s_last_flags = e[3];
            s_last_minute = e[6] | e[7] << 8;
            s_count++;
        }
    } else {
        s_session = 0;
        s_count = 0;
        s_last_flags = BATT_USB;   /* nothing to carry on */
    }
    ESP_LOGI(TAG, "%" PRIu32 " entries in log %u, at 0x%" PRIx32, s_count, s_session, s_base);
    return ESP_OK;
}

static void start(void)
{
    s_session++;
    s_count = 0;
    uint8_t head[ENTRY] = { HEAD_TAG, s_session };
    if (esp_partition_erase_range(s_part, s_base, SECTOR) != ESP_OK ||
        esp_partition_write(s_part, s_base, head, ENTRY) != ESP_OK) {
        s_max = 0;   /* flash trouble: no log */
    }
    ESP_LOGI(TAG, "log %u started: off USB", s_session);
}

void batt_log_sample(const pmu_power_t *p, bool screen_on, bool recording, bool standby)
{
    if (!s_part || p->battery_pct < 0) {
        return;
    }
    s_seen |= (screen_on ? BATT_SCREEN : 0) | (recording ? BATT_RECORDING : 0) | (standby ? BATT_STANDBY : 0);
    int64_t now = esp_timer_get_time();
    bool on_battery = !p->usb && !p->charging;
    if (!on_battery) {
        if (s_active || (s_count && !(s_last_flags & BATT_USB))) {
            append(s_last_minute + (s_active ? (uint32_t)((now - s_last_us) / EVERY_US) : 0), p->battery_mv,
                   p->battery_pct, BATT_USB | (p->charging ? BATT_CHARGING : 0));
            ESP_LOGI(TAG, "log %u ended: USB, %" PRIu32 " entries", s_session, s_count);
        }
        s_active = false;
        return;
    }
    if (!s_active) {
        uint8_t flags = s_seen;
        if (s_count && !(s_last_flags & BATT_USB)) {
            /* Turned on away from USB: the same discharge, carried on.
             * The time it was off isn't counted. */
            s_t0 = now - (int64_t)(s_last_minute + 1) * EVERY_US;
            flags |= BATT_BOOT;
        } else {
            start();
            s_t0 = now;
        }
        append((uint32_t)((now - s_t0) / EVERY_US), p->battery_mv, p->battery_pct, flags);
        s_active = true;
        s_last_us = now;
        s_seen = 0;
        return;
    }
    if (now - s_last_us >= EVERY_US) {
        append((uint32_t)((now - s_t0) / EVERY_US), p->battery_mv, p->battery_pct, s_seen);
        s_last_us = now;
        s_seen = 0;
    }
}

void batt_log_power_off(void)
{
    if (!s_active) {
        return;
    }
    pmu_power_t p;
    if (pmu_last_power(&p) == ESP_OK) {
        append((uint32_t)((esp_timer_get_time() - s_t0) / EVERY_US), p.battery_mv, p.battery_pct, s_seen | BATT_OFF);
    }
    s_active = false;
}

int batt_log_get(uint32_t i, uint32_t *minute, uint32_t *mv, uint32_t *pct, uint32_t *flags)
{
    uint8_t e[ENTRY];
    if (!s_part || i >= s_count || !read_entry(i, e)) {
        return -1;
    }
    *pct = e[2];
    *flags = e[3];
    *mv = e[4] | e[5] << 8;
    *minute = e[6] | e[7] << 8;
    return 0;
}
