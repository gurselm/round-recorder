#include "rec_store.h"

#include <stddef.h>
#include <string.h>

#include "rec_ring.h"

_Static_assert(sizeof(rec_header_t) == 40, "header layout");

#define NAME_OFF 64             /* in the header sector, after the header */
#define NAME_MAGIC 0x4D414E52u  /* "RNAM" */
#define ENV_OFF 2048            /* the save's record, after the name log */
#define ENV_MAGIC 0x564E4552u   /* "RENV" */

#define NAME_KEPT 0x01          /* in flags: never replaced by a save */
#define NAME_PULLED 0x02        /* downloaded by pull.py */

typedef struct {
    uint32_t magic;
    uint8_t len;
    uint8_t flags;
    uint8_t zero[2];
    char name[52];   /* len bytes, then zeros */
    uint32_t crc;    /* of the bytes before it */
} name_rec_t;

_Static_assert(sizeof(name_rec_t) == 64, "name record layout");
_Static_assert(REC_NAME_MAX <= sizeof(((name_rec_t *)0)->name), "name fits its record");

#define NAME_RECS ((ENV_OFF - NAME_OFF) / sizeof(name_rec_t))

/* Written once by the save, before the header: how loud it was over its
 * length, and when it was saved by the board's own count, which needs no
 * clock. */
typedef struct {
    uint32_t magic;
    uint32_t boot;
    uint32_t uptime_s;
    uint8_t env[REC_ENV_N];
    uint32_t crc;    /* of the bytes before it */
} env_rec_t;

_Static_assert(sizeof(env_rec_t) == 64, "save record layout");

uint32_t rec_crc32(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t NIBBLE[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ NIBBLE[crc & 15];
        crc = (crc >> 4) ^ NIBBLE[crc & 15];
    }
    return ~crc;
}

static uint32_t header_crc(const rec_header_t *h)
{
    return rec_crc32(0, h, offsetof(rec_header_t, header_crc));
}

static bool read_header(rec_store_t *s, uint32_t slot, rec_header_t *h)
{
    if (slot >= s->slots || s->f.read(s->f.ctx, slot * s->slot_bytes, h, sizeof(*h))) {
        return false;
    }
    return h->magic == REC_MAGIC && h->version == REC_VERSION && h->block_bytes == REC_BLOCK_BYTES &&
           h->seq && h->blocks && h->blocks <= s->max_blocks && h->header_crc == header_crc(h);
}

/* The slot's name and flags (its newest good record), and the first record
 * never written, or -1 if there's none left. A record a power cut tore is
 * neither. */
static int read_meta(rec_store_t *s, uint32_t slot, char *name, uint8_t *flags)
{
    name[0] = '\0';
    *flags = 0;
    for (uint32_t i = 0; i < NAME_RECS; i++) {
        name_rec_t r;
        if (s->f.read(s->f.ctx, slot * s->slot_bytes + NAME_OFF + i * sizeof(r), &r, sizeof(r))) {
            return -1;
        }
        const uint8_t *b = (const uint8_t *)&r;
        size_t k = 0;
        while (k < sizeof(r) && b[k] == 0xFF) {
            k++;
        }
        if (k == sizeof(r)) {
            return (int)i;
        }
        if (r.magic == NAME_MAGIC && r.len <= REC_NAME_MAX && r.crc == rec_crc32(0, &r, offsetof(name_rec_t, crc))) {
            memcpy(name, r.name, r.len);
            name[r.len] = '\0';
            *flags = r.flags;
        }
    }
    return -1;
}

static void to_clip(rec_store_t *s, uint32_t slot, const rec_header_t *h, rec_clip_t *out)
{
    *out = (rec_clip_t){
        .slot = slot,
        .seq = h->seq,
        .first = h->first_seq ? h->first_seq : h->seq,
        .blocks = h->blocks,
        .unix_time = h->unix_time,
        .data_crc = h->data_crc,
    };
    uint8_t flags;
    read_meta(s, slot, out->name, &flags);
    out->kept = flags & NAME_KEPT;
    out->pulled = flags & NAME_PULLED;
    env_rec_t e;
    if (!s->f.read(s->f.ctx, slot * s->slot_bytes + ENV_OFF, &e, sizeof(e)) && e.magic == ENV_MAGIC &&
        e.crc == rec_crc32(0, &e, offsetof(env_rec_t, crc))) {
        memcpy(out->env, e.env, sizeof(out->env));
        out->boot = e.boot;
        out->uptime_s = e.uptime_s;
    }
}

