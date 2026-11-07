#include "tempo.h"

#include <math.h>
#include <string.h>

#define FRAME (2 * TEMPO_HOP)

/* The rising half of a Hann window; the falling half is 1 minus it, so two
 * frames half a frame apart always add up to the original level. */
static float rise(int i)
{
    float s = sinf((float)M_PI * (float)i / (float)FRAME);
    return s * s;
}

void tempo_init(tempo_t *t, int speed_pct)
{
    memset(t, 0, sizeof(*t));
    t->speed = speed_pct < 50 ? 50 : speed_pct > 300 ? 300 : speed_pct;
    t->pos = TEMPO_SEARCH;   /* room to look back before the first frame */
    t->prev = -1;
}

bool tempo_put(tempo_t *t, const int16_t *in, size_t n)
{
    if (n > TEMPO_BUF - t->n) {
        return false;
    }
    memcpy(t->in + t->n, in, n * sizeof(*in));
    t->n += n;
    return true;
}

/* Drops input no later frame can reach. */
static void compact(tempo_t *t)
{
    long keep = (long)t->pos - TEMPO_SEARCH;
    if (t->prev >= 0 && t->prev + TEMPO_HOP < keep) {
        keep = t->prev + TEMPO_HOP;
    }
    if (keep < TEMPO_BUF / 2) {
        return;   /* not worth the copy yet */
    }
    memmove(t->in, t->in + keep, (t->n - (size_t)keep) * sizeof(t->in[0]));
    t->n -= (size_t)keep;
    t->pos -= (double)keep;
    t->prev -= keep;
}

static size_t passthrough(tempo_t *t, int16_t *out, size_t max)
{
    size_t k = t->n < max ? t->n : max;
    memcpy(out, t->in, k * sizeof(*out));
    memmove(t->in, t->in + k, (t->n - k) * sizeof(t->in[0]));
    t->n -= k;
    return k;
}

size_t tempo_get(tempo_t *t, int16_t *out, size_t max)
{
    if (t->speed == 100) {
        return passthrough(t, out, max);
    }
    size_t made = 0;
    while (max - made >= TEMPO_HOP) {
        long ideal = (long)t->pos;
        if ((size_t)(ideal + TEMPO_SEARCH + FRAME) > t->n) {
            break;   /* needs more input */
        }
        long at = ideal;
        if (t->prev >= 0) {
            /* The frame start, within the search range, whose first half
             * best matches what would have followed the last frame. Every
             * fourth sample is plenty to line up the waveform. */
            const int16_t *want = t->in + t->prev + TEMPO_HOP;
            float best = -INFINITY;
            for (long d = -TEMPO_SEARCH; d <= TEMPO_SEARCH; d++) {
                const int16_t *x = t->in + ideal + d;
                float c = 0;
                for (int i = 0; i < TEMPO_HOP; i += 4) {
                    c += (float)x[i] * (float)want[i];
                }
                if (c > best) {
                    best = c;
                    at = ideal + d;
                }
            }
        }
        for (int i = 0; i < TEMPO_HOP; i++) {
            float y = t->tail[i] + rise(i) * (float)t->in[at + i];
            out[made + (size_t)i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
            t->tail[i] = (1.0f - rise(i)) * (float)t->in[at + TEMPO_HOP + i];
        }
        made += TEMPO_HOP;
        t->prev = at;
        t->pos += TEMPO_HOP * t->speed / 100.0;
        compact(t);
    }
    return made;
}
