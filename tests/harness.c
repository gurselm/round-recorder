/*
 * Host harness for the IDF-free core (rec_ring, rec_store, rec_proto), driven
 * by test_core.py:
 *
 *   harness ring CAP CHUNK IN.raw OUT.blocks   feed IN in CHUNK-sample reads, write the ring's copy
 *   harness span                                piecewise copies while recording, then "span ok"
 *   harness decode IN.blocks OUT.raw
 *   harness tempo PCT IN.raw OUT.raw           IN at PCT % speed, as playback does it
 *   harness multitap KEY@MS|mode=N ...         the text those keypad presses type
 *   harness crc FILE
 *   harness hours START PCT HOURS              the hours left shown after HOURS from START % to PCT %
 *   harness store                              store checks; prints "store ok"
 *   harness device DIR [REC]                   a fake board on stdin/stdout with five clips (the
 *                                              last two one recording), each one's blocks also
 *                                              written to DIR/<seq>.blocks; REC: the recording it
 *                                              says is still going on
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "batt_est.h"
#include "rec_proto.h"
#include "multitap.h"
#include "rec_ring.h"
#include "tempo.h"
#include "rec_store.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* NOR flash in RAM: erase sets 0xFF, and a write may only land on erased bytes. */
typedef struct {
    uint8_t *mem;
    uint32_t size;
    int fail_writes_after;   /* < 0: never */
} ram_flash_t;

static int ram_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    ram_flash_t *f = ctx;
    CHECK(off <= f->size && len <= f->size - off);
    memcpy(buf, f->mem + off, len);
    return 0;
}

static int ram_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    ram_flash_t *f = ctx;
    CHECK(off <= f->size && len <= f->size - off);
    if (f->fail_writes_after == 0) {
        return -1;
    }
    if (f->fail_writes_after > 0) {
        f->fail_writes_after--;
    }
    for (uint32_t i = 0; i < len; i++) {
        CHECK(f->mem[off + i] == 0xFF);   /* written without an erase */
    }
    memcpy(f->mem + off, buf, len);
    return 0;
}

static int ram_erase(void *ctx, uint32_t off, uint32_t len)
{
    ram_flash_t *f = ctx;
    CHECK(off % REC_SECTOR == 0 && len % REC_SECTOR == 0 && off <= f->size && len <= f->size - off);
    memset(f->mem + off, 0xFF, len);
    return 0;
}

static rec_flash_t ram_flash(ram_flash_t *f, uint32_t size)
{
    f->mem = malloc(size);
    f->size = size;
    f->fail_writes_after = -1;
    memset(f->mem, 0xFF, size);
    return (rec_flash_t){ .read = ram_read, .write = ram_write, .erase = ram_erase, .ctx = f, .size = size };
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    CHECK(fp);
    fseek(fp, 0, SEEK_END);
    *n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(*n + 1);
    CHECK(fread(buf, 1, *n, fp) == *n);
    fclose(fp);
    return buf;
}

static void spit(const char *path, const void *buf, size_t n)
{
    FILE *fp = fopen(path, "wb");
    CHECK(fp && fwrite(buf, 1, n, fp) == n);
    fclose(fp);
}

/* Blocks of a tone, through the ring as the board makes them. */
static uint32_t tone_blocks(uint8_t *out, uint32_t blocks, float hz)
{
    uint8_t *mem = malloc((size_t)blocks * REC_BLOCK_BYTES);
    rec_ring_t r;
    rec_ring_init(&r, mem, blocks);
    int16_t pcm[REC_BLOCK_SAMPLES];
    for (uint32_t b = 0; b < blocks; b++) {
        for (int i = 0; i < REC_BLOCK_SAMPLES; i++) {
            float t = (float)(b * REC_BLOCK_SAMPLES + i) / REC_RATE;
            pcm[i] = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * hz * t));
        }
        rec_ring_feed(&r, pcm, REC_BLOCK_SAMPLES);
    }
    uint32_t n = rec_ring_copy(&r, out);
    free(mem);
    return n;
}