int rec_store_open(rec_store_t *s, const rec_flash_t *f, uint32_t max_blocks)
{
    memset(s, 0, sizeof(*s));
    s->f = *f;
    s->max_blocks = max_blocks;
    uint64_t need = REC_SECTOR + (uint64_t)max_blocks * REC_BLOCK_BYTES;
    s->slot_bytes = (uint32_t)((need + REC_ERASE_UNIT - 1) / REC_ERASE_UNIT * REC_ERASE_UNIT);
    s->slots = f->size / s->slot_bytes;
    if (s->slots > REC_MAX_SLOTS) {
        s->slots = REC_MAX_SLOTS;
    }
    if (!max_blocks || !s->slots) {
        return -1;
    }
    s->next_seq = 1;
    for (uint32_t i = 0; i < s->slots; i++) {
        rec_header_t h;
        if (read_header(s, i, &h) && h.seq >= s->next_seq) {
            s->next_seq = h.seq + 1;
        }
    }
    return 0;
}

int rec_store_list(rec_store_t *s, rec_clip_t *out, int max)
{
    int n = 0;
    for (uint32_t i = 0; i < s->slots && max > 0; i++) {
        rec_header_t h;
        if (!read_header(s, i, &h)) {
            continue;
        }
        /* Insertion by seq, newest first: a few dozen slots at most. */
        int j;
        if (n < max) {
            j = n++;
        } else if (out[max - 1].seq < h.seq) {
            j = max - 1;   /* the rows are full: the oldest goes */
        } else {
            continue;      /* older than every row */
        }
        while (j > 0 && out[j - 1].seq < h.seq) {
            out[j] = out[j - 1];
            j--;
        }
        to_clip(s, i, &h, &out[j]);
    }
    return n;
}

bool rec_store_clip(rec_store_t *s, uint32_t slot, rec_clip_t *out)
{
    rec_header_t h;
    if (!read_header(s, slot, &h)) {
        return false;
    }
    to_clip(s, slot, &h, out);
    return true;
}

/* An empty slot, else the oldest clip not in a kept recording or in
 * first_seq's; -1 if there's none. */
static int pick_slot(rec_store_t *s, uint32_t first_seq)
{
    static uint32_t seq[REC_MAX_SLOTS], first[REC_MAX_SLOTS], kept[REC_MAX_SLOTS];
    uint32_t n_kept = 0;
    for (uint32_t i = 0; i < s->slots; i++) {
        rec_header_t h;
        if (!read_header(s, i, &h)) {
            return (int)i;
        }
        seq[i] = h.seq;
        first[i] = h.first_seq ? h.first_seq : h.seq;
        char name[REC_NAME_MAX + 1];
        uint8_t f;
        if (!h.first_seq && (read_meta(s, i, name, &f), f & NAME_KEPT)) {
            kept[n_kept++] = h.seq;
        }
    }
    int pick = -1;
    for (uint32_t i = 0; i < s->slots; i++) {
        bool keep = first_seq && first[i] == first_seq;
        for (uint32_t k = 0; k < n_kept && !keep; k++) {
            keep = first[i] == kept[k];
        }
        if (!keep && (pick < 0 || seq[i] < seq[pick])) {
            pick = (int)i;
        }
    }
    return pick;
}

/* The loudest step index in each of REC_ENV_N stretches of the blocks: an
 * ADPCM block starts with its encoder's step, which follows the loudness on
 * a log scale, so this needs nothing decoded. */
static void envelope(const uint8_t *blocks, uint32_t n, uint8_t *env)
{
    for (uint32_t b = 0; b < REC_ENV_N; b++) {
        uint32_t from = (uint32_t)((uint64_t)b * n / REC_ENV_N), to = (uint32_t)((uint64_t)(b + 1) * n / REC_ENV_N);
        uint8_t peak = 0;
        for (uint32_t i = from; i < to || i == from; i++) {
            uint8_t step = blocks[(size_t)(i < n ? i : n - 1) * REC_BLOCK_BYTES + 2];
            peak = step > peak ? step : peak;
        }
        env[b] = peak > 88 ? 88 : peak;
    }
}

