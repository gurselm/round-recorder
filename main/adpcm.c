#include "adpcm.h"

/* The standard's 89 steps, each about 1.1 times the one before. */
static const int16_t STEP[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

/* How the step moves after each code, by its size (the sign bit aside):
 * small codes shrink it, large ones grow it fast. */
static const int8_t STEP_MOVE[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Applies one code to the state, as both sides do, and returns the sample. */
static int16_t apply(adpcm_t *s, uint8_t code)
{
    int32_t step = STEP[s->index];
    int32_t delta = step >> 3;
    if (code & 4) {
        delta += step;
    }
    if (code & 2) {
        delta += step >> 1;
    }
    if (code & 1) {
        delta += step >> 2;
    }
    int32_t v = (code & 8) ? s->pred - delta : s->pred + delta;
    s->pred = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    int32_t i = s->index + STEP_MOVE[code & 7];
    s->index = (int8_t)(i < 0 ? 0 : i > 88 ? 88 : i);
    return s->pred;
}

/* The code whose delta comes closest to the difference: the sign, then the
 * step's multiples 1, 1/2 and 1/4 taken off while they fit. */
static uint8_t quantize(const adpcm_t *s, int16_t sample)
{
    int32_t diff = (int32_t)sample - s->pred;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int32_t step = STEP[s->index];
    for (uint8_t bit = 4; bit; bit >>= 1, step >>= 1) {
        if (diff >= step) {
            code |= bit;
            diff -= step;
        }
    }
    return code;
}

void adpcm_encode_block(adpcm_t *s, const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t lo = quantize(s, pcm[i]);
        apply(s, lo);
        uint8_t hi = quantize(s, pcm[i + 1]);
        apply(s, hi);
        out[i / 2] = (uint8_t)(lo | hi << 4);
    }
}

void adpcm_decode_block(adpcm_t *s, const uint8_t *in, size_t n, int16_t *pcm)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        pcm[i] = apply(s, in[i / 2] & 15);
        pcm[i + 1] = apply(s, in[i / 2] >> 4);
    }
}