static int cmd_ring(int argc, char **argv)
{
    CHECK(argc == 6);
    uint32_t cap = (uint32_t)atoi(argv[2]);
    size_t chunk = (size_t)atoi(argv[3]), n;
    int16_t *pcm = (int16_t *)slurp(argv[4], &n);
    n /= 2;
    uint8_t *mem = malloc((size_t)cap * REC_BLOCK_BYTES), *out = malloc((size_t)cap * REC_BLOCK_BYTES);
    rec_ring_t r;
    rec_ring_init(&r, mem, cap);
    for (size_t i = 0; i < n; i += chunk) {
        rec_ring_feed(&r, pcm + i, n - i < chunk ? n - i : chunk);
    }
    uint32_t got = rec_ring_copy(&r, out);
    CHECK(got == (n / REC_BLOCK_SAMPLES < cap ? n / REC_BLOCK_SAMPLES : cap));
    spit(argv[5], out, (size_t)got * REC_BLOCK_BYTES);
    return 0;
}

/* Blocks of sawtooth, no two alike, so a misplaced block shows. */
static void feed_blocks(rec_ring_t *r, uint32_t n)
{
    static uint32_t k;
    int16_t pcm[REC_BLOCK_SAMPLES];
    for (uint32_t b = 0; b < n; b++, k++) {
        for (int i = 0; i < REC_BLOCK_SAMPLES; i++) {
            pcm[i] = (int16_t)((int)((k * 37 + (uint32_t)i * 11) % 2000) - 1000);
        }
        rec_ring_feed(r, pcm, REC_BLOCK_SAMPLES);
    }
}

/* A save's piecewise copy, with recording going on between the pieces. */
static int cmd_span(void)
{
    enum { CAP = 100, PIECE = 7, BETWEEN = 3 };
    static uint8_t mem[CAP * REC_BLOCK_BYTES], want[CAP * REC_BLOCK_BYTES], got[CAP * REC_BLOCK_BYTES];
    for (int wrap = 0; wrap < 2; wrap++) {
        for (uint32_t held = 40; held <= 250; held += 210) {
            rec_ring_t r;
            rec_ring_init(&r, mem, CAP);
            r.written = wrap ? UINT32_MAX - 60 : 0;   /* the counter wraps mid-test */
            feed_blocks(&r, held);
            rec_span_t s = rec_ring_span(&r);
            CHECK(s.count == (held < CAP ? held : CAP));
            CHECK(rec_ring_copy(&r, want) == s.count);
            for (uint32_t at = 0; at < s.count; at += PIECE) {
                uint32_t n = s.count - at < PIECE ? s.count - at : PIECE;
                CHECK(rec_ring_copy_span(&r, s, at, n, got + (size_t)at * REC_BLOCK_BYTES) == 0);
                feed_blocks(&r, BETWEEN);
            }
            CHECK(!memcmp(want, got, (size_t)s.count * REC_BLOCK_BYTES));
        }
    }
    /* Overwritten blocks are refused, the rest still copy. */
    rec_ring_t r;
    rec_ring_init(&r, mem, CAP);
    feed_blocks(&r, CAP);
    rec_span_t s = rec_ring_span(&r);
    feed_blocks(&r, 5);
    CHECK(rec_ring_copy_span(&r, s, 0, 1, got) == -1);
    CHECK(rec_ring_copy_span(&r, s, 4, 2, got) == -1);
    CHECK(rec_ring_copy_span(&r, s, 5, CAP - 5, got) == 0);
    CHECK(rec_ring_copy_span(&r, s, 5, CAP - 4, got) == -1);   /* past the span */

    /* A save keeps only what's new since the one before; never more than
     * the ring holds; nothing if nothing came since. */
    for (int wrap = 0; wrap < 2; wrap++) {
        rec_ring_init(&r, mem, CAP);
        r.written = wrap ? UINT32_MAX - 30 : 0;
        uint32_t start = r.written;
        feed_blocks(&r, 60);
        rec_span_t all = rec_ring_span_since(&r, start);   /* nothing saved yet: all of it */
        CHECK(all.count == 60 && all.first == start);
        uint32_t saved_end = all.first + all.count;
        CHECK(rec_ring_span_since(&r, saved_end).count == 0);
        feed_blocks(&r, 25);
        rec_span_t fresh = rec_ring_span_since(&r, saved_end);
        CHECK(fresh.first == saved_end && fresh.count == 25);
        CHECK(rec_ring_copy_span(&r, fresh, 0, fresh.count, got) == 0);
        feed_blocks(&r, 3 * CAP);   /* long since the last save: capped at what's held */
        fresh = rec_ring_span_since(&r, saved_end);
        CHECK(fresh.count == CAP && fresh.first == r.written - CAP);
    }
    printf("span ok\n");
    return 0;
}

