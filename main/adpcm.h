#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * IMA ADPCM (the IMA/DVI standard): each 16-bit sample becomes a 4-bit code,
 * the difference from a prediction in units of a step that grows and
 * shrinks with the sound. Encoder and decoder must start from the same
 * state and see the same codes in the same order; tools/pull.py decodes the
 * same way.
 */

typedef struct {
    int16_t pred;    /* the last sample, as the decoder will have it */
    int8_t index;    /* into the step table, 0 to 88 */
} adpcm_t;

/* n samples (even) to or from n/2 bytes, the first sample of each pair in the low nibble. */
void adpcm_encode_block(adpcm_t *s, const int16_t *pcm, size_t n, uint8_t *out);
void adpcm_decode_block(adpcm_t *s, const uint8_t *in, size_t n, int16_t *pcm);