int rec_store_save(rec_store_t *s, const uint8_t *blocks, uint32_t n, int64_t unix_time, uint32_t first_seq,
                   uint32_t boot, uint32_t uptime_s, rec_clip_t *saved)
{
    if (!n || n > s->max_blocks) {
        return -1;
    }
    int picked = pick_slot(s, first_seq);
    if (picked < 0) {
        return REC_FULL;
    }
    uint32_t slot = (uint32_t)picked;
    uint32_t base = slot * s->slot_bytes, len = n * REC_BLOCK_BYTES;
    if (s->f.erase(s->f.ctx, base, s->slot_bytes) || s->f.write(s->f.ctx, base + REC_SECTOR, blocks, len)) {
        return -1;
    }
    rec_header_t h = {
        .magic = REC_MAGIC,
        .version = REC_VERSION,
        .block_bytes = REC_BLOCK_BYTES,
        .seq = s->next_seq,
        .blocks = n,
        .rate = REC_RATE,
        .first_seq = first_seq,
        .unix_time = unix_time,
        .data_crc = rec_crc32(0, blocks, len),
    };
    h.header_crc = header_crc(&h);
    env_rec_t e = { .magic = ENV_MAGIC, .boot = boot, .uptime_s = uptime_s };
    envelope(blocks, n, e.env);
    e.crc = rec_crc32(0, &e, offsetof(env_rec_t, crc));
    rec_header_t back;
    if (s->f.write(s->f.ctx, base + ENV_OFF, &e, sizeof(e)) || s->f.write(s->f.ctx, base, &h, sizeof(h)) ||
        !read_header(s, slot, &back) || back.seq != h.seq) {
        return -1;
    }
    s->next_seq++;
    if (saved) {
        to_clip(s, slot, &h, saved);
    }
    return 0;
}

int rec_group(const rec_clip_t *clips, int n, rec_recording_t *out, int max)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        const rec_clip_t *c = &clips[i];
        int r = 0;
        while (r < count && out[r].first != c->first) {
            r++;
        }
        if (r == count) {
            if (count == max) {
                continue;
            }
            out[count++] = (rec_recording_t){ .first = c->first, .unix_time = c->unix_time };
        }
        rec_recording_t *rec = &out[r];
        if (rec->parts == REC_MAX_PARTS) {
            continue;
        }
        /* Clips come newest first, so each part goes in front of those so far. */
        memmove(rec->slots + 1, rec->slots, rec->parts * sizeof(rec->slots[0]));
        rec->slots[0] = c->slot;
        rec->parts++;
        rec->blocks += c->blocks;
        if (c->seq == c->first) {
            rec->whole = true;
            rec->kept = c->kept;
            rec->pulled = c->pulled;
            rec->unix_time = c->unix_time;
            rec->boot = c->boot;
            rec->uptime_s = c->uptime_s;
            memcpy(rec->name, c->name, sizeof(rec->name));
        } else if (!rec->whole) {
            /* Its earliest part left, for a time. */
            rec->unix_time = c->unix_time ? c->unix_time : rec->unix_time;
            rec->boot = c->boot;
            rec->uptime_s = c->uptime_s;
        }
    }
    return count;
}

void rec_group_envelope(const rec_recording_t *r, const rec_clip_t *clips, int n, uint8_t *env)
{
    /* Each part's shape, stretched over its share of the whole. */
    memset(env, 0, REC_ENV_N);
    uint32_t start = 0;
    for (uint32_t p = 0; p < r->parts; p++) {
        const rec_clip_t *c = NULL;
        for (int i = 0; i < n && !c; i++) {
            c = clips[i].slot == r->slots[p] ? &clips[i] : NULL;
        }
        if (!c || !r->blocks) {
            continue;
        }
        for (uint32_t b = 0; b < REC_ENV_N; b++) {
            uint64_t at = (uint64_t)(b * 2 + 1) * r->blocks / (REC_ENV_N * 2);   /* the middle of b */
            if (at >= start && at < start + c->blocks) {
                env[b] = c->env[(at - start) * REC_ENV_N / c->blocks];
            }
        }
        start += c->blocks;
    }
}

/* Writes the slot's next name record: name, or its name so far if NULL, and
 * each flag in mask set as in flags, the rest as they were. */
