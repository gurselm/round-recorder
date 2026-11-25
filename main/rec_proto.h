#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rec_store.h"

/*
 * The laptop's side of the USB serial port (tools/pull.py), one command per
 * line. Every reply line starts with "RR " and comes as one write, with a
 * newline before and after, so a log line can't run into it:
 *
 *   RR HELLO                 RR HELLO round-recorder <version> slots=<n> seconds=<s> time=<unix> recording=<first>
 *                                                recording: the one still going on, by its first part, or 0
 *   RR TIME <unix> <utc_s>   RR OK              sets the clock, and the offset the screen shows
 *   RR LIST                  RR CLIP slot=<n> seq=<n> first=<n> time=<unix> blocks=<n> crc=<hex> kept=<0|1>
 *                                                pulled=<0|1> name=<rest> ...
 *                            RR END <count>      newest first; first is the recording's first part
 *                                                (rec_store.h); the name runs to the end of the line
 *   RR GET <slot>            RR DATA <base64 of up to REC_PROTO_CHUNK bytes> ...
 *                            RR END <bytes> <crc>
 *   RR DEL <slot>            RR OK
 *   RR NAME <slot> <seq> <name>                 RR OK   the name is the rest of the line; none clears it
 *   RR KEEP <slot> <seq> <0|1>                  RR OK   1: no save replaces it (rec_store.h)
 *   RR PULLED <slot> <seq>                      RR OK   downloaded: the screen shows it
 *   RR TAP <x> <y> [ms]      RR OK              presses the screen there (ui_tap), for checking flows
 *   RR SNAP [screen]         RR SNAP <w> <h> rgb565, RR DATA ..., RR END <bytes> <crc>
 *                                                the screen (ui.h's ui_snapshot), for tools/snap.py
 *   RR BATT                  RR B <minute> <mV> <percent> <flags> ...
 *                            RR END <count>      the battery log (batt_log.h), oldest first
 *
 * Failures answer "RR ERR <why>". No IDF in here: the store and the port come
 * as callbacks, which take any locks they need.
 */

#define REC_PROTO_CHUNK 3072

typedef struct {
    const char *version;
    uint32_t slots;
    uint32_t seconds;
    int (*list)(rec_clip_t *out, int max);
    int (*clip)(uint32_t slot, rec_clip_t *out);   /* 0 if the slot has a clip */
    int (*read)(uint32_t slot, uint32_t off, void *buf, uint32_t len);
    int (*del)(uint32_t slot);
    int (*set_name)(uint32_t slot, uint32_t seq, const char *name);   /* rec_store_set_name() */
    int (*set_kept)(uint32_t slot, uint32_t seq, bool kept);          /* rec_store_set_kept() */
    int (*set_pulled)(uint32_t slot, uint32_t seq);                   /* rec_store_set_pulled() */
    void (*set_time)(int64_t unix_time, int32_t utc_offset_s);
    int64_t (*time)(void);                         /* 0 if not set */
    uint32_t (*recording)(void);                   /* the recording going on, or 0 */
    /* The screen as w * h RGB565 pixels in a buffer it malloc'd, which the
     * command frees; NULL, or nonzero back, for none. */
    int (*snap)(const char *screen, uint8_t **rgb565, uint32_t *w, uint32_t *h);
    void (*tap)(int x, int y, int ms);   /* NULL: none */
    /* The battery log's entry i: 0 if there is one. NULL: no log. */
    int (*batt)(uint32_t i, uint32_t *minute, uint32_t *mv, uint32_t *pct, uint32_t *flags);
    void (*out)(const char *line, size_t n);
} rec_proto_t;

/* Handles one line, without its newline. */
void rec_proto_line(const rec_proto_t *p, const char *line);
