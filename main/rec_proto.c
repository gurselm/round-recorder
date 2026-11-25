#include "rec_proto.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rec_ring.h"


static void reply(const rec_proto_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void reply(const rec_proto_t *p, const char *fmt, ...)
{
    char line[192] = "\nRR ";
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + 4, sizeof(line) - 5, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = 4 + ((size_t)n < sizeof(line) - 5 ? (size_t)n : sizeof(line) - 6);
    line[len++] = '\n';
    p->out(line, len);
}

/* A decimal number that ends the string or a word; false for anything else. */
static bool number(const char *s, long long *out)
{
    char *end;
    if (!*s) {
        return false;
    }
    *out = strtoll(s, &end, 10);
    return end != s && (*end == '\0' || *end == ' ');
}

static void list(const rec_proto_t *p)
{
    rec_clip_t *clips = malloc(p->slots * sizeof(*clips));   /* one a slot at most */
    if (!clips) {
        reply(p, "ERR no memory");
        return;
    }
    int n = p->list(clips, (int)p->slots);
    for (int i = 0; i < n; i++) {
        reply(p, "CLIP slot=%" PRIu32 " seq=%" PRIu32 " first=%" PRIu32 " time=%" PRId64 " blocks=%" PRIu32 " crc=%08" PRIx32
              " kept=%d pulled=%d name=%s", clips[i].slot, clips[i].seq, clips[i].first, clips[i].unix_time,
              clips[i].blocks, clips[i].data_crc, clips[i].kept, clips[i].pulled, clips[i].name);
    }
    free(clips);
    reply(p, "END %d", n);
}

static size_t base64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *o++ = A[v >> 18 & 63];
        *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? A[v & 63] : '=';
    }
    return (size_t)(o - out);
}

/* One "RR DATA" line of up to REC_PROTO_CHUNK bytes. */
static void data(const rec_proto_t *p, const uint8_t *buf, uint32_t n)
{
    static char line[sizeof("\nRR DATA \n") + REC_PROTO_CHUNK / 3 * 4];
    size_t len = sizeof("\nRR DATA ") - 1;
    memcpy(line, "\nRR DATA ", len);
    len += base64(buf, n, line + len);
    line[len++] = '\n';
    p->out(line, len);
}

static void get(const rec_proto_t *p, uint32_t slot)
{
    static uint8_t buf[REC_PROTO_CHUNK];
    rec_clip_t clip;
    if (p->clip(slot, &clip)) {
        reply(p, "ERR no clip in slot %" PRIu32, slot);
        return;
    }
    uint32_t total = clip.blocks * REC_BLOCK_BYTES, crc = 0;
    for (uint32_t off = 0; off < total; off += REC_PROTO_CHUNK) {
        uint32_t n = total - off < REC_PROTO_CHUNK ? total - off : REC_PROTO_CHUNK;
        if (p->read(slot, off, buf, n)) {
            reply(p, "ERR flash read at %" PRIu32, off);
            return;
        }
        crc = rec_crc32(crc, buf, n);
        data(p, buf, n);
    }
    /* The CRC is of what was sent: a clip replaced mid-transfer fails the check. */
    if (crc != clip.data_crc) {
        reply(p, "ERR clip changed while it was sent");
        return;
    }
    reply(p, "END %" PRIu32 " %08" PRIx32, total, crc);
}

static void snap(const rec_proto_t *p, const char *screen)
{
    uint8_t *px;
    uint32_t w, h;
    if (!p->snap || p->snap(screen, &px, &w, &h)) {
        reply(p, "ERR no screenshot");
        return;
    }
    reply(p, "SNAP %" PRIu32 " %" PRIu32 " rgb565", w, h);
    uint32_t total = w * h * 2;
    for (uint32_t off = 0; off < total; off += REC_PROTO_CHUNK) {
        data(p, px + off, total - off < REC_PROTO_CHUNK ? total - off : REC_PROTO_CHUNK);
    }
    reply(p, "END %" PRIu32 " %08" PRIx32, total, rec_crc32(0, px, total));
    free(px);
}