static int write_meta(rec_store_t *s, uint32_t slot, uint32_t seq, const char *name, uint8_t mask, uint8_t flags)
{
    rec_header_t h;
    if (!read_header(s, slot, &h) || h.seq != seq) {
        return -1;
    }
    char old[REC_NAME_MAX + 1];
    uint8_t was;
    int i = read_meta(s, slot, old, &was);
    name = name ? name : old;
    uint8_t now = (uint8_t)((was & ~mask) | (flags & mask));
    size_t len = strlen(name);
    if (len > REC_NAME_MAX) {
        return -1;
    }
    for (size_t k = 0; k < len; k++) {
        if ((uint8_t)name[k] < 0x20 || name[k] == 0x7F) {
            return -1;   /* it goes into protocol lines and file names */
        }
    }
    if (!strcmp(old, name) && now == was) {
        return 0;
    }
    if (i < 0) {
        return -1;
    }
    name_rec_t r = { .magic = NAME_MAGIC, .len = (uint8_t)len, .flags = now };
    memcpy(r.name, name, len);
    r.crc = rec_crc32(0, &r, offsetof(name_rec_t, crc));
    if (s->f.write(s->f.ctx, slot * s->slot_bytes + NAME_OFF + (uint32_t)i * sizeof(r), &r, sizeof(r))) {
        return -1;
    }
    char back[REC_NAME_MAX + 1];
    uint8_t back_flags;
    read_meta(s, slot, back, &back_flags);
    return strcmp(back, name) || back_flags != now ? -1 : 0;
}

int rec_store_set_name(rec_store_t *s, uint32_t slot, uint32_t seq, const char *name)
{
    return write_meta(s, slot, seq, name, 0, 0);
}

int rec_store_set_kept(rec_store_t *s, uint32_t slot, uint32_t seq, bool kept)
{
    return write_meta(s, slot, seq, NULL, NAME_KEPT, kept ? NAME_KEPT : 0);
}

int rec_store_set_pulled(rec_store_t *s, uint32_t slot, uint32_t seq)
{
    return write_meta(s, slot, seq, NULL, NAME_PULLED, NAME_PULLED);
}

int rec_store_read(rec_store_t *s, uint32_t slot, uint32_t off, void *buf, uint32_t len)
{
    if (slot >= s->slots || off > s->slot_bytes - REC_SECTOR || len > s->slot_bytes - REC_SECTOR - off) {
        return -1;
    }
    return s->f.read(s->f.ctx, slot * s->slot_bytes + REC_SECTOR + off, buf, len);
}

int rec_store_delete(rec_store_t *s, uint32_t slot)
{
    if (slot >= s->slots) {
        return -1;
    }
    return s->f.erase(s->f.ctx, slot * s->slot_bytes, REC_SECTOR);
}

int rec_store_delete_undoable(rec_store_t *s, uint32_t slot, rec_deleted_t *out)
{
    if (!read_header(s, slot, &out->header)) {
        return -1;
    }
    out->slot = slot;
    read_meta(s, slot, out->name, &out->flags);
    s->f.read(s->f.ctx, slot * s->slot_bytes + ENV_OFF, out->env_rec, sizeof(out->env_rec));
    return rec_store_delete(s, slot);
}

int rec_store_restore(rec_store_t *s, const rec_deleted_t *d)
{
    /* Only into the slot as the delete left it: header sector erased, the
     * blocks untouched. A save since would have erased or rewritten them. */
    uint8_t buf[1024];
    uint32_t base = d->slot * s->slot_bytes;
    if (d->slot >= s->slots || s->f.read(s->f.ctx, base, buf, NAME_OFF + sizeof(name_rec_t))) {
        return -1;
    }
    for (size_t i = 0; i < NAME_OFF + sizeof(name_rec_t); i++) {
        if (buf[i] != 0xFF) {
            return -1;
        }
    }
    uint32_t total = d->header.blocks * REC_BLOCK_BYTES, crc = 0;
    for (uint32_t off = 0; off < total; off += sizeof(buf)) {
        uint32_t n = total - off < sizeof(buf) ? total - off : sizeof(buf);
        if (rec_store_read(s, d->slot, off, buf, n)) {
            return -1;
        }
        crc = rec_crc32(crc, buf, n);
    }
    if (crc != d->header.data_crc || s->f.write(s->f.ctx, base + ENV_OFF, d->env_rec, sizeof(d->env_rec)) ||
        s->f.write(s->f.ctx, base, &d->header, sizeof(d->header))) {
        return -1;
    }
    if ((d->name[0] || d->flags) && write_meta(s, d->slot, d->header.seq, d->name, 0xFF, d->flags)) {
        return -1;
    }
    rec_clip_t back;
    return rec_store_clip(s, d->slot, &back) && back.seq == d->header.seq ? 0 : -1;
}
