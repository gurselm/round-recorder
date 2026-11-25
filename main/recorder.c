#include "recorder.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rec_ring.h"
#include "settings.h"
#include "tempo.h"

static const char *TAG = "recorder";

#define BLOCKS (CONFIG_REC_SECONDS * REC_BLOCKS_PER_S)
#define CLIPS_SUBTYPE 0x40
#define PLAY_BATCH 25   /* blocks read from flash at a time: half a second */
#define COPY_BATCH 250  /* blocks a save copies per hold of the ring's lock: 41 KB, a few ms */
#define GAP_US 60000    /* a mic read this late means the I2S DMA nearly overran */
#define PART_MARGIN (5 * REC_BLOCKS_PER_S)   /* a part goes to flash this long before the ring would lose it */
#define MAX_REC_BLOCKS (CONFIG_REC_KEEP_MAX_S * REC_BLOCKS_PER_S)

static rec_ring_t s_ring;
static SemaphoreHandle_t s_ring_lock;   /* the audio task's feed against a save's copy */
static rec_store_t s_store;
/* Changes to the store (save, delete, name) take turns; reading it needs no
 * lock, so a save's seconds of erasing never hold up the screen or playback.
 * A read racing a change sees the slot's clip before or after it, or none. */
static SemaphoreHandle_t s_store_lock;
static SemaphoreHandle_t s_list_lock;   /* s_clips and s_recs */
static uint8_t *s_snapshot;             /* a save's copy of the ring, PSRAM */
static rec_clip_t *s_clips;             /* one per slot, PSRAM; under s_list_lock */
static rec_recording_t *s_recs;         /* likewise */
static tempo_t *s_tempo;                /* the play task's, PSRAM */
static TaskHandle_t s_save_task, s_play_task;
static uint32_t s_max_parts;

static atomic_bool s_paused;
static atomic_int s_level_milli;
static atomic_int s_save_state;
static atomic_bool s_stop_req;          /* end the recording */
static atomic_bool s_snapshot_only;     /* Save: what's held, and no more */
/* The last save, which Keep recording carries on: its first part, and where
 * in the ring's numbering it ended. First 0 once it can't be. */
static atomic_uint s_cont_first, s_cont_next, s_cont_blocks;
static atomic_bool s_cont_req;
static atomic_llong s_saved_at;         /* when the last save was done, us; 0 before one */
static atomic_uint s_rec_first;         /* the recording going on, or 0 */
static atomic_uint s_rec_held, s_rec_mark;   /* at Save: blocks held, and the ring's count then */
static atomic_uint s_saved_end;   /* the ring's count where the last save or recording ended */
static atomic_int s_chime;              /* a sound for the play task to make, or CHIME_NONE */
static rec_deleted_t s_undo[REC_MAX_PARTS];   /* the last recording deleted; under the store's lock */
static int s_undo_n;
static atomic_uint s_last_slot, s_last_seq;   /* seq 0: nothing saved yet */
static atomic_uint s_changes;
static const esp_partition_t *s_part;   /* the clips partition */

static atomic_uint s_play_req;          /* a recording to play, or 0 */
static atomic_uint s_play_first;        /* the one playing, or 0 */
static atomic_uint s_play_block, s_play_blocks;
static atomic_bool s_play_stop, s_play_paused;
static atomic_int s_play_seek = -1;
static atomic_int s_play_speed = 100;

static atomic_bool s_time_set;

