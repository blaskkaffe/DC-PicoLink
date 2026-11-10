/*
 * PPP frame filter for the bytes coming from the Dreamcast (host-compilable, used by the ESP32).
 *
 * Every frame is decoded, its IPv4 packet goes through ipfilter_model.h (TTL 64, PowerSmash
 * double SYN), and it is encoded again with every control character escaped (valid under any
 * ACCM). Bad FCS: dropped (lwIP would do the same). Frames that are not IPv4 pass unchanged in content.
 * "AT" as the first two bytes of a frame (or before the first flag) means the Dreamcast is
 * talking to a modem again (reset, or a game that hung up without PPP shutdown): saw_at is set.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ipfilter_model.h"

#define HDLC_MAX_FRAME  1700            /* decoded bytes incl. FCS; PPP MRU is 1500 */

typedef void (*hdlc_emit_fn)(void *ctx, const uint8_t *p, size_t n);

typedef struct {
    uint8_t buf[HDLC_MAX_FRAME];
    uint16_t n;
    bool in, esc, overflow;
    uint8_t stray_prev;                  /* last byte seen outside a frame */
    bool saw_at;                         /* "AT" seen where a frame should start (cleared by the reader) */
    ipf_t ipf;                           /* TTL / SYN rules and their counters */
    /* counters */
    uint32_t frames, fcs_bad, too_long, aborted;
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

    if (proto == 0x0021 && !ipf_packet(&h->ipf, b + p, n - p, now_ms)) return;

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
