#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Plays speech faster without raising its pitch: WSOLA (waveform-similarity
 * overlap-add) at 16 kHz. It cuts the input into 40 ms frames, picks each
 * one within 10 ms of where the speed puts it so it lines up with the last,
 * and overlap-adds them half a frame apart. At 100 % it passes samples
 * through untouched. No IDF in here, so the host tests compile it.
 */

#define TEMPO_HOP 320                  /* output samples per frame step: 20 ms */
#define TEMPO_SEARCH 160               /* how far a frame may move to line up: 10 ms */
#define TEMPO_BUF 4096                 /* input samples held */

typedef struct {
    int speed;                         /* percent */
    int16_t in[TEMPO_BUF];
    size_t n;                          /* samples in in[] */
    double pos;                        /* where the next frame would start at this speed */
    long prev;                         /* where the last frame started; -1 before the first */
    float tail[TEMPO_HOP];             /* the last frame's second half, windowed */
} tempo_t;

/* Also starts again, dropping what's buffered: after a seek. */
void tempo_init(tempo_t *t, int speed_pct);

/* Adds input; false if it doesn't fit (take output first). */
bool tempo_put(tempo_t *t, const int16_t *in, size_t n);

/* Up to max output samples; 0 once it needs more input. */
size_t tempo_get(tempo_t *t, int16_t *out, size_t max);