static int part_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return esp_partition_read(ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int part_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    return esp_partition_write(ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int part_erase(void *ctx, uint32_t off, uint32_t len)
{
    return esp_partition_erase_range(ctx, off, len) == ESP_OK ? 0 : -1;
}

static void lock_store(void)
{
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
}

static void unlock_store(void)
{
    xSemaphoreGive(s_store_lock);
}

static void lock_lists(void)
{
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
}

static void unlock_lists(void)
{
    xSemaphoreGive(s_list_lock);
}

static void audio_task(void *arg)
{
    static int16_t pcm[AUDIO_CHUNK];
    int64_t last = 0;
    for (;;) {
        if (audio_read(pcm, AUDIO_CHUNK) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (last && now - last > GAP_US) {
            ESP_LOGW(TAG, "mic read %lld ms after the last", (now - last) / 1000);
        }
        last = now;
        /* Map -60..-10 dBFS onto 0..1 for the meter. */
        float level = (audio_dbfs(pcm, AUDIO_CHUNK) + 60.0f) / 50.0f;
        atomic_store(&s_level_milli, (int)(1000 * (level < 0 ? 0 : level > 1 ? 1 : level)));
        if (!atomic_load(&s_paused)) {
            xSemaphoreTake(s_ring_lock, portMAX_DELAY);
            rec_ring_feed(&s_ring, pcm, AUDIO_CHUNK);
            xSemaphoreGive(s_ring_lock);
        }
    }
}

/* Copies span from the ring a piece at a time, so the mic task never waits
 * on the whole copy, and saves it. The copy runs many times faster than
 * recording overwrites the oldest blocks, so it loses none unless starved. */
static int save_span(rec_span_t span, uint32_t first_seq, rec_clip_t *clip)
{
    for (uint32_t n = 0; n < span.count;) {
        uint32_t k = span.count - n < COPY_BATCH ? span.count - n : COPY_BATCH;
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        int lost = rec_ring_copy_span(&s_ring, span, n, k, s_snapshot + (size_t)n * REC_BLOCK_BYTES);
        xSemaphoreGive(s_ring_lock);
        if (lost) {
            return -1;
        }
        n += k;
    }
    lock_store();
    int err = rec_store_save(&s_store, s_snapshot, span.count, recorder_time(), first_seq, settings_boot(),
                             (uint32_t)(esp_timer_get_time() / 1000000), clip);
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

static rec_save_state_t failure(int err)
{
    return err == REC_FULL ? REC_SAVE_FULL : REC_SAVE_FAILED;
}

enum { CHIME_NONE, CHIME_START, CHIME_STOP, CHIME_ERROR, CHIME_SAVED };

/* A short sound, if Settings has them on: the play task makes it, unless
 * it's playing a recording. */
static void chime(int kind)
{
    if (settings_sounds()) {
        atomic_store(&s_chime, kind);
        xTaskNotifyGive(s_play_task);
    }
}

static void save_task(void *arg)
{
    uint32_t first = 0, next = 0, parts = 0, blocks = 0;   /* the recording going on */
    for (;;) {
        /* While recording, look every second for a part to write. */
        ulTaskNotifyTake(pdTRUE, first ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        rec_span_t held = rec_ring_span(&s_ring);
        rec_span_t fresh_span = rec_ring_span_since(&s_ring, atomic_load(&s_saved_end));   /* a new save's */
        xSemaphoreGive(s_ring_lock);
        uint32_t end = held.first + held.count;   /* in the ring's numbering */
        rec_clip_t clip;
        if (!first && atomic_exchange(&s_cont_req, false)) {
            /* Keep recording: on from where the save ended, as one recording. */
            first = atomic_exchange(&s_cont_first, 0);
            next = atomic_load(&s_cont_next);
            blocks = atomic_load(&s_cont_blocks);
            parts = 1;
            if (!first || end - next >= held.count) {
                ESP_LOGE(TAG, "too late to carry on recording %" PRIu32, first);
                first = 0;
                atomic_store(&s_save_state, REC_SAVE_FAILED);
                chime(CHIME_ERROR);
                continue;
            }
            atomic_store(&s_rec_first, first);
            ESP_LOGI(TAG, "recording %" PRIu32 ": carried on from its save", first);
        }
        if (!first) {
            int st = atomic_load(&s_save_state);
            if (st != REC_SAVE_BUSY && st != REC_SAVE_RECORDING) {
                continue;
            }
            int err = fresh_span.count ? save_span(fresh_span, 0, &clip) : -1;
            if (err) {
                ESP_LOGE(TAG, "save failed%s", err == REC_FULL ? ": every clip is in a kept recording" : "");
                atomic_store(&s_save_state, failure(err));
                chime(CHIME_ERROR);
                continue;
            }
            first = clip.seq;
            next = end;
            parts = 1;
            blocks = fresh_span.count;
            atomic_store(&s_saved_end, end);   /* the next save starts here */
            atomic_store(&s_last_slot, clip.slot);
            atomic_store(&s_last_seq, clip.seq);
            if (atomic_load(&s_snapshot_only)) {
                ESP_LOGI(TAG, "saved %" PRIu32 ": the %.1f s held, in slot %" PRIu32, first,
                         (float)blocks / REC_BLOCKS_PER_S, clip.slot);
                atomic_store(&s_cont_next, end);   /* for Keep recording */
                atomic_store(&s_cont_blocks, blocks);
                atomic_store(&s_cont_first, first);
                atomic_store(&s_saved_at, esp_timer_get_time());
                first = 0;
                atomic_store(&s_save_state, REC_SAVE_DONE);
                continue;
            }
            atomic_store(&s_rec_first, first);
            ESP_LOGI(TAG, "recording %" PRIu32 ": the %.1f s held, in slot %" PRIu32, first,
                     (float)blocks / REC_BLOCKS_PER_S, clip.slot);
        }
        /* A part when the ring is about to lose what's not yet saved, or
         * at the end: Save again, or the longest a recording may be. */
        uint32_t fresh = end - next;
        uint32_t room = MAX_REC_BLOCKS > blocks ? MAX_REC_BLOCKS - blocks : 0;
        bool asked = atomic_exchange(&s_stop_req, false);
        bool stop = asked || fresh >= room;
        if (!stop && fresh < BLOCKS - PART_MARGIN) {
            continue;
        }
        uint32_t k = fresh < room ? fresh : room;
        k = k < BLOCKS ? k : BLOCKS;
        int err = 0;
        if (k >= REC_BLOCKS_PER_S) {   /* less than a second at the end isn't worth a slot */
            err = save_span((rec_span_t){ .first = next, .count = k }, first, &clip);
            if (!err) {
                next += k;
                atomic_store(&s_saved_end, next);
                blocks += k;
                parts++;
                ESP_LOGI(TAG, "recording %" PRIu32 ": part %" PRIu32 ", %.1f s, in slot %" PRIu32, first, parts,
                         (float)k / REC_BLOCKS_PER_S, clip.slot);
            }
        }
        if (stop || err || parts >= s_max_parts) {
            if (err) {
                ESP_LOGE(TAG, "recording %" PRIu32 " cut short: %s", first,
                         err == REC_FULL ? "every clip is in a kept recording" : "flash");
            }
            if (err || !asked) {
                chime(err ? CHIME_ERROR : CHIME_STOP);   /* a press already sounded its own stop */
            }
            ESP_LOGI(TAG, "recording %" PRIu32 " done: %.1f s in %" PRIu32 " part%s", first,
                     (float)blocks / REC_BLOCKS_PER_S, parts, parts == 1 ? "" : "s");
            first = 0;
            atomic_store(&s_rec_first, 0);
            atomic_store(&s_save_state, err ? failure(err) : REC_SAVE_DONE);
        }
    }
}

/* Under s_list_lock: the recordings, in s_recs. */
static int group_locked(void)
{
    int n = rec_store_list(&s_store, s_clips, (int)s_store.slots);
    int count = rec_group(s_clips, n, s_recs, (int)s_store.slots);
    for (int i = 0; i < count; i++) {
        rec_group_envelope(&s_recs[i], s_clips, n, s_recs[i].env);
    }
    return count;
}

static bool find_recording(uint32_t first, rec_recording_t *out)
{
    lock_lists();
    int n = group_locked();
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (s_recs[i].first == first) {
            *out = s_recs[i];
            found = true;
        }
    }
    unlock_lists();
    return found;
}

/* The I2S DMA repeats its last buffers when nothing new comes: fill them with
 * silence before the amp goes off. */
static void quiet(void)
{
    static const int16_t zero[REC_BLOCK_SAMPLES];
    for (int i = 0; i < 6; i++) {
        audio_write(zero, REC_BLOCK_SAMPLES);
    }
    audio_speaker(false);
}

/* Two 80 ms notes, up for a start, down for a stop, the same twice for a
 * save, low for a failure: heard
 * in a pocket, and left in the recording as a marker. */
static void play_chime(int kind)
{
    static const float NOTES[][2] = { [CHIME_START] = { 880, 1320 }, [CHIME_STOP] = { 1320, 880 },
                                      [CHIME_ERROR] = { 330, 247 }, [CHIME_SAVED] = { 1320, 1320 } };
    enum { NOTE_BLOCKS = 4, NOTE_SAMPLES = NOTE_BLOCKS * REC_BLOCK_SAMPLES };
    static int16_t pcm[REC_BLOCK_SAMPLES];
    audio_speaker(true);
    for (int n = 0; n < 2; n++) {
        for (int b = 0; b < NOTE_BLOCKS; b++) {
            for (int i = 0; i < REC_BLOCK_SAMPLES; i++) {
                int k = b * REC_BLOCK_SAMPLES + i;
                float env = sinf((float)M_PI * (float)k / NOTE_SAMPLES);   /* no clicks at either end */
                pcm[i] = (int16_t)(6000.0f * env * sinf(2.0f * (float)M_PI * NOTES[kind][n] * (float)k / REC_RATE));
            }
            audio_write(pcm, REC_BLOCK_SAMPLES);
        }
    }
    quiet();
}

static void play_task(void *arg)
{
    static uint8_t buf[PLAY_BATCH * REC_BLOCK_BYTES];
    static int16_t pcm[REC_BLOCK_SAMPLES], out[TEMPO_HOP];
    tempo_t *tempo = s_tempo;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int kind = atomic_exchange(&s_chime, CHIME_NONE);
        if (kind != CHIME_NONE) {
            play_chime(kind);
        }
        uint32_t first = atomic_exchange(&s_play_req, 0);
        rec_recording_t rec;
        if (!first || !find_recording(first, &rec)) {
            continue;
        }
        uint32_t part_blocks[REC_MAX_PARTS];
        for (uint32_t p = 0; p < rec.parts; p++) {
            rec_clip_t c;
            part_blocks[p] = recorder_clip(rec.slots[p], &c) ? 0 : c.blocks;
        }
        atomic_store(&s_play_stop, false);
        atomic_store(&s_play_paused, false);
        atomic_store(&s_play_seek, -1);
        atomic_store(&s_play_blocks, rec.blocks);
        atomic_store(&s_play_block, 0);
        atomic_store(&s_play_first, first);
        int speed = atomic_load(&s_play_speed);
        tempo_init(tempo, speed);
        audio_speaker(true);
        bool amp = true;
        uint32_t pos = 0;
        while (!atomic_load(&s_play_stop)) {
            int seek = atomic_exchange(&s_play_seek, -1);
            if (seek >= 0) {
                pos = (uint32_t)seek < rec.blocks ? (uint32_t)seek : rec.blocks;
                atomic_store(&s_play_block, pos);
                tempo_init(tempo, speed);
            }
            if (atomic_load(&s_play_speed) != speed) {
                speed = atomic_load(&s_play_speed);
                tempo_init(tempo, speed);
            }
            if (pos >= rec.blocks) {
                atomic_store(&s_play_paused, true);   /* at the end; play starts it over */
            }
            if (atomic_load(&s_play_paused)) {
                if (amp) {
                    quiet();
                    amp = false;
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            if (!amp) {
                audio_speaker(true);
                amp = true;
            }
            uint32_t p = 0, off = pos;
            while (p < rec.parts && off >= part_blocks[p]) {
                off -= part_blocks[p++];
            }
            if (p == rec.parts) {
                pos = rec.blocks;
                continue;
            }
            uint32_t k = part_blocks[p] - off < PLAY_BATCH ? part_blocks[p] - off : PLAY_BATCH;
            if (recorder_read(rec.slots[p], off * REC_BLOCK_BYTES, buf, k * REC_BLOCK_BYTES)) {
                break;
            }
            for (uint32_t i = 0; i < k && !atomic_load(&s_play_stop) && !atomic_load(&s_play_paused) &&
                                 atomic_load(&s_play_seek) < 0;
                 i++) {
                rec_block_decode(buf + i * REC_BLOCK_BYTES, pcm);
                tempo_put(tempo, pcm, REC_BLOCK_SAMPLES);
                size_t m;
                while ((m = tempo_get(tempo, out, TEMPO_HOP)) > 0) {
                    audio_write(out, m);
                }
                atomic_store(&s_play_block, ++pos);
            }
        }
        if (amp) {
            quiet();
        }
        atomic_store(&s_chime, CHIME_NONE);   /* skipped while playing, and stale now */
        atomic_store(&s_play_first, 0);
        if (atomic_load(&s_play_req)) {
            xTaskNotifyGive(s_play_task);   /* another recording asked for meanwhile */
        }
    }
}

esp_err_t recorder_start(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, CLIPS_SUBTYPE, "clips");
    s_part = part;
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no clips partition");
    /* A flash chip that can't address past 16 MB is used up to there only. */
    uint32_t chip = 0;
    esp_flash_get_size(part->flash_chip, &chip);
    uint32_t size = part->address >= chip ? 0 : chip - part->address < part->size ? chip - part->address : part->size;
    rec_flash_t flash = {
        .read = part_read,
        .write = part_write,
        .erase = part_erase,
        .ctx = (void *)part,
        .size = size,
    };
    ESP_RETURN_ON_FALSE(rec_store_open(&s_store, &flash, BLOCKS) == 0, ESP_ERR_INVALID_SIZE, TAG,
                        "clips partition too small (%" PRIu32 " of %" PRIu32 " bytes usable)", size, part->size);

    uint8_t *ring = heap_caps_malloc((size_t)BLOCKS * REC_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    s_snapshot = heap_caps_malloc((size_t)BLOCKS * REC_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    s_clips = heap_caps_malloc(s_store.slots * sizeof(*s_clips), MALLOC_CAP_SPIRAM);
    s_recs = heap_caps_malloc(s_store.slots * sizeof(*s_recs), MALLOC_CAP_SPIRAM);
    s_tempo = heap_caps_malloc(sizeof(*s_tempo), MALLOC_CAP_SPIRAM);
    s_ring_lock = xSemaphoreCreateMutex();
    s_store_lock = xSemaphoreCreateMutex();
    s_list_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(ring && s_snapshot && s_clips && s_recs && s_tempo && s_ring_lock && s_store_lock && s_list_lock, ESP_ERR_NO_MEM, TAG,
                        "no memory");
    rec_ring_init(&s_ring, ring, BLOCKS);
    /* A recording leaves at least one slot to the others. */
    s_max_parts = s_store.slots - 1 < REC_MAX_PARTS ? s_store.slots - 1 : REC_MAX_PARTS;
    s_max_parts = s_max_parts ? s_max_parts : 1;

    rec_clip_t clips[1];
    int saved = rec_store_list(&s_store, clips, 1);
    ESP_LOGI(TAG, "%d s kept; %" PRIu32 " slots of %" PRIu32 " KB%s, newest clip %" PRIu32, CONFIG_REC_SECONDS,
             s_store.slots, s_store.slot_bytes / 1024, size < part->size ? " (flash past 16 MB unusable)" : "",
             saved ? clips[0].seq : 0);
    uint32_t longest = s_max_parts * CONFIG_REC_SECONDS < CONFIG_REC_KEEP_MAX_S ? s_max_parts * CONFIG_REC_SECONDS
                                                                                : CONFIG_REC_KEEP_MAX_S;
    ESP_LOGI(TAG, "recordings up to %" PRIu32 " s, in parts of up to %d s", longest, CONFIG_REC_SECONDS);

    /* The mic reader comes first: a stall of more than the I2S DMA's 90 ms
     * loses audio. Flash writes go at the lowest priority. Stacks in
     * internal RAM, as flash writes stop the PSRAM cache. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(audio_task, "rec_audio", 4096, NULL, 12, NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "audio task");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(play_task, "rec_play", 4096, NULL, 10, &s_play_task, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "play task");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(save_task, "rec_save", 4096, NULL, 2, &s_save_task, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "save task");
    return ESP_OK;
}

void recorder_set_paused(bool paused)
{
    atomic_store(&s_paused, paused);
    ESP_LOGI(TAG, "%s", paused ? "paused" : "recording");
}

bool recorder_paused(void)
{
    return atomic_load(&s_paused);
}

float recorder_level(void)
{
    return atomic_load(&s_level_milli) / 1000.0f;
}

/* Blocks a new save would keep. */
static uint32_t fresh_blocks(void)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t n = rec_ring_span_since(&s_ring, atomic_load(&s_saved_end)).count;
    xSemaphoreGive(s_ring_lock);
    return n;
}

uint32_t recorder_seconds(void)
{
    return fresh_blocks() / REC_BLOCKS_PER_S;
}

static bool saving(void)
{
    int st = atomic_load(&s_save_state);
    return st == REC_SAVE_BUSY || st == REC_SAVE_RECORDING;
}

/* Starts a recording or ends the one going on; false if neither happened. */
/* Saves what's held: alone (snapshot), or as the start of a recording that
 * goes on until stop(). False if one is already being made. */
static bool start(bool snapshot, bool sound)
{
    if (fresh_blocks() < REC_BLOCKS_PER_S) {
        return false;   /* nothing new since the last save */
    }
    int st = atomic_load(&s_save_state);
    if (st == REC_SAVE_BUSY || st == REC_SAVE_RECORDING ||
        !atomic_compare_exchange_strong(&s_save_state, &st, snapshot ? REC_SAVE_BUSY : REC_SAVE_RECORDING)) {
        return false;
    }
    atomic_store(&s_snapshot_only, snapshot);
    atomic_store(&s_stop_req, false);
    atomic_store(&s_cont_first, 0);   /* a new save: the last one can't be carried on */
    /* Where the recording stands now, for its length on the screen from the
     * press on, though the flash takes seconds to catch up. */
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    atomic_store(&s_rec_held, rec_ring_span_since(&s_ring, atomic_load(&s_saved_end)).count);
    atomic_store(&s_rec_mark, s_ring.written);
    xSemaphoreGive(s_ring_lock);
    if (sound) {
        chime(snapshot ? CHIME_SAVED : CHIME_START);
    }
    xTaskNotifyGive(s_save_task);
    return true;
}

static void stop(bool sound)
{
    atomic_store(&s_stop_req, true);
    if (sound) {
        chime(CHIME_STOP);
    }
    xTaskNotifyGive(s_save_task);
}

bool recorder_save(void)
{
    if (atomic_load(&s_save_state) == REC_SAVE_RECORDING) {
        stop(true);   /* Save while recording: the recording is what's being saved */
        return true;
    }
    return start(true, true);
}

bool recorder_can_continue(void)
{
    if (atomic_load(&s_save_state) != REC_SAVE_DONE || !atomic_load(&s_cont_first)) {
        return false;
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t since = s_ring.written - atomic_load(&s_cont_next);
    xSemaphoreGive(s_ring_lock);
    return since < BLOCKS - PART_MARGIN;   /* the ring still holds where the save ended */
}

void recorder_continue(void)
{
    int done = REC_SAVE_DONE;
    if (!recorder_can_continue() || !atomic_compare_exchange_strong(&s_save_state, &done, REC_SAVE_RECORDING)) {
        return;
    }
    atomic_store(&s_stop_req, false);
    atomic_store(&s_rec_held, atomic_load(&s_cont_blocks));   /* its length on the screen: the save and on */
    atomic_store(&s_rec_mark, atomic_load(&s_cont_next));
    atomic_store(&s_cont_req, true);
    chime(CHIME_START);
    xTaskNotifyGive(s_save_task);
}

uint32_t recorder_saved_ms(void)
{
    int64_t at = atomic_load(&s_saved_at);
    return at ? (uint32_t)((esp_timer_get_time() - at) / 1000) : UINT32_MAX;
}

void recorder_warn(void)
{
    chime(CHIME_ERROR);
}

void recorder_beep(void)
{
    atomic_store(&s_chime, CHIME_SAVED);   /* the speaker's volume to hear, sounds on or off */
    xTaskNotifyGive(s_play_task);
}

rec_save_state_t recorder_save_state(void)
{
    return atomic_load(&s_save_state);
}

uint32_t recorder_recording_seconds(void)
{
    if (atomic_load(&s_save_state) != REC_SAVE_RECORDING) {
        return 0;
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t since = s_ring.written - atomic_load(&s_rec_mark);
    xSemaphoreGive(s_ring_lock);
    uint32_t blocks = atomic_load(&s_rec_held) + since;
    return (blocks < MAX_REC_BLOCKS ? blocks : MAX_REC_BLOCKS) / REC_BLOCKS_PER_S;
}

uint32_t recorder_recording(void)
{
    return atomic_load(&s_rec_first);
}

bool recorder_last_saved(uint32_t *slot, uint32_t *seq)
{
    *seq = atomic_load(&s_last_seq);
    *slot = atomic_load(&s_last_slot);
    return *seq != 0;
}

void recorder_finish(bool save_idle, uint32_t timeout_ms)
{
    if (saving()) {
        stop(false);   /* quietly: the power is going */
    } else if (save_idle) {
        start(true, false);
    } else {
        return;
    }
    for (uint32_t ms = 0; saving() && ms < timeout_ms; ms += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (saving()) {
        ESP_LOGW(TAG, "still saving after %" PRIu32 " ms", timeout_ms);
    }
}

uint32_t recorder_changes(void)
{
    return atomic_load(&s_changes);
}

uint32_t recorder_slots(void)
{
    return s_store.slots;
}

bool recorder_spare(const esp_partition_t **part, uint32_t *off, uint32_t *len)
{
    if (!s_part || !s_store.slots) {
        return false;
    }
    *part = s_part;
    *off = s_store.slots * s_store.slot_bytes;
    *len = s_store.f.size > *off ? s_store.f.size - *off : 0;
    return true;
}

int recorder_list(rec_clip_t *out, int max)
{
    return rec_store_list(&s_store, out, max);
}

int recorder_clip(uint32_t slot, rec_clip_t *out)
{
    return rec_store_clip(&s_store, slot, out) ? 0 : -1;
}

int recorder_read(uint32_t slot, uint32_t off, void *buf, uint32_t len)
{
    return rec_store_read(&s_store, slot, off, buf, len);
}

int recorder_delete(uint32_t slot)
{
    lock_store();
    int err = rec_store_delete(&s_store, slot);
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

int recorder_set_name(uint32_t slot, uint32_t seq, const char *name)
{
    lock_store();
    int err = rec_store_set_name(&s_store, slot, seq, name);
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

int recorder_recordings(rec_recording_t *out, int max)
{
    lock_lists();
    int n = group_locked();
    n = n < max ? n : max;
    memcpy(out, s_recs, (size_t)n * sizeof(*out));
    unlock_lists();
    return n;
}

int recorder_delete_recording(uint32_t first)
{
    if (first == atomic_load(&s_rec_first)) {
        return -1;   /* still being recorded */
    }
    if (atomic_load(&s_play_first) == first) {
        recorder_play_stop();
        for (int i = 0; i < 20 && atomic_load(&s_play_first) == first; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* so it isn't reading what's erased */
        }
    }
    lock_store();
    lock_lists();
    int n = rec_store_list(&s_store, s_clips, (int)s_store.slots), err = 0;
    s_undo_n = 0;
    for (int i = 0; i < n; i++) {
        if (s_clips[i].first != first) {
            continue;
        }
        rec_deleted_t *keep = s_undo_n < REC_MAX_PARTS ? &s_undo[s_undo_n] : NULL;
        if (keep ? rec_store_delete_undoable(&s_store, s_clips[i].slot, keep)
                 : rec_store_delete(&s_store, s_clips[i].slot)) {
            err = -1;
        } else if (keep) {
            s_undo_n++;
        }
    }
    unlock_lists();
    unlock_store();
    atomic_fetch_add(&s_changes, 1);
    return err;
}

int recorder_undo_delete(void)
{
    lock_store();
    int err = s_undo_n ? 0 : -1;
    for (int i = 0; i < s_undo_n; i++) {
        if (rec_store_restore(&s_store, &s_undo[i])) {
            err = -1;   /* a save took that part's slot meanwhile */
        }
    }
    s_undo_n = 0;
    unlock_store();
    atomic_fetch_add(&s_changes, 1);
    return err;
}

/* Names or keeps a recording, on its first part: kept < 0 leaves the flag. */
static int set_first(uint32_t first, const char *name, int kept)
{
    lock_store();
    lock_lists();
    int n = rec_store_list(&s_store, s_clips, (int)s_store.slots), err = -1;
    for (int i = 0; i < n; i++) {
        if (s_clips[i].seq == first) {
            err = name ? rec_store_set_name(&s_store, s_clips[i].slot, first, name)
                       : rec_store_set_kept(&s_store, s_clips[i].slot, first, kept);
        }
    }
    unlock_lists();
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

int recorder_name_recording(uint32_t first, const char *name)
{
    return set_first(first, name, -1);
}

int recorder_protect_recording(uint32_t first, bool kept)
{
    return set_first(first, NULL, kept);
}

int recorder_set_pulled(uint32_t slot, uint32_t seq)
{
    lock_store();
    int err = rec_store_set_pulled(&s_store, slot, seq);
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

/* Every recording that isn't protected or still being recorded, or with
 * pulled_only, just those downloaded too; how many. */
static int delete_many(bool pulled_only)
{
    uint32_t going = atomic_load(&s_rec_first);
    lock_store();
    lock_lists();
    int n = group_locked(), deleted = 0;
    for (int i = 0; i < n; i++) {
        const rec_recording_t *rec = &s_recs[i];
        if ((pulled_only && !rec->pulled) || rec->kept || rec->first == going) {
            continue;   /* not on the laptop, protected, or still being recorded */
        }
        for (uint32_t p = 0; p < rec->parts; p++) {
            rec_store_delete(&s_store, rec->slots[p]);
        }
        deleted++;
    }
    unlock_lists();
    unlock_store();
    atomic_fetch_add(&s_changes, 1);
    return deleted;
}

int recorder_delete_downloaded(void)
{
    return delete_many(true);
}

int recorder_delete_all(void)
{
    return delete_many(false);
}

int recorder_set_kept(uint32_t slot, uint32_t seq, bool kept)
{
    lock_store();
    int err = rec_store_set_kept(&s_store, slot, seq, kept);
    unlock_store();
    if (!err) {
        atomic_fetch_add(&s_changes, 1);
    }
    return err;
}

void recorder_play(uint32_t first)
{
    atomic_store(&s_play_req, first);
    atomic_store(&s_play_stop, true);   /* ends whatever plays; play_task then takes the request */
    xTaskNotifyGive(s_play_task);
}

void recorder_play_stop(void)
{
    atomic_store(&s_play_req, 0);
    atomic_store(&s_play_stop, true);
    xTaskNotifyGive(s_play_task);
}

void recorder_play_pause(bool paused)
{
    if (!paused && atomic_load(&s_play_block) >= atomic_load(&s_play_blocks)) {
        atomic_store(&s_play_seek, 0);   /* from the end: start over */
    }
    atomic_store(&s_play_paused, paused);
    xTaskNotifyGive(s_play_task);
}

void recorder_play_seek(uint32_t block)
{
    atomic_store(&s_play_seek, (int)block);
    xTaskNotifyGive(s_play_task);
}

void recorder_play_speed(int pct)
{
    atomic_store(&s_play_speed, pct);
}

void recorder_play_status(rec_play_t *out)
{
    *out = (rec_play_t){
        .first = atomic_load(&s_play_first),
        .block = atomic_load(&s_play_block),
        .blocks = atomic_load(&s_play_blocks),
        .paused = atomic_load(&s_play_paused),
        .speed = atomic_load(&s_play_speed),
    };
}

void recorder_set_time(int64_t unix_time, int32_t utc_offset_s)
{
    struct timeval tv = { .tv_sec = (time_t)unix_time };
    settimeofday(&tv, NULL);
    settings_set_utc_offset(utc_offset_s);   /* kept across restarts, unlike the clock */
    atomic_store(&s_time_set, true);
    ESP_LOGI(TAG, "clock set (UTC%+d min)", (int)(utc_offset_s / 60));
}

int64_t recorder_time(void)
{
    return atomic_load(&s_time_set) ? (int64_t)time(NULL) : 0;
}

int32_t recorder_utc_offset(void)
{
    return settings_utc_offset();
}