static int cmd_decode(int argc, char **argv)
{
    CHECK(argc == 4);
    size_t n;
    uint8_t *in = slurp(argv[2], &n);
    CHECK(n % REC_BLOCK_BYTES == 0);
    size_t blocks = n / REC_BLOCK_BYTES;
    int16_t *pcm = malloc(blocks * REC_BLOCK_SAMPLES * sizeof(int16_t) + 1);
    for (size_t b = 0; b < blocks; b++) {
        rec_block_decode(in + b * REC_BLOCK_BYTES, pcm + b * REC_BLOCK_SAMPLES);
    }
    spit(argv[3], pcm, blocks * REC_BLOCK_SAMPLES * sizeof(int16_t));
    return 0;
}

static int cmd_tempo(int argc, char **argv)
{
    CHECK(argc == 5);
    static tempo_t t;
    tempo_init(&t, atoi(argv[2]));
    size_t n;
    int16_t *in = (int16_t *)slurp(argv[3], &n);
    n /= 2;
    int16_t *out = malloc(n * 2 * sizeof(int16_t) + TEMPO_HOP * sizeof(int16_t));
    size_t made = 0;
    for (size_t i = 0; i < n; i += REC_BLOCK_SAMPLES) {
        CHECK(tempo_put(&t, in + i, n - i < REC_BLOCK_SAMPLES ? n - i : REC_BLOCK_SAMPLES));
        size_t k;
        while ((k = tempo_get(&t, out + made, TEMPO_HOP)) > 0) {
            made += k;
        }
    }
    spit(argv[4], out, made * sizeof(int16_t));
    return 0;
}

static int cmd_multitap(int argc, char **argv)
{
    multitap_t m;
    multitap_reset(&m);
    char text[64] = "";
    int len = 0;
    for (int i = 2; i < argc; i++) {
        int key, mode;
        long long ms;
        if (sscanf(argv[i], "mode=%d", &mode) == 1) {   /* the mode key: 0 letters, 1 numbers, 2 symbols */
            multitap_set_mode(&m, (multitap_mode_t)mode);
            continue;
        }
        CHECK(sscanf(argv[i], "%d@%lld", &key, &ms) == 2);
        bool replace;
        char c = multitap_press(&m, key, ms, len, &replace);
        CHECK(c);
        if (replace) {
            text[len - 1] = c;
        } else {
            CHECK(len < (int)sizeof(text) - 1);
            text[len++] = c;
            text[len] = '\0';
        }
    }
    printf("[%s]\n", text);
    return 0;
}

static int cmd_hours(int argc, char **argv)
{
    CHECK(argc == 5);
    printf("%.2f\n", batt_hours_left(atoi(argv[2]), atoi(argv[3]), atof(argv[4])));
    return 0;
}

static int cmd_crc(int argc, char **argv)
{
    CHECK(argc == 3);
    size_t n;
    uint8_t *buf = slurp(argv[2], &n);
    printf("%08x\n", (unsigned)rec_crc32(0, buf, n));
    return 0;
}

/* IMA ADPCM over a fixed mix of sounds (a loud tone, noise, a quiet tone
 * with hiss, a clipping square wave, near-silence): the CRC of the codes,
 * then of the samples decoded from them. Any change to the format moves
 * them. */
