#include "rec_ring.h"

#include <string.h>

void rec_ring_init(rec_ring_t *r, uint8_t *mem, uint32_t capacity)
{
    memset(r, 0, sizeof(*r));
    r->mem = mem;
    r->capacity = capacity;
}

static void put_block(rec_ring_t *r)
{
    uint8_t *b = r->mem + (size_t)r->head * REC_BLOCK_BYTES;
    uint16_t pred = (uint16_t)r->enc.pred;
    b[0] = (uint8_t)pred;
    b[1] = (uint8_t)(pred >> 8);
    b[2] = (uint8_t)r->enc.index;
    b[3] = 0;
    adpcm_encode_block(&r->enc, r->pending, REC_BLOCK_SAMPLES, b + REC_BLOCK_HEADER);
    r->head = (r->head + 1) % r->capacity;
    r->written++;
    if (r->count < r->capacity) {
        r->count++;
    }
}

void rec_ring_feed(rec_ring_t *r, const int16_t *pcm, size_t n)
{
    while (n) {
        size_t take = REC_BLOCK_SAMPLES - r->pending_n;
        if (take > n) {
            take = n;
        }
        memcpy(r->pending + r->pending_n, pcm, take * sizeof(*pcm));
        r->pending_n += take;
        pcm += take;
        n -= take;
        if (r->pending_n == REC_BLOCK_SAMPLES) {
            put_block(r);
            r->pending_n = 0;
        }
    }
}

uint32_t rec_ring_copy(const rec_ring_t *r, uint8_t *out)
{
    rec_span_t s = rec_ring_span(r);
    rec_ring_copy_span(r, s, 0, s.count, out);
    return s.count;
}

rec_span_t rec_ring_span(const rec_ring_t *r)
{
    return (rec_span_t){ .first = r->written - r->count, .count = r->count };
}

int rec_ring_copy_span(const rec_ring_t *r, rec_span_t s, uint32_t at, uint32_t n, uint8_t *out)
{
    if (at > s.count || n > s.count - at) {
        return -1;
    }
    /* How many blocks back from the newest the piece starts; it's all still
     * held if that's no more than count. Unsigned, so written may wrap. */
    uint32_t age = r->written - (s.first + at);
    if (age > r->count || n > age) {
        return -1;
    }
    uint32_t start = (r->head + r->capacity - age) % r->capacity;
    uint32_t first = r->capacity - start < n ? r->capacity - start : n;
    memcpy(out, r->mem + (size_t)start * REC_BLOCK_BYTES, (size_t)first * REC_BLOCK_BYTES);
    memcpy(out + (size_t)first * REC_BLOCK_BYTES, r->mem, (size_t)(n - first) * REC_BLOCK_BYTES);
    return 0;
}

void rec_block_decode(const uint8_t *block, int16_t *pcm)
{
    adpcm_t s = {
        .pred = (int16_t)(block[0] | block[1] << 8),
        .index = (int8_t)(block[2] > 88 ? 88 : block[2]),
    };
    adpcm_decode_block(&s, block + REC_BLOCK_HEADER, REC_BLOCK_SAMPLES, pcm);
}
