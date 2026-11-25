#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "rec_store.h"

/*
 * Keeps the last CONFIG_REC_SECONDS from the mics. Save keeps those, and
 * that's all; Record keeps them and goes on recording until it's stopped (or
 * CONFIG_REC_KEEP_MAX_S), writing a part to flash each time the ring has
 * nearly turned over, so a recording can be longer than the ring. Also plays
 * recordings, and holds the clock the laptop sets. Every function here may be
 * called from any task.
 */

typedef enum {
    REC_SAVE_NONE,
    REC_SAVE_BUSY,        /* Save: writing what was held */
    REC_SAVE_RECORDING,   /* Record: from the press until it's stopped */
    REC_SAVE_DONE,
    REC_SAVE_FAILED,
    REC_SAVE_FULL,        /* every clip is in a kept recording */
} rec_save_state_t;

/* audio_init() first. */
esp_err_t recorder_start(void);

void recorder_set_paused(bool paused);
bool recorder_paused(void);

/* The mics' loudness over the last 20 ms, 0 to 1. */
float recorder_level(void);

/* Seconds a save would keep: what's held since the last save ended, up to
 * CONFIG_REC_SECONDS. */
uint32_t recorder_seconds(void);

/* Save: what's held since the last save, as a recording of its own; while
 * recording, it stops (what's being saved is the recording). False if
 * nothing happened: under a second new, or a save already going. */
bool recorder_save(void);
/* Keep recording: the last save, carried on from where it ended as one
 * recording, while the ring still holds that point (nearly a ring's length). */
bool recorder_can_continue(void);
void recorder_continue(void);
uint32_t recorder_saved_ms(void);   /* since the last save was done; UINT32_MAX before one */
/* The save sound at the speaker's volume, sounds on or off: to hear it. */
void recorder_beep(void);
void recorder_warn(void);   /* the low pair, if sounds are on: the battery warning */
rec_save_state_t recorder_save_state(void);
uint32_t recorder_recording_seconds(void);   /* the one going on, from the press on */
uint32_t recorder_recording(void);           /* its first part's seq, or 0 */

/* The last recording started, by its first part; false before the first. */
bool recorder_last_saved(uint32_t *slot, uint32_t *seq);

/* Ends a recording going on, or with save_idle saves what's held, and
 * waits up to timeout_ms for it to reach flash: before the power goes. */
void recorder_finish(bool save_idle, uint32_t timeout_ms);

/* Changes whenever a clip is saved, named or deleted. */
uint32_t recorder_changes(void);

uint32_t recorder_slots(void);
/* The clips partition's flash past its last slot, which no recording uses
 * (the battery log keeps its entries there). False before recorder_start(). */
bool recorder_spare(const esp_partition_t **part, uint32_t *off, uint32_t *len);
int recorder_list(rec_clip_t *out, int max);
int recorder_clip(uint32_t slot, rec_clip_t *out);   /* 0 if the slot has a clip */
int recorder_read(uint32_t slot, uint32_t off, void *buf, uint32_t len);
int recorder_delete(uint32_t slot);
int recorder_set_name(uint32_t slot, uint32_t seq, const char *name);   /* rec_store_set_name() */
int recorder_set_kept(uint32_t slot, uint32_t seq, bool kept);          /* rec_store_set_kept() */

/* The same for whole recordings, by their first part's seq. */
int recorder_recordings(rec_recording_t *out, int max);
int recorder_delete_recording(uint32_t first);
int recorder_name_recording(uint32_t first, const char *name);
int recorder_protect_recording(uint32_t first, bool kept);   /* no save replaces it */
int recorder_set_pulled(uint32_t slot, uint32_t seq);           /* rec_store_set_pulled() */
/* Deletes every recording pull.py downloaded that isn't protected; how many. */
int recorder_delete_downloaded(void);
/* Deletes every recording that isn't protected or still being recorded; how many. */
int recorder_delete_all(void);

/* Puts back the recording deleted last, if no save has taken its slots
 * since; -1 if all or part of it couldn't come back. */
int recorder_undo_delete(void);

/* Playback of a recording, from the start. */
void recorder_play(uint32_t first);
void recorder_play_stop(void);
void recorder_play_pause(bool paused);
void recorder_play_seek(uint32_t block);   /* from the recording's start */
void recorder_play_speed(int pct);         /* 100, 150, 200: same pitch */

typedef struct {
    uint32_t first;    /* 0 if nothing is playing */
    uint32_t block;    /* where it's got to */
    uint32_t blocks;
    bool paused;
    int speed;
} rec_play_t;

void recorder_play_status(rec_play_t *out);

/* The clock exists only once the laptop sets it (tools/pull.py), and only
 * until the power goes: the board has no RTC. */
void recorder_set_time(int64_t unix_time, int32_t utc_offset_s);
int64_t recorder_time(void);   /* 0 if not set */
int32_t recorder_utc_offset(void);   /* the last one set, kept in NVS */
