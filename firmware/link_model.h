/*
 * DC-PicoLink <-> ESP32 link: framing shared by both firmwares (host-compilable).
 *
 * One UART, hardware flow control (RTS/CTS), carries packets for several channels:
 *
 *   packet  = magic 0xDC | version 1 | channel | seq | payload (0..LINK_MAX_PAYLOAD) | crc32 (LE)
 *   on wire = COBS(packet) 0x00
 *
 * crc32 is the usual reflected CRC-32 (zlib, poly 0xEDB88320) over magic..payload.
 * seq counts packets per sender (mod 256); a gap means packets were lost.
 * A receiver ignores packets with an unknown version or channel (old/new mix).
 *
 * Channels:
 *   LINK_CH_API   dc1 text messages (04-control-channel.md): hello, get/set link.*
 *   LINK_CH_STAT  one status line (plain English, no newline), cable -> ESP32
 *   LINK_CH_SER   coder's-cable bytes, both directions
 *   LINK_CH_ETH   one Ethernet frame (W5500 MACRAW), both directions
 *   LINK_CH_SD    reserved for SD blocks (until the board's own SD slot)
 *
 * Neither side needs DMA to be lossless: a receiver that can't take a packet yet
 * stops reading the UART, its RX FIFO fills, RTS drops and the sender's UART waits.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define LINK_MAGIC        0xDC
#define LINK_VERSION      1
#define LINK_MAX_PAYLOAD  1536u
#define LINK_HDR          4u
#define LINK_CRC          4u
#define LINK_MAX_PACKET   (LINK_HDR + LINK_MAX_PAYLOAD + LINK_CRC)
/* COBS adds one byte per 254 (rounded up) plus the leading code; plus the 0x00 */
#define LINK_MAX_WIRE     (LINK_MAX_PACKET + LINK_MAX_PACKET / 254u + 2u)
#define LINK_BAUD         3000000u

enum { LINK_CH_API = 0, LINK_CH_STAT = 1, LINK_CH_SER = 2, LINK_CH_ETH = 3, LINK_CH_SD = 4, LINK_CH_COUNT };

static inline uint32_t link_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* COBS-encode n bytes from src into dst (room for n + n/254 + 1), no trailing zero.
   Returns the encoded length. */
static inline size_t link_cobs_encode(const uint8_t *src, size_t n, uint8_t *dst) {
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == 0) {
            dst[code_at] = code; code_at = o++; code = 1;
        } else {
            dst[o++] = src[i];
            if (++code == 0xFF) { dst[code_at] = code; code_at = o++; code = 1; }
        }
    }
    dst[code_at] = code;
    return o;
}

/* incremental COBS encoder writing straight into the output (no packet-sized
   temporary: the RP2040's core0 stack is 2 KB) */
typedef struct { uint8_t *out; size_t code_at, o; uint8_t code; } link_enc_t;
static inline void link_enc_start(link_enc_t *e, uint8_t *out) { e->out = out; e->code_at = 0; e->o = 1; e->code = 1; }
static inline void link_enc_byte(link_enc_t *e, uint8_t b) {
    if (b == 0) { e->out[e->code_at] = e->code; e->code_at = e->o++; e->code = 1; return; }
    e->out[e->o++] = b;
    if (++e->code == 0xFF) { e->out[e->code_at] = e->code; e->code_at = e->o++; e->code = 1; }
}
static inline size_t link_enc_end(link_enc_t *e) { e->out[e->code_at] = e->code; e->out[e->o] = 0; return e->o + 1; }

/* Build a whole wire packet (COBS + 0x00) into out[LINK_MAX_WIRE]. 0 if too long. */
static inline size_t link_build(uint8_t ch, uint8_t seq, const void *payload, size_t n, uint8_t *out) {
    if (n > LINK_MAX_PAYLOAD) return 0;
    const uint8_t hdr[LINK_HDR] = { LINK_MAGIC, LINK_VERSION, ch, seq };
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t c = link_crc32(link_crc32(0, hdr, LINK_HDR), p, n);
    link_enc_t e; link_enc_start(&e, out);
    for (size_t i = 0; i < LINK_HDR; i++) link_enc_byte(&e, hdr[i]);
    for (size_t i = 0; i < n; i++) link_enc_byte(&e, p[i]);
    for (int i = 0; i < 4; i++) link_enc_byte(&e, (uint8_t)(c >> (8 * i)));
    return link_enc_end(&e);
}

