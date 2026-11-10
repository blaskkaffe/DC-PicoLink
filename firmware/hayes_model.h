/*
 * Hayes modem stub for the serial cable (host-compilable, used by the ESP32).
 *
 * The Dreamcast talks to this like to a modem: AT commands in, OK / CONNECT out. After
 * "CONNECT" the bytes are PPP. Replies follow a real modem: echo on, verbose results, CR LF
 * around words. Anything that does not start with "AT" is ignored without an answer, so
 * line noise and the start of a PPP frame never get an "ERROR" back.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define HAYES_LINE_MAX 96

typedef struct {
    char line[HAYES_LINE_MAX];
    uint8_t n;
    bool overflow;
    bool echo;        /* E1 (default) */
    bool verbose;     /* V1 (default): words; V0: numbers */
    bool quiet;       /* Q1: no result codes */
    uint32_t baud;    /* shown in "CONNECT <baud>" */
} hayes_t;

typedef enum {
    HAYES_NONE = 0,
    HAYES_DIAL,        /* "CONNECT" has been written to out: what follows is PPP */
    HAYES_PPP_FLAG,    /* a PPP flag (0x7E) with no dial first: PPP starts, give this byte to PPP */
} hayes_ev_t;

static inline void hayes_init(hayes_t *h) {
    memset(h, 0, sizeof *h);
    h->echo = true;
    h->verbose = true;
    h->baud = 33600;
}

/* append to out (cap), keeping the terminator */
static inline void hayes__put(char *out, size_t cap, size_t *o, const char *s) {
    while (*s && *o + 1 < cap) out[(*o)++] = *s++;
    out[*o] = 0;
}

static inline void hayes__result(hayes_t *h, int code, char *out, size_t cap, size_t *o) {
    static const char *const words[] = { "OK", "CONNECT", "RING", "NO CARRIER", "ERROR" };
    if (h->quiet) return;
    if (h->verbose) {
        hayes__put(out, cap, o, "\r\n");
        if (code == 1) {
            char t[24]; int k = 0; uint32_t b = h->baud; char d[12]; int dn = 0;
            do { d[dn++] = (char)('0' + b % 10); b /= 10; } while (b);
            memcpy(t, "CONNECT ", 8); k = 8;
            while (dn) t[k++] = d[--dn];
            t[k] = 0;
            hayes__put(out, cap, o, t);
        } else {
            hayes__put(out, cap, o, words[code]);
        }
        hayes__put(out, cap, o, "\r\n");
    } else {
        char t[4] = { (char)('0' + code), '\r', 0 };
        hayes__put(out, cap, o, t);
    }
}

/* "NO CARRIER" for when the PPP session ends */
static inline size_t hayes_no_carrier(hayes_t *h, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    hayes__result(h, 3, out, cap, &o);
    return o;
}

static inline char hayes__up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* run the line after "AT"; returns HAYES_DIAL when a dial/answer command was found */
static inline hayes_ev_t hayes__run(hayes_t *h, char *out, size_t cap, size_t *o) {
    const char *s = h->line;
    size_t i = 2, n = h->n;
    while (i < n) {
        char c = hayes__up(s[i++]);
        if (c == ' ') continue;
        if (c == 'D' || c == 'A') {                   /* dial / answer: the rest of the line is the number */
            hayes__result(h, 1, out, cap, o);
            return HAYES_DIAL;
        }
        if (c == '+' || c == '%' || c == '\\') break;  /* extended command: take the rest as OK */
        if (c == '&') { if (i < n) i++; }              /* &F, &C1 ... */
        int val = -1;
        while (i < n && s[i] >= '0' && s[i] <= '9') { val = (val < 0 ? 0 : val * 10) + (s[i] - '0'); i++; }
        if (c == 'S') { if (i < n && (s[i] == '=' || s[i] == '?')) { i++; while (i < n && s[i] >= '0' && s[i] <= '9') i++; } continue; }
        if (c == 'E') h->echo = val != 0;
        else if (c == 'V') h->verbose = val != 0;
        else if (c == 'Q') h->quiet = val > 0;
        else if (c == 'Z') { h->echo = true; h->verbose = true; h->quiet = false; }
    }
    hayes__result(h, 0, out, cap, o);
    return HAYES_NONE;
}

/*
 * One byte from the Dreamcast while in command mode. Text for the Dreamcast is appended to
 * out (starts empty, *olen set). Returns the event.
 */
static inline hayes_ev_t hayes_byte(hayes_t *h, uint8_t c, char *out, size_t cap, size_t *olen) {
    size_t o = 0;
    out[0] = 0;
    hayes_ev_t ev = HAYES_NONE;

    if (c == 0x7E && h->n == 0) {                       /* PPP without a dial */
        *olen = 0;
        return HAYES_PPP_FLAG;
    }
    if (h->echo && c >= 8 && c != 0x7E && c != 0x7F) {  /* echo what was typed, as a modem does */
        char e[2] = { (char)c, 0 };
        if (c != '\n') hayes__put(out, cap, &o, e);
    }
    if (c == '\r') {
        if (h->n >= 2 && !h->overflow && hayes__up(h->line[0]) == 'A' && hayes__up(h->line[1]) == 'T')
            ev = hayes__run(h, out, cap, &o);
        h->n = 0; h->overflow = false;
    } else if (c == '\n') {
        /* ignored */
    } else if (c == 8 || c == 0x7F) {
        if (h->n) h->n--;
    } else if (c >= 0x20 && c < 0x7F) {
        if (h->n < HAYES_LINE_MAX) h->line[h->n++] = (char)c; else h->overflow = true;
    }
    *olen = o;
    return ev;
}