static int cmd_adpcm(void)
{
    enum { N = 320, BLOCKS = 500 };
    int16_t pcm[N], out[N];
    uint8_t codes[N / 2];
    adpcm_t enc = { 0, 0 };
    uint32_t seed = 12345, crc_codes = 0, crc_pcm = 0;
    for (int k = 0; k < BLOCKS; k++) {
        adpcm_t start = enc;
        for (int i = 0; i < N; i++) {
            double t = (double)(k * N + i) / 16000.0, v;
            seed = seed * 1103515245u + 12345u;
            int noise = (int)((seed >> 8) & 0xFFFF) - 32768;
            switch (k % 5) {
            case 0: v = 30000 * sin(2 * M_PI * 440 * t); break;
            case 1: v = noise; break;
            case 2: v = 8000 * sin(2 * M_PI * 97 * t) + noise / 32; break;
            case 3: v = (i % 40 < 20) ? 40000 : -40000; break;
            default: v = 50 * sin(2 * M_PI * 3000 * t); break;
            }
            pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        adpcm_encode_block(&enc, pcm, N, codes);
        crc_codes = rec_crc32(crc_codes, codes, sizeof(codes));
        adpcm_decode_block(&start, codes, N, out);
        crc_pcm = rec_crc32(crc_pcm, out, sizeof(out));
        CHECK(start.pred == enc.pred && start.index == enc.index);
    }
    printf("%08x %08x\n", (unsigned)crc_codes, (unsigned)crc_pcm);
    return 0;
}

static int cmd_store(void)
{
    enum { MAX = 3000 };   /* 60 s */
    ram_flash_t ram;
    rec_flash_t f = ram_flash(&ram, 5 * 512 * 1024 + 4096);   /* five slots and a bit */
    rec_store_t s;
    CHECK(rec_store_open(&s, &f, MAX) == 0);
    CHECK(s.slot_bytes == 512 * 1024 && s.slots == 5 && s.next_seq == 1);
    rec_clip_t list[8], c;
    CHECK(rec_store_list(&s, list, 8) == 0);

    uint8_t *data = malloc(MAX * REC_BLOCK_BYTES);
    CHECK(tone_blocks(data, MAX, 440) == MAX);

    /* Fill every slot, then one more: the oldest goes. */
    for (int i = 0; i < 6; i++) {
        CHECK(rec_store_save(&s, data, MAX - i, 1000 + i, 0, 0, 0, &c) == 0);
        CHECK(c.seq == (uint32_t)i + 1 && c.blocks == (uint32_t)(MAX - i));
        CHECK(c.slot == (i < 5 ? (uint32_t)i : 0));
    }
    int n = rec_store_list(&s, list, 8);
    CHECK(n == 5 && list[0].seq == 6 && list[4].seq == 2);
    CHECK(list[0].unix_time == 1005 && list[0].data_crc == rec_crc32(0, data, (MAX - 5) * REC_BLOCK_BYTES));
    /* Fewer rows than clips: the newest, wherever their slots are. */
    CHECK(rec_store_list(&s, list, 2) == 2 && list[0].seq == 6 && list[1].seq == 5);

    /* The data reads back, and not past the slot. */
    uint8_t buf[REC_BLOCK_BYTES];
    CHECK(rec_store_read(&s, list[0].slot, REC_BLOCK_BYTES, buf, sizeof(buf)) == 0);
    CHECK(!memcmp(buf, data + REC_BLOCK_BYTES, sizeof(buf)));
    CHECK(rec_store_read(&s, 0, s.slot_bytes - REC_SECTOR, buf, 1) != 0);
    CHECK(rec_store_read(&s, 5, 0, buf, 1) != 0);

    /* Delete frees the slot for the next save; seq keeps rising across a reopen. */
    CHECK(rec_store_delete(&s, 2) == 0 && !rec_store_clip(&s, 2, &c));
    CHECK(rec_store_list(&s, list, 8) == 4);
    CHECK(rec_store_open(&s, &f, MAX) == 0 && s.next_seq == 7);
    CHECK(rec_store_save(&s, data, 50, 0, 0, 0, 0, &c) == 0 && c.slot == 2 && c.seq == 7);

    /* A corrupt header is no clip, and its slot is taken first. */
    ram.mem[1 * s.slot_bytes + 8] ^= 1;
    CHECK(!rec_store_clip(&s, 1, &c) && rec_store_list(&s, list, 8) == 4);
    ram.mem[1 * s.slot_bytes + 8] ^= 1;
    CHECK(rec_store_clip(&s, 1, &c));
    ram.mem[3 * s.slot_bytes + 8] ^= 1;
    CHECK(rec_store_save(&s, data, 10, 0, 0, 0, 0, &c) == 0 && c.slot == 3);

    /* A save whose header write fails (power cut) leaves no clip in that slot. */
    CHECK(rec_store_delete(&s, 4) == 0);
    ram.fail_writes_after = 1;   /* the data goes, the header doesn't */
    CHECK(rec_store_save(&s, data, 10, 0, 0, 0, 0, &c) != 0 && !rec_store_clip(&s, 4, &c));
    ram.fail_writes_after = -1;

    /* Names: set, renamed, cleared; the newest record wins. */
    rec_clip_t n1;
    CHECK(rec_store_clip(&s, 1, &n1) && n1.name[0] == '\0');
    CHECK(rec_store_set_name(&s, 1, n1.seq, "Grocery list") == 0);
    CHECK(rec_store_clip(&s, 1, &c) && !strcmp(c.name, "Grocery list"));
    CHECK(rec_store_set_name(&s, 1, n1.seq, "Groceries") == 0);
    int listed = rec_store_list(&s, list, 8), at = 0;
    while (at < listed && list[at].slot != 1) {
        at++;
    }
    CHECK(at < listed && !strcmp(list[at].name, "Groceries"));
    CHECK(rec_store_set_name(&s, 1, n1.seq, "") == 0 && rec_store_clip(&s, 1, &c) && c.name[0] == '\0');
    /* Not for another clip, too long, or with a control character. */
    CHECK(rec_store_set_name(&s, 1, n1.seq + 100, "x") != 0);
    CHECK(rec_store_set_name(&s, 1, n1.seq, "0123456789012345678901234567890123456789012345678") != 0);
    CHECK(rec_store_set_name(&s, 1, n1.seq, "a\nb") != 0);
    CHECK(rec_store_set_name(&s, 1, n1.seq, "012345678901234567890123456789012345678901234567") == 0);
    /* A record a power cut tore is skipped: the name before stays, the next one goes after it. */
    uint32_t rec = 64 + 4 * 64;   /* the fifth record, the next free one */
    CHECK(ram.mem[1 * s.slot_bytes + rec] == 0xFF);
    memcpy(ram.mem + 1 * s.slot_bytes + rec, "RNAM\x05\0\0\0Hal", 11);
    CHECK(rec_store_clip(&s, 1, &c) && !strcmp(c.name, "012345678901234567890123456789012345678901234567"));
    CHECK(rec_store_set_name(&s, 1, n1.seq, "Half") == 0 && rec_store_clip(&s, 1, &c) && !strcmp(c.name, "Half"));
    /* Naming it what it's called writes nothing; a full log refuses. */
    CHECK(rec_store_set_name(&s, 1, n1.seq, "Half") == 0);
    int renames = 0;
    char name[16];
    while (snprintf(name, sizeof(name), "n%d", renames), rec_store_set_name(&s, 1, n1.seq, name) == 0) {
        renames++;
    }
    CHECK(renames == 31 - 6 && rec_store_clip(&s, 1, &c) && !strcmp(c.name, "n24"));
    /* Deleting a clip, or a save reusing its slot, takes its name too. */
    CHECK(rec_store_delete(&s, 1) == 0);
    CHECK(rec_store_save(&s, data, 10, 0, 0, 0, 0, &c) == 0 && c.slot == 1 && c.name[0] == '\0');

    /* A recording in parts; a named one is kept when the store fills. */
    {
        ram_flash_t r2;
        rec_flash_t f2 = ram_flash(&r2, 4 * 512 * 1024);
        rec_store_t t;
        CHECK(rec_store_open(&t, &f2, MAX) == 0 && t.slots == 4);
        rec_clip_t a, b, d;
        CHECK(rec_store_save(&t, data, 100, 2000, 0, 0, 0, &a) == 0 && a.first == a.seq);       /* seq 1 */
        CHECK(rec_store_save(&t, data, MAX, 2100, a.seq, 0, 0, &b) == 0 && b.first == a.seq);   /* seq 2, part 2 */
        CHECK(rec_store_save(&t, data, 30, 2200, 0, 0, 0, &d) == 0 && d.first == d.seq);        /* seq 3 */
        rec_clip_t clips[8];
        rec_recording_t recs[4];
        int n = rec_store_list(&t, clips, 8);
        CHECK(n == 3 && rec_group(clips, n, recs, 4) == 2);
        CHECK(recs[0].first == 3 && recs[0].parts == 1 && recs[0].blocks == 30);
        CHECK(recs[1].first == 1 && recs[1].parts == 2 && recs[1].whole && recs[1].unix_time == 2000);
        CHECK(recs[1].slots[0] == a.slot && recs[1].slots[1] == b.slot && recs[1].blocks == 100 + MAX);
        /* Kept, recording 1 stays: the next saves take the free slot, then
         * clip 3's (a name alone doesn't keep it), then refuse rather than
         * touch recording 1. Keeping leaves the name, and naming the flag. */
        CHECK(rec_store_set_kept(&t, a.slot, a.seq, true) == 0);
        CHECK(rec_store_set_name(&t, a.slot, a.seq, "Keep me") == 0);
        CHECK(rec_store_clip(&t, a.slot, &c) && c.kept && !strcmp(c.name, "Keep me"));
        CHECK(rec_store_set_name(&t, d.slot, d.seq, "Only named") == 0);
        n = rec_store_list(&t, clips, 8);
        CHECK(rec_group(clips, n, recs, 4) == 2 && recs[1].kept && !recs[0].kept);
        CHECK(rec_store_save(&t, data, 10, 0, 0, 0, 0, &c) == 0 && c.seq == 4);
        CHECK(rec_store_save(&t, data, 10, 0, 0, 0, 0, &c) == 0 && c.slot == d.slot);
        CHECK(rec_store_save(&t, data, 10, 0, 0, 0, 0, &c) == 0 && c.seq == 6);   /* clip 4's, the oldest not kept */
        /* A recording's own earlier parts are never what its next part replaces. */
        CHECK(rec_store_save(&t, data, 10, 0, 6, 0, 0, &c) == 0 && c.first == 6);   /* clip 5's */
        CHECK(rec_store_save(&t, data, 10, 0, 6, 0, 0, &c) == REC_FULL);
        CHECK(rec_store_set_kept(&t, a.slot, a.seq, false) == 0);
        CHECK(rec_store_clip(&t, a.slot, &c) && !c.kept && !strcmp(c.name, "Keep me"));
        CHECK(rec_store_save(&t, data, 10, 0, 6, 0, 0, &c) == 0 && c.slot == a.slot);
        /* Its first part gone, what's left of recording 1 is still a recording. */
        n = rec_store_list(&t, clips, 8);
        CHECK(rec_group(clips, n, recs, 4) == 2);
        CHECK(recs[0].first == 6 && recs[0].parts == 3 && recs[0].whole);
        CHECK(recs[1].first == 1 && recs[1].parts == 1 && !recs[1].whole && recs[1].unix_time == 2100);
        free(r2.mem);
    }

    /* Undo: a deleted clip comes back with its name and keep flag, until a
     * save takes its slot. */
    {
        ram_flash_t r3;
        rec_flash_t f3 = ram_flash(&r3, 2 * 512 * 1024);
        rec_store_t t;
        CHECK(rec_store_open(&t, &f3, MAX) == 0 && t.slots == 2);
        rec_clip_t a, b;
        CHECK(rec_store_save(&t, data, 200, 3000, 0, 0, 0, &a) == 0);
        CHECK(rec_store_set_name(&t, a.slot, a.seq, "Undo me") == 0 && rec_store_set_kept(&t, a.slot, a.seq, true) == 0);
        rec_deleted_t gone;
        CHECK(rec_store_delete_undoable(&t, a.slot, &gone) == 0 && !rec_store_clip(&t, a.slot, &b));
        CHECK(rec_store_restore(&t, &gone) == 0 && rec_store_clip(&t, a.slot, &b));
        CHECK(b.seq == a.seq && b.blocks == 200 && b.unix_time == 3000 && b.kept && !strcmp(b.name, "Undo me"));
        CHECK(rec_store_restore(&t, &gone) != 0);   /* it's back: the slot isn't empty */
        /* Deleted again, and a save takes the slot: no undo. */
        CHECK(rec_store_delete_undoable(&t, a.slot, &gone) == 0);
        CHECK(rec_store_save(&t, data, 10, 0, 0, 0, 0, &b) == 0 && b.slot == a.slot);
        CHECK(rec_store_restore(&t, &gone) != 0);
        /* Nor if the blocks changed, as a save cut short after its erase leaves them. */
        CHECK(rec_store_delete_undoable(&t, b.slot, &gone) == 0);
        r3.mem[b.slot * t.slot_bytes + REC_SECTOR] ^= 0x10;
        CHECK(rec_store_restore(&t, &gone) != 0);
        CHECK(rec_store_delete_undoable(&t, b.slot, &gone) != 0);   /* nothing there to delete */
        free(r3.mem);
    }

    /* The save's record: a shape that follows the loudness, and when it was
     * saved by the board's count; marking it downloaded keeps its name. */
    {
        ram_flash_t r4;
        rec_flash_t f4 = ram_flash(&r4, 2 * 512 * 1024);
        rec_store_t t;
        CHECK(rec_store_open(&t, &f4, MAX) == 0);
        static uint8_t rising[480 * REC_BLOCK_BYTES];
        rec_ring_t ring;
        static uint8_t mem[480 * REC_BLOCK_BYTES];
        rec_ring_init(&ring, mem, 480);
        int16_t pcm[REC_BLOCK_SAMPLES];
        for (int b = 0; b < 480; b++) {   /* quiet, then louder and louder */
            for (int i = 0; i < REC_BLOCK_SAMPLES; i++) {
                pcm[i] = (int16_t)((b < 240 ? 30 : 30 + (b - 240) * 120) * sinf(2.0f * (float)M_PI * 400 * i / REC_RATE));
            }
            rec_ring_feed(&ring, pcm, REC_BLOCK_SAMPLES);
        }
        CHECK(rec_ring_copy(&ring, rising) == 480);
        rec_clip_t a, b2;
        CHECK(rec_store_save(&t, rising, 480, 0, 0, 7, 1234, &a) == 0);
        CHECK(rec_store_clip(&t, a.slot, &b2) && b2.boot == 7 && b2.uptime_s == 1234 && !b2.pulled);
        CHECK(b2.env[0] < b2.env[REC_ENV_N - 1] && b2.env[REC_ENV_N / 4] < b2.env[3 * REC_ENV_N / 4]);
        for (int i = REC_ENV_N / 2 + 1; i < REC_ENV_N; i++) {
            CHECK(b2.env[i] >= b2.env[i - 1]);   /* louder is never lower */
        }
        CHECK(rec_store_set_name(&t, a.slot, a.seq, "Ramp") == 0 && rec_store_set_pulled(&t, a.slot, a.seq) == 0);
        CHECK(rec_store_clip(&t, a.slot, &b2) && b2.pulled && !b2.kept && !strcmp(b2.name, "Ramp"));
        /* Undo puts the record and the flags back with the clip. */
        rec_deleted_t gone;
        CHECK(rec_store_delete_undoable(&t, a.slot, &gone) == 0 && rec_store_restore(&t, &gone) == 0);
        CHECK(rec_store_clip(&t, a.slot, &b2) && b2.pulled && b2.uptime_s == 1234 && b2.env[REC_ENV_N - 1] == a.env[REC_ENV_N - 1]);
        /* A recording's shape: its parts' in turn, each over its share. */
        rec_clip_t p2;
        CHECK(rec_store_save(&t, rising, 240, 0, a.seq, 7, 1300, &p2) == 0);   /* a quiet part after */
        rec_clip_t clips[4];
        rec_recording_t rec;
        int n = rec_store_list(&t, clips, 4);
        CHECK(rec_group(clips, n, &rec, 1) == 1 && rec.parts == 2 && rec.blocks == 720 && rec.pulled);
        uint8_t env[REC_ENV_N];
        rec_group_envelope(&rec, clips, n, env);
        CHECK(env[REC_ENV_N * 2 / 3 - 2] > env[REC_ENV_N - 1]);   /* loud end of part 1, then quiet part 2 */
        free(r4.mem);
    }

    /* Too long, empty, or a store that can't hold one clip. */
    CHECK(rec_store_save(&s, data, MAX + 1, 0, 0, 0, 0, &c) != 0 && rec_store_save(&s, data, 0, 0, 0, 0, 0, &c) != 0);
    ram_flash_t small;
    rec_flash_t sf = ram_flash(&small, 256 * 1024);
    CHECK(rec_store_open(&s, &sf, MAX) != 0);
    printf("store ok\n");
    return 0;
}

/* ---- device: rec_proto over stdin/stdout ---- */

static rec_store_t s_dev;
static int64_t s_time;

static int dev_list(rec_clip_t *out, int max) { return rec_store_list(&s_dev, out, max); }
static int dev_clip(uint32_t slot, rec_clip_t *out) { return rec_store_clip(&s_dev, slot, out) ? 0 : -1; }
static int dev_read(uint32_t slot, uint32_t off, void *buf, uint32_t len) { return rec_store_read(&s_dev, slot, off, buf, len); }
static int dev_del(uint32_t slot) { return rec_store_delete(&s_dev, slot); }
static int dev_set_name(uint32_t slot, uint32_t seq, const char *name) { return rec_store_set_name(&s_dev, slot, seq, name); }
static int dev_set_kept(uint32_t slot, uint32_t seq, bool kept) { return rec_store_set_kept(&s_dev, slot, seq, kept); }
static int dev_set_pulled(uint32_t slot, uint32_t seq) { return rec_store_set_pulled(&s_dev, slot, seq); }
static void dev_set_time(int64_t t, int32_t offset) { s_time = t; fprintf(stderr, "time %lld %d\n", (long long)t, offset); }
static int64_t dev_time(void) { return s_time; }
static uint32_t s_recording;
static uint32_t dev_recording(void) { return s_recording; }

/* A 3x2 screen: red, green, blue on top, white, black and 0x1234 below. */
static int dev_snap(const char *screen, uint8_t **rgb565, uint32_t *w, uint32_t *h)
{
    static const uint16_t PX[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x1234 };
    if (strcmp(screen, "") && strcmp(screen, "main")) {
        return -1;
    }
    *rgb565 = malloc(sizeof(PX));
    for (size_t i = 0; i < sizeof(PX) / 2; i++) {
        (*rgb565)[2 * i] = (uint8_t)PX[i];
        (*rgb565)[2 * i + 1] = (uint8_t)(PX[i] >> 8);
    }
    *w = 3;
    *h = 2;
    return 0;
}

static void dev_out(const char *line, size_t n)
{
    /* A log line's start written just before, as the board's logs can be. */
    static const char noise[] = "I (1234) audio: level -40.2 dBFS";
    CHECK(write(1, noise, sizeof(noise) - 1) == (ssize_t)sizeof(noise) - 1);
    while (n) {
        ssize_t w = write(1, line, n);
        CHECK(w > 0);
        line += w;
        n -= (size_t)w;
    }
}

/* A battery log of two hours off USB, 100% to 90%, the screen on one
 * minute in ten, ending in a power-off. */
static int dev_batt(uint32_t i, uint32_t *minute, uint32_t *mv, uint32_t *pct, uint32_t *flags)
{
    if (i > 120) {
        return -1;
    }
    *minute = i;
    *pct = 100 - i / 12;
    *mv = 4150 - i * 2;
    *flags = (i % 10 == 0 ? 4 : 0) | (i == 120 ? 16 : 0);
    return 0;
}

static int cmd_device(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    s_recording = argc == 4 ? (uint32_t)atoi(argv[3]) : 0;
    enum { MAX = 3000 };
    ram_flash_t ram;
    rec_flash_t f = ram_flash(&ram, 6 * 512 * 1024);
    CHECK(rec_store_open(&s_dev, &f, MAX) == 0);
    /* Clips 4 and 5 are one recording in two parts. */
    static const struct { uint32_t blocks; float hz; int64_t time; uint32_t first; } CLIPS[] = {
        { 100, 440, 1793000000, 0 }, { 250, 1000, 1793000600, 0 }, { 50, 300, 0, 0 },
        { 120, 500, 1793001200, 0 }, { 80, 700, 1793001400, 4 },
    };
    uint8_t *data = malloc(MAX * REC_BLOCK_BYTES);
    for (size_t i = 0; i < sizeof(CLIPS) / sizeof(CLIPS[0]); i++) {
        rec_clip_t c;
        uint32_t n = tone_blocks(data, CLIPS[i].blocks, CLIPS[i].hz);
        CHECK(rec_store_save(&s_dev, data, n, CLIPS[i].time, CLIPS[i].first, 0, 0, &c) == 0);
        if (i == 1) {
            CHECK(rec_store_set_name(&s_dev, c.slot, c.seq, "Grocery list") == 0);
            CHECK(rec_store_set_kept(&s_dev, c.slot, c.seq, true) == 0);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%u.blocks", argv[2], (unsigned)c.seq);
        spit(path, data, (size_t)n * REC_BLOCK_BYTES);
    }
    const rec_proto_t p = {
        .version = "test",
        .slots = s_dev.slots,
        .seconds = MAX / REC_BLOCKS_PER_S,
        .list = dev_list,
        .clip = dev_clip,
        .read = dev_read,
        .del = dev_del,
        .set_name = dev_set_name,
        .set_kept = dev_set_kept,
        .set_pulled = dev_set_pulled,
        .set_time = dev_set_time,
        .time = dev_time,
        .recording = dev_recording,
        .snap = dev_snap,
        .batt = dev_batt,
        .out = dev_out,
    };
    fprintf(stderr, "ready\n");
    char line[128];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) {
            return 0;
        }
        if (c == '\n' || c == '\r') {
            if (len && !overflow) {
                line[len] = '\0';
                rec_proto_line(&p, line);
            }
            len = 0;
            overflow = false;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        } else {
            overflow = true;
        }
    }
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "ring")) return cmd_ring(argc, argv);
    if (!strcmp(argv[1], "decode")) return cmd_decode(argc, argv);
    if (!strcmp(argv[1], "crc")) return cmd_crc(argc, argv);
    if (!strcmp(argv[1], "hours")) return cmd_hours(argc, argv);
    if (!strcmp(argv[1], "adpcm")) return cmd_adpcm();
    if (!strcmp(argv[1], "tempo")) return cmd_tempo(argc, argv);
    if (!strcmp(argv[1], "multitap")) return cmd_multitap(argc, argv);
    if (!strcmp(argv[1], "store")) return cmd_store();
    if (!strcmp(argv[1], "span")) return cmd_span();
    if (!strcmp(argv[1], "device")) return cmd_device(argc, argv);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
