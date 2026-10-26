/* dctest wire protocol: shared by the Dreamcast program and the host-side selftest
   build. Python mirror: dctest.py. Frame (little endian):
     D3 7C | type | flags | seq u32 | len u16 | payload | crc32 u32 (type..payload) */
#pragma once
#include <stdint.h>
#include <string.h>

#define DT_S0 0xD3
#define DT_S1 0x7C
#define DT_HDR 10
#define DT_MAXPAY 2048
#define DT_MAXFRAME (DT_HDR + DT_MAXPAY + 4)

enum {
    DT_HELLO = 1,    /* reply: "dctest <version>" */
    DT_ECHO = 2,     /* reply: same flags/seq/payload */
    DT_SINK = 3,     /* no reply; counted, checked against the pattern unless flags & 0x80 */
    DT_STATS = 4,    /* reply: 13 x u32 */
    DT_CLEAR = 5,    /* payload u32 sink seed; zeroes the counters */
    DT_SRC = 6,      /* u32 count, u16 len, u8 pattern, u8 0, u32 seed, u32 interval_us -> DT_DATA x count */
    DT_SETBAUD = 7,  /* u32 baud; reply at the old rate, then switch; reverts after 5 s without a frame */
    DT_DATA = 8,     /* DC -> host, from DT_SRC */
    DT_NETSTART = 9, /* u16 port: leave serial, bring up the W5500, serve UDP on that port */
    DT_NETSTOP = 10, /* (over UDP/TCP) leave the network, back to serial */
    DT_TCPSTART = 11,/* (over UDP) u16 port: accept one TCP connection, same protocol on the stream */
    DT_TRACE = 13,   /* -> u32 DC clock (us), u32 events so far, then up to 32 x {u32 t_us, u16 event, u16 arg}, oldest first */
    DT_PROBE = 12,   /* u8 rerun -> 8 bytes: W5500 version reg, SD R1, dcload type at start, flags, u32 baud */
    DT_REPLY = 0x80
};

enum { DT_PAT_RANDOM, DT_PAT_ZERO, DT_PAT_ONES, DT_PAT_ALT, DT_PAT_RAMP, DT_PAT_WALK,
       DT_PAT_PPP, DT_PAT_EDGES, DT_PATTERNS };

static uint32_t dt_crc_tab[256];
static int dt_crc_ready;
static uint32_t dt_crc32(const uint8_t *p, int n) {          /* same as zlib.crc32 */
    if (!dt_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            dt_crc_tab[i] = c;
        }
        dt_crc_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = dt_crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void dt_put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t dt_get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Payload for (pattern, seed, seq): both sides regenerate it, so nothing is stored. */
static void dt_pattern(int pat, uint32_t seed, uint32_t seq, int len, uint8_t *out) {
    static const uint8_t edges[6] = {0x00, 0xFF, 0x80, 0x01, 0x7F, 0xFE};
    uint32_t x = (seed ^ (seq * 0x9E3779B1u)) | 0;
    if (!x) x = 1;
    for (int i = 0; i < len; i++) {
        uint8_t b;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        uint8_t r = x >> 8;
        switch (pat) {
        case DT_PAT_RANDOM: b = r; break;
        case DT_PAT_ZERO: b = 0x00; break;
        case DT_PAT_ONES: b = 0xFF; break;
        case DT_PAT_ALT: b = ((i + seq) & 1) ? 0xAA : 0x55; break;
        case DT_PAT_RAMP: b = (i + seq) & 0xFF; break;
        case DT_PAT_WALK: b = 1u << ((i + seq) & 7); break;
        case DT_PAT_PPP: b = r < 48 ? 0x7E : r < 80 ? 0x7D : r < 96 ? 0x11 : r < 112 ? 0x13 : r; break;
        default: b = edges[(i + seq) % 6]; break;
        }
        out[i] = b;
    }
}

static int dt_encode(uint8_t *o, int type, int flags, uint32_t seq, const uint8_t *pay, int len) {
    o[0] = DT_S0; o[1] = DT_S1; o[2] = type; o[3] = flags;
    dt_put32(o + 4, seq); o[8] = len; o[9] = len >> 8;
    if (len) memcpy(o + DT_HDR, pay, len);
    dt_put32(o + DT_HDR + len, dt_crc32(o + 2, 8 + len));
    return DT_HDR + len + 4;
}

typedef struct { int type, flags, len; uint32_t seq; const uint8_t *pay; } dt_frame;
typedef struct {
    uint8_t buf[DT_MAXFRAME];
    int n, pending;               /* pending: bytes of the frame just returned, dropped on the next call */
    uint32_t crc_bad, resync;     /* bad frames; bytes skipped outside frames */
} dt_parser;

static void dt_drop(dt_parser *p, int k) { memmove(p->buf, p->buf + k, p->n - k); p->n -= k; }

/* 1 if p->buf holds a complete valid frame (filled into f), else 0 */
static int dt_scan(dt_parser *p, dt_frame *f) {
    for (;;) {
        if (p->n >= 1 && p->buf[0] != DT_S0) { dt_drop(p, 1); p->resync++; continue; }
        if (p->n >= 2 && p->buf[1] != DT_S1) { dt_drop(p, 1); p->resync++; continue; }
        if (p->n < DT_HDR) return 0;
        int len = p->buf[8] | p->buf[9] << 8;
        if (len > DT_MAXPAY) { p->crc_bad++; dt_drop(p, 1); p->resync++; continue; }
        int total = DT_HDR + len + 4;
        if (p->n < total) return 0;
        if (dt_crc32(p->buf + 2, 8 + len) != dt_get32(p->buf + DT_HDR + len)) {
            p->crc_bad++; dt_drop(p, 1); p->resync++; continue;
        }
        f->type = p->buf[2]; f->flags = p->buf[3]; f->seq = dt_get32(p->buf + 4);
        f->len = len; f->pay = p->buf + DT_HDR;
        p->pending = total;
        return 1;
    }
}
static int dt_poll(dt_parser *p, dt_frame *f) {                /* next frame already buffered? */
    if (p->pending) { dt_drop(p, p->pending); p->pending = 0; }
    return dt_scan(p, f);
}
static int dt_feed(dt_parser *p, uint8_t c, dt_frame *f) {
    if (p->pending) { dt_drop(p, p->pending); p->pending = 0; }
    if (p->n == 0 && c != DT_S0) { p->resync++; return 0; }
    p->buf[p->n++] = c;
    return dt_scan(p, f);
}
