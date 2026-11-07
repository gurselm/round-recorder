#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Saved clips in a raw flash partition, one per fixed-size slot: a 4 KB
 * header sector, then the clip's ADPCM blocks (rec_ring.h). A save erases the
 * slot, writes the blocks, then the header, so a save cut short by power loss
 * leaves no clip rather than a broken one. No IDF in here: the flash comes as
 * callbacks.
 *
 * A recording longer than a slot is saved as parts, one clip each, all
 * naming the recording's first part; together they're one stretch of audio.
 * A new save takes an empty slot, else the oldest clip whose recording isn't
 * kept: a recording's first part carries a keep flag.
 *
 * A clip's name and flags (kept, downloaded) live in the rest of its header
 * sector, which the save left erased: a log of 64-byte records, the last good
 * one winning. Changing them then writes erased flash only, never erases, and
 * a power cut mid-write leaves what it had. At its 2 KB mark, the save writes
 * one record of its own: the clip's loudness shape, and when it was saved by
 * the board's count since boot.
 */

#define REC_SECTOR 4096u
#define REC_ERASE_UNIT 65536u   /* slots are whole 64 KB blocks, which erase fastest */
#define REC_MAGIC 0x43455252u   /* "RREC" */
#define REC_VERSION 1
#define REC_NAME_MAX 48         /* bytes of UTF-8 */
#define REC_MAX_SLOTS 256
#define REC_MAX_PARTS 16
#define REC_FULL -2             /* rec_store_save(): every clip is in a kept recording */
#define REC_ENV_N 48            /* points in a clip's loudness shape */

typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t off, uint32_t len);   /* sector aligned */
    void *ctx;
    uint32_t size;
} rec_flash_t;

/* At the start of a slot. 40 bytes, no padding on the host or the S3. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t block_bytes;
    uint32_t seq;          /* one more than the last save's; 0 is never used */
    uint32_t blocks;
    uint32_t rate;
    uint32_t first_seq;    /* the recording's first part; 0 if this is it */
    int64_t unix_time;     /* when it was saved; 0 if the clock wasn't set */
    uint32_t data_crc;     /* CRC-32 (zlib's) of the blocks */
    uint32_t header_crc;   /* of the bytes before it */
} rec_header_t;

typedef struct {
    uint32_t slot;
    uint32_t seq;
    uint32_t first;        /* the recording's first part: seq itself if this is it */
    uint32_t blocks;
    int64_t unix_time;
    uint32_t data_crc;
    char name[REC_NAME_MAX + 1];   /* "" if it has none */
    bool kept;
    bool pulled;                   /* downloaded by pull.py */
    uint8_t env[REC_ENV_N];        /* loudness over its length, 0 to 88; zeros if not known */
    uint32_t boot, uptime_s;       /* saved in that boot, that long after it began */
} rec_clip_t;

/* A recording: its parts in order. Its name, time and keep flag are its first
 * part's. */
typedef struct {
    uint32_t first;
    uint32_t parts;
    uint32_t slots[REC_MAX_PARTS];
    uint32_t blocks;       /* all parts' */
    int64_t unix_time;
    bool whole;            /* its first part is still there */
    bool kept;
    bool pulled;
    uint32_t boot, uptime_s;
    uint8_t env[REC_ENV_N];   /* rec_group_envelope() fills it; rec_group() leaves zeros */
    char name[REC_NAME_MAX + 1];
} rec_recording_t;

/* What rec_store_restore() needs to put a deleted clip back. */
typedef struct {
    uint32_t slot;
    rec_header_t header;
    char name[REC_NAME_MAX + 1];
    uint8_t flags;
    uint8_t env_rec[64];
} rec_deleted_t;

typedef struct {
    rec_flash_t f;
    uint32_t max_blocks;
    uint32_t slot_bytes;
    uint32_t slots;
    uint32_t next_seq;
} rec_store_t;

uint32_t rec_crc32(uint32_t crc, const void *data, size_t len);

/* Lays out slots for clips of up to max_blocks and finds the next seq. -1 if
 * not even one slot fits. */
int rec_store_open(rec_store_t *s, const rec_flash_t *f, uint32_t max_blocks);

/* Saved clips, newest first. Returns how many (up to max). */
int rec_store_list(rec_store_t *s, rec_clip_t *out, int max);

/* The clip in a slot; false if the slot holds none. */
bool rec_store_clip(rec_store_t *s, uint32_t slot, rec_clip_t *out);

/* Saves n blocks: a new recording with first_seq 0, else the next part of
 * the recording whose first part is first_seq. -1 on a flash failure,
 * REC_FULL if every slot holds part of a kept recording (or of this one). */
int rec_store_save(rec_store_t *s, const uint8_t *blocks, uint32_t n, int64_t unix_time, uint32_t first_seq,
                   uint32_t boot, uint32_t uptime_s, rec_clip_t *saved);

/* Clips (newest first, as rec_store_list gives them) as recordings, newest
 * first. Returns how many (up to max). */
int rec_group(const rec_clip_t *clips, int n, rec_recording_t *out, int max);

/* A recording's loudness shape from its parts' (in clips, as rec_group took
 * them), each over its share of the whole. */
void rec_group_envelope(const rec_recording_t *r, const rec_clip_t *clips, int n, uint8_t *env);

/* Names the clip seq in slot, or with "" clears its name. -1 if the slot no
 * longer holds that clip, the name is too long or has control characters,
 * or the clip has been renamed so often (about 60 times) its log is full. */
int rec_store_set_name(rec_store_t *s, uint32_t slot, uint32_t seq, const char *name);

/* Keeps the clip, or lets a save replace it again; -1 as for set_name. It
 * counts for a whole recording on its first part. */
int rec_store_set_kept(rec_store_t *s, uint32_t slot, uint32_t seq, bool kept);

/* Marks the clip downloaded (pull.py); -1 as for set_name. */
int rec_store_set_pulled(rec_store_t *s, uint32_t slot, uint32_t seq);

/* Reads a clip's block bytes from off. */
int rec_store_read(rec_store_t *s, uint32_t slot, uint32_t off, void *buf, uint32_t len);

int rec_store_delete(rec_store_t *s, uint32_t slot);

/* Deletes as rec_store_delete does, which erases only the header sector, and
 * keeps in out what rec_store_restore() needs to put it back. */
int rec_store_delete_undoable(rec_store_t *s, uint32_t slot, rec_deleted_t *out);

/* Puts a deleted clip back, with its name and keep flag; -1 if its slot isn't
 * as the delete left it (a save has taken it since) or its blocks don't
 * match their CRC any more. */
int rec_store_restore(rec_store_t *s, const rec_deleted_t *d);
