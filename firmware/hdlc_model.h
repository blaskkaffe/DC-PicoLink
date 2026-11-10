/*
 * PPP frame filter for the bytes coming from the Dreamcast (host-compilable, used by the ESP32).
 *
 * Does what DreamPi does with iptables, before the frames reach lwIP:
 *   - TTL: every IPv4 packet leaves with TTL 64 (lwIP then forwards it at 63; DreamPi's
 *     "-t mangle FORWARD -i ppp0 -j TTL --ttl-set 64" gives 64 on the wire, so we set 65 here)
 *   - PowerSmash: a TCP SYN from source port 3200-3205 is dropped if another SYN to the same
 *     destination address was seen less than a second ago (the game sends every SYN twice).
 *     Same as DreamPi's "recent" rule: a dropped SYN also renews the one-second timer.
 *   - bad FCS: dropped (lwIP would do the same)
 *
 * Every frame is decoded, looked at, and encoded again with every control character escaped
 * (valid under any ACCM). Frames that are not IPv4 pass unchanged in content.
 * "AT" as the first two bytes of a frame (or before the first flag) means the Dreamcast is
 * talking to a modem again (reset, or a game that hung up without PPP shutdown): saw_at is set.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define HDLC_MAX_FRAME  1700            /* decoded bytes incl. FCS; PPP MRU is 1500 */
#define HDLC_SYN_SLOTS  8
#define HDLC_TTL_OUT    65

typedef void (*hdlc_emit_fn)(void *ctx, const uint8_t *p, size_t n);

typedef struct {
    uint8_t buf[HDLC_MAX_FRAME];
    uint16_t n;
    bool in, esc, overflow;
    uint8_t stray_prev;                  /* last byte seen outside a frame */
    bool saw_at;                         /* "AT" seen where a frame should start (cleared by the reader) */
    struct { uint32_t dst; uint32_t ms; bool used; } syn[HDLC_SYN_SLOTS];
    /* counters */
    uint32_t frames, ip_frames, ttl_fixed, syn_dropped, fcs_bad, too_long, aborted;
    hdlc_emit_fn emit; void *ctx;
} hdlc_t;

static inline void hdlc_init(hdlc_t *h, hdlc_emit_fn emit, void *ctx) {
    memset(h, 0, sizeof *h);
    h->emit = emit; h->ctx = ctx;
}

/* ---- FCS-16 (RFC 1662) ---- */
static inline uint16_t hdlc_fcs(uint16_t fcs, const uint8_t *p, size_t n) {
    while (n--) {
        fcs ^= *p++;
        for (int k = 0; k < 8; k++) fcs = (fcs & 1) ? (uint16_t)((fcs >> 1) ^ 0x8408) : (uint16_t)(fcs >> 1);
    }
    return fcs;
}

/* flag + escaped body (FCS appended here) + flag; out must hold 2*(n+2)+2. Returns length. */
static inline size_t hdlc_encode(const uint8_t *p, size_t n, uint8_t *out) {
    uint16_t fcs = (uint16_t)(hdlc_fcs(0xFFFF, p, n) ^ 0xFFFF);
    size_t o = 0;
    out[o++] = 0x7E;
    for (size_t i = 0; i < n + 2; i++) {
        uint8_t c = i < n ? p[i] : (i == n ? (uint8_t)(fcs & 0xFF) : (uint8_t)(fcs >> 8));
        if (c < 0x20 || c == 0x7D || c == 0x7E) { out[o++] = 0x7D; out[o++] = c ^ 0x20; }
        else out[o++] = c;
    }
    out[o++] = 0x7E;
    return o;
}

