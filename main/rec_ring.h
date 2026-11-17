#pragma once

#include <stddef.h>
#include <stdint.h>

#include "adpcm.h"

/*
 * The last few minutes of audio as a ring of ADPCM blocks. Each block is
 * 20 ms and carries the encoder state it started from, so any run of blocks
 * decodes on its own: a copy of the ring is a clip. No IDF in here, so the
 * host tests compile it.
 *
 * A block on flash and in the ring: pred (int16, little-endian), index
 * (uint8), a zero byte, then REC_BLOCK_SAMPLES 4-bit codes, two to a byte,
 * the first sample in the low nibble.
 */

#define REC_RATE 16000
#define REC_BLOCK_SAMPLES 320
#define REC_BLOCKS_PER_S (REC_RATE / REC_BLOCK_SAMPLES)
#define REC_BLOCK_HEADER 4
#define REC_BLOCK_BYTES (REC_BLOCK_HEADER + REC_BLOCK_SAMPLES / 2)

typedef struct {
    uint8_t *mem;        /* capacity * REC_BLOCK_BYTES */
    uint32_t capacity;   /* blocks */
    uint32_t head;       /* where the next block goes */
    uint32_t count;      /* blocks held, up to capacity */
    uint32_t written;    /* blocks ever added, wrapping */
    adpcm_t enc;
    int16_t pending[REC_BLOCK_SAMPLES];
    size_t pending_n;
} rec_ring_t;

void rec_ring_init(rec_ring_t *r, uint8_t *mem, uint32_t capacity);

/* Any number of samples; a block is added each time REC_BLOCK_SAMPLES are in. */
void rec_ring_feed(rec_ring_t *r, const int16_t *pcm, size_t n);

/* Copies the blocks held, oldest first, to out (room for capacity blocks).
 * Returns how many. */
uint32_t rec_ring_copy(const rec_ring_t *r, uint8_t *out);

/* The blocks held at one moment, to copy a piece at a time while recording
 * goes on: a save then holds the ring's lock for one piece, not a minute's
 * worth of PSRAM copying. */
typedef struct {
    uint32_t first;   /* in written's numbering */
    uint32_t count;
} rec_span_t;

rec_span_t rec_ring_span(const rec_ring_t *r);

/* The blocks held that were written after since (in written's numbering):
 * what a save keeps when the one before it ended at since. */
rec_span_t rec_ring_span_since(const rec_ring_t *r, uint32_t since);

/* Copies n of the span's blocks, from its block at, to out. -1 if recording
 * has overwritten any of them since. */
int rec_ring_copy_span(const rec_ring_t *r, rec_span_t s, uint32_t at, uint32_t n, uint8_t *out);

/* One block back to REC_BLOCK_SAMPLES samples. */
void rec_block_decode(const uint8_t *block, int16_t *pcm);