void rec_proto_line(const rec_proto_t *p, const char *line)
{
    while (*line == ' ' || *line == '\r') {
        line++;
    }
    if (strncmp(line, "RR ", 3)) {
        return;   /* not for us: stray input */
    }
    const char *cmd = line + 3;
    long long a, b;
    if (!strcmp(cmd, "HELLO")) {
        reply(p, "HELLO round-recorder %s slots=%" PRIu32 " seconds=%" PRIu32 " time=%" PRId64 " recording=%" PRIu32,
              p->version, p->slots, p->seconds, p->time(), p->recording());
    } else if (!strncmp(cmd, "TIME ", 5) && number(cmd + 5, &a) && strchr(cmd + 5, ' ') &&
               number(strchr(cmd + 5, ' ') + 1, &b) && a > 0 && b > -86400 && b < 86400) {
        p->set_time(a, (int32_t)b);
        reply(p, "OK");
    } else if (!strcmp(cmd, "LIST")) {
        list(p);
    } else if (!strcmp(cmd, "BATT")) {
        uint32_t i = 0, minute, mv, pct, flags;
        while (p->batt && p->batt(i, &minute, &mv, &pct, &flags) == 0) {
            reply(p, "B %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32, minute, mv, pct, flags);
            i++;
        }
        reply(p, "END %" PRIu32, i);
    } else if (!strncmp(cmd, "TAP ", 4) && p->tap) {
        int x, y, ms = 120;
        if (sscanf(cmd + 4, "%d %d %d", &x, &y, &ms) >= 2 && x >= 0 && y >= 0 && ms > 0 && ms < 10000) {
            p->tap(x, y, ms);
            reply(p, "OK");
        } else {
            reply(p, "ERR TAP x y [ms]");
        }
    } else if (!strcmp(cmd, "SNAP") || !strncmp(cmd, "SNAP ", 5)) {
        snap(p, cmd[4] ? cmd + 5 : "");
    } else if (!strncmp(cmd, "GET ", 4) && number(cmd + 4, &a) && a >= 0 && a < p->slots) {
        get(p, (uint32_t)a);
    } else if (!strncmp(cmd, "DEL ", 4) && number(cmd + 4, &a) && a >= 0 && a < p->slots) {
        rec_clip_t clip;
        if (p->clip((uint32_t)a, &clip)) {
            reply(p, "ERR no clip in slot %lld", a);
        } else if (p->del((uint32_t)a)) {
            reply(p, "ERR flash erase");
        } else {
            reply(p, "OK");
        }
    } else if (!strncmp(cmd, "NAME ", 5) && number(cmd + 5, &a) && a >= 0 && a < p->slots && strchr(cmd + 5, ' ') &&
               number(strchr(cmd + 5, ' ') + 1, &b) && b > 0 && b <= UINT32_MAX) {
        const char *name = strchr(strchr(cmd + 5, ' ') + 1, ' ');
        if (p->set_name((uint32_t)a, (uint32_t)b, name ? name + 1 : "")) {
            reply(p, "ERR can't name clip %lld in slot %lld", b, a);
        } else {
            reply(p, "OK");
        }
    } else if (!strncmp(cmd, "PULLED ", 7) && number(cmd + 7, &a) && a >= 0 && a < p->slots && strchr(cmd + 7, ' ') &&
               number(strchr(cmd + 7, ' ') + 1, &b) && b > 0 && b <= UINT32_MAX) {
        if (p->set_pulled((uint32_t)a, (uint32_t)b)) {
            reply(p, "ERR can't mark clip %lld in slot %lld", b, a);
        } else {
            reply(p, "OK");
        }
    } else if (!strncmp(cmd, "KEEP ", 5) && number(cmd + 5, &a) && a >= 0 && a < p->slots && strchr(cmd + 5, ' ') &&
               number(strchr(cmd + 5, ' ') + 1, &b) && b > 0 && b <= UINT32_MAX &&
               strchr(strchr(cmd + 5, ' ') + 1, ' ')) {
        const char *flag = strchr(strchr(cmd + 5, ' ') + 1, ' ') + 1;
        if ((strcmp(flag, "0") && strcmp(flag, "1")) || p->set_kept((uint32_t)a, (uint32_t)b, *flag == '1')) {
            reply(p, "ERR can't keep clip %lld in slot %lld", b, a);
        } else {
            reply(p, "OK");
        }
    } else {
        reply(p, "ERR unknown command");
    }
}