static inline uint16_t hdlc__ipsum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* the one-second "recent" list; true = drop this SYN */
static inline bool hdlc__syn_drop(hdlc_t *h, uint32_t dst, uint32_t now_ms) {
    int free_i = -1, old_i = 0;
    for (int i = 0; i < HDLC_SYN_SLOTS; i++) {
        if (h->syn[i].used && h->syn[i].dst == dst) {
            bool recent = (uint32_t)(now_ms - h->syn[i].ms) < 1000;
            h->syn[i].ms = now_ms;               /* --update and --set both refresh the time */
            return recent;
        }
        if (!h->syn[i].used) free_i = i;
        else if ((uint32_t)(now_ms - h->syn[i].ms) > (uint32_t)(now_ms - h->syn[old_i].ms)) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    h->syn[i].used = true; h->syn[i].dst = dst; h->syn[i].ms = now_ms;
    return false;
}

static inline void hdlc__frame(hdlc_t *h, uint32_t now_ms) {
    uint8_t *b = h->buf;
    size_t n = h->n;
    if (n < 4 + 2) return;
    if (hdlc_fcs(0xFFFF, b, n) != 0xF0B8) { h->fcs_bad++; return; }
    n -= 2;                                       /* drop the FCS */
    h->frames++;

    size_t p = 0;
    if (b[0] == 0xFF && b[1] == 0x03) p = 2;      /* address/control may be compressed away */
    uint16_t proto;
    if (p < n && (b[p] & 1)) { proto = b[p]; p += 1; }              /* protocol field compression */
    else if (p + 1 < n) { proto = (uint16_t)(b[p] << 8 | b[p + 1]); p += 2; }
    else return;

    if (proto == 0x0021 && n - p >= 20 && (b[p] >> 4) == 4) {
        uint8_t *ip = b + p;
        size_t ihl = (size_t)(ip[0] & 15) * 4;
        if (ihl >= 20 && ihl <= n - p) {
            h->ip_frames++;
            /* PowerSmash: SYN from 3200-3205 */
            if (ip[9] == 6 && ((ip[6] & 0x1F) | ip[7]) == 0 && n - p >= ihl + 14) {
                const uint8_t *t = ip + ihl;
                uint16_t sport = (uint16_t)(t[0] << 8 | t[1]);
                uint8_t fl = t[13];
                if (sport >= 3200 && sport <= 3205 && (fl & 0x17) == 0x02) {   /* SYN, no ACK/RST/FIN */
                    uint32_t dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
                    if (hdlc__syn_drop(h, dst, now_ms)) { h->syn_dropped++; return; }
                }
            }
            if (ip[8] != HDLC_TTL_OUT) {
                ip[8] = HDLC_TTL_OUT;
                ip[10] = ip[11] = 0;
                uint16_t c = hdlc__ipsum(ip, ihl);
                ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
                h->ttl_fixed++;
            }
        }
    }
    uint8_t out[2 * (HDLC_MAX_FRAME + 2) + 2];
    size_t k = hdlc_encode(b, n, out);
    h->emit(h->ctx, out, k);
}

/* feed raw bytes from the Dreamcast; now_ms is any millisecond clock */
static inline void hdlc_feed(hdlc_t *h, const uint8_t *p, size_t len, uint32_t now_ms) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c == 0x7E) {
            if (h->in && h->esc) { h->aborted++; }                 /* 0x7D 0x7E = abort */
            else if (h->in && h->n && !h->overflow) hdlc__frame(h, now_ms);
            h->in = true; h->esc = false; h->n = 0; h->overflow = false;
            h->stray_prev = 0;
            continue;
        }
        if (!h->in) {                                              /* between frames: only flags are normal */
            if ((h->stray_prev == 'A' || h->stray_prev == 'a') && (c == 'T' || c == 't')) h->saw_at = true;
            h->stray_prev = c;
            continue;
        }
        if (c == 0x7D) { h->esc = true; continue; }
        if (h->esc) { c ^= 0x20; h->esc = false; }                 /* raw control bytes are data (ACCM 0); a bad FCS catches noise */
        if (h->n >= HDLC_MAX_FRAME) { if (!h->overflow) h->too_long++; h->overflow = true; continue; }
        h->buf[h->n++] = c;
        if (h->n == 2 && (h->buf[0] | 0x20) == 'a' && (h->buf[1] | 0x20) == 't') h->saw_at = true;
    }
}