/* ---- streaming receiver -------------------------------------------------- */
typedef struct {
    uint8_t buf[LINK_MAX_PACKET];  /* decoded bytes of the packet being received */
    size_t  len;
    uint8_t code, left;            /* COBS: current block code and bytes left in it */
    bool    skip;                  /* overlong or broken: drop until the next 0x00 */
    /* result of the last complete packet (valid while link_rx_byte returned true) */
    uint8_t ch, seq;
    const uint8_t *payload;
    size_t  n;
    /* counters */
    uint32_t good, crc_bad, too_long, cobs_bad, bad_hdr, lost;
    uint8_t  next_seq;
    bool     seen;
} link_rx_t;

static inline void link_rx_init(link_rx_t *r) { memset(r, 0, sizeof *r); }

/* Feed one wire byte. Returns true when a good packet is complete; its fields are
   then in r->ch, r->seq, r->payload, r->n (valid until the next call). */
static inline bool link_rx_byte(link_rx_t *r, uint8_t b) {
    if (b == 0) {                                   /* end of packet */
        bool ok = false;
        if (!r->skip && r->len) {
            if (r->left) r->cobs_bad++;            /* block cut short */
            else if (r->len < LINK_HDR + LINK_CRC) r->bad_hdr++;
            else {
                size_t L = r->len - LINK_CRC;
                uint32_t c = (uint32_t)r->buf[L] | (uint32_t)r->buf[L + 1] << 8 |
                             (uint32_t)r->buf[L + 2] << 16 | (uint32_t)r->buf[L + 3] << 24;
                if (c != link_crc32(0, r->buf, L)) r->crc_bad++;
                else if (r->buf[0] != LINK_MAGIC || r->buf[1] != LINK_VERSION || r->buf[2] >= LINK_CH_COUNT) r->bad_hdr++;
                else {
                    r->ch = r->buf[2]; r->seq = r->buf[3];
                    if (r->seen && r->seq != r->next_seq) r->lost += (uint8_t)(r->seq - r->next_seq);
                    r->seen = true; r->next_seq = (uint8_t)(r->seq + 1);
                    r->payload = r->buf + LINK_HDR; r->n = L - LINK_HDR;
                    r->good++;
                    ok = true;
                }
            }
        }
        r->len = 0; r->code = 0; r->left = 0; r->skip = false;
        return ok;
    }
    if (r->skip) return false;
    if (r->left == 0) {                             /* a new COBS block starts */
        if (r->code && r->code != 0xFF) {           /* the previous block implied a zero */
            if (r->len >= sizeof r->buf) { r->too_long++; r->skip = true; return false; }
            r->buf[r->len++] = 0;
        }
        r->code = b; r->left = (uint8_t)(b - 1);
        return false;
    }
    if (r->len >= sizeof r->buf) { r->too_long++; r->skip = true; return false; }
    r->buf[r->len++] = b;
    r->left--;
    return false;
}

/* ---- dc1 text helpers (both sides) ---------------------------------------
   Messages are lines separated by '\n'. link_next_line() walks them in place. */
static inline bool link_next_line(const char **p, const char *end, const char **line, size_t *len) {
    if (*p >= end) return false;
    const char *s = *p, *e = s;
    while (e < end && *e != '\n') e++;
    *line = s; *len = (size_t)(e - s);
    *p = e < end ? e + 1 : e;
    return true;
}

/* true if line[0..len) starts with the word w followed by ' ' or the end */
static inline bool link_word(const char *line, size_t len, const char *w, const char **rest) {
    size_t k = strlen(w);
    if (len < k || memcmp(line, w, k) != 0) return false;
    if (len > k && line[k] != ' ') return false;
    if (rest) *rest = len > k ? line + k + 1 : line + k;
    return true;
}
