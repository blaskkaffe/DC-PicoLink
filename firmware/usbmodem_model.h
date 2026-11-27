/*
 * What a USB voice modem says, sorted out (the ESP32's USB modem port: esp32/main/usbmodem.c).
 * The Dreamcast's own modem calls the USB modem over a phone cord, as with a DreamPi:
 *   1. voice mode, dial tone out; the modem reports each tone the Dreamcast dials as DLE + digit
 *   2. after the last digit: ATA, wait for CONNECT
 *   3. the bytes are PPP until "NO CARRIER"
 * Three small parsers, one per phase. Host-compilable (tests/test_usbmodem.c).
 */
#ifndef USBMODEM_MODEL_H
#define USBMODEM_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define USBM_DLE 0x10
#define USBM_ETX 0x03

enum { USBM_NONE = 0, USBM_OK, USBM_CONNECT, USBM_ERROR, USBM_NO_CARRIER, USBM_BUSY, USBM_RING };

typedef struct {
    char line[64]; uint8_t n;       /* command phase: the line being received */
    bool dle;                       /* voice phase: the last byte was DLE */
    uint8_t nc;                     /* data phase: how much of "\r\nNO CARRIER\r\n" matched */
} usbm_rx_t;

static inline void usbm_init(usbm_rx_t *r) { memset(r, 0, sizeof *r); }

/* Command phase: one byte of the modem's answer. Returns an event when a whole line says so.
   The finished line (for the log) stays in r->line until the next byte. */
static inline int usbm_line_byte(usbm_rx_t *r, uint8_t b) {
    if (b != '\r' && b != '\n') {
        if (r->n == sizeof r->line - 1) return USBM_NONE;       /* too long: keep the start */
        if (r->n == 0) r->line[0] = 0;
        r->line[r->n++] = (char)b; r->line[r->n] = 0;
        return USBM_NONE;
    }
    if (!r->n) return USBM_NONE;
    r->n = 0;
    const char *l = r->line;
    if (!strcmp(l, "OK")) return USBM_OK;
    if (!strncmp(l, "CONNECT", 7)) return USBM_CONNECT;
    if (!strcmp(l, "ERROR")) return USBM_ERROR;
    if (!strcmp(l, "NO CARRIER") || !strcmp(l, "NO ANSWER") || !strcmp(l, "NO DIALTONE") || !strcmp(l, "NO DIAL TONE"))
        return USBM_NO_CARRIER;
    if (!strcmp(l, "BUSY")) return USBM_BUSY;
    if (!strcmp(l, "RING")) return USBM_RING;
    return USBM_NONE;
}

/* Voice phase: returns the dialled key ('0'-'9', '*', '#', 'A'-'D') when the modem reports one, else 0.
   Modems send DLE+key; newer ones wrap it as DLE '/' DLE '~' DLE key DLE '~' (same pairs). */
static inline char usbm_voice_byte(usbm_rx_t *r, uint8_t b) {
    if (!r->dle) { r->dle = b == USBM_DLE; return 0; }
    r->dle = false;
    if ((b >= '0' && b <= '9') || b == '*' || b == '#' || (b >= 'A' && b <= 'D')) return (char)b;
    return 0;                                                    /* DLE DLE (data), busy, silence, ... */
}

/* Data phase: true when the modem has just said NO CARRIER (the call is over). */
static inline bool usbm_data_byte(usbm_rx_t *r, uint8_t b) {
    static const char pat[] = "\r\nNO CARRIER\r\n";
    if (b == (uint8_t)pat[r->nc]) { if (++r->nc == sizeof pat - 1) { r->nc = 0; return true; } }
    else r->nc = b == '\r';
    return false;
}

/* Dial tone (350 + 440 Hz) as 8-bit unsigned samples at 8000 Hz. The two tones repeat together
   every 800 samples, so one table of 800 is the whole tone. `sine` = sin(2*pi*k/800) * 32767.
   No sample is DLE, which would need doubling on the way to the modem. */
#define USBM_TONE_N 800
static inline uint8_t usbm_tone_sample(const int16_t *sine, uint32_t i) {
    int v = 128 + (sine[(i * 35u) % USBM_TONE_N] * 40 + sine[(i * 44u) % USBM_TONE_N] * 40) / 32767;
    return (uint8_t)(v == USBM_DLE ? USBM_DLE + 1 : v);
}
/* ---- modems other than the one this was first written for ---- */

/* Pick the voice mode from the answer to AT+VSM=? : 8-bit unsigned/linear samples at 8000 Hz (what the
   dial tone table is). Conexant lists `1,"8-BIT UNSIGNED PCM",(8000),(0),(0)`, Rockwell-style modems
   `128,"8-BIT LINEAR",(7200,8000,11025),(0),(0)`. Writes "AT+VSM=<mode>,8000"; false if no line fits. */
static inline bool usbm_vsm_pick(const char *txt, char *out, size_t cap) {
    int best = 0, bestmode = -1;
    const char *p = txt;
    while (*p) {
        const char *e = p; while (*e && *e != '\r' && *e != '\n') e++;
        char l[96]; size_t n = (size_t)(e - p); if (n >= sizeof l) n = sizeof l - 1;
        memcpy(l, p, n); l[n] = 0;
        p = *e ? e + 1 : e;
        char *q = l; while (*q == ' ') q++;
        if (*q < '0' || *q > '9') continue;
        int mode = 0; while (*q >= '0' && *q <= '9') { mode = mode * 10 + (*q++ - '0'); if (mode > 9999) break; }
        if (*q != ',' || mode > 9999) continue;
        char *a = strchr(q, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!b) continue;
        *b = 0;
        char name[64]; size_t k = 0;
        for (const char *c = a + 1; *c && k < sizeof name - 1; c++) name[k++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
        name[k] = 0;
        if (!strstr(b + 1, "8000")) continue;
        if (!strstr(name, "8-BIT") && !strstr(name, "8 BIT")) continue;
        if (strstr(name, "16") || strstr(name, "ADPCM") || strstr(name, "LAW")) continue;
        int score = strstr(name, "UNSIGNED") ? 3 : strstr(name, "LINEAR") ? 2 : strstr(name, "PCM") ? 1 : 0;
        if (strstr(name, "SIGNED") && !strstr(name, "UNSIGNED")) score = 0;
        if (score > best) { best = score; bestmode = mode; }
    }
    if (bestmode < 0) return false;
    int w = snprintf(out, cap, "AT+VSM=%d,8000", bestmode);
    return w > 0 && (size_t)w < cap;
}

/* The first useful line of an answer (not the echo, not OK/ERROR). Empty if none. */
static inline void usbm_first_line(const char *txt, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    const char *p = txt;
    while (*p) {
        const char *e = p; while (*e && *e != '\r' && *e != '\n') e++;
        size_t n = (size_t)(e - p);
        bool echo = n >= 2 && (p[0] == 'A' || p[0] == 'a') && (p[1] == 'T' || p[1] == 't');
        if (n && !echo && !(n == 2 && !strncmp(p, "OK", 2)) && !(n == 5 && !strncmp(p, "ERROR", 5))) {
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n); out[n] = 0;
            return;
        }
        p = *e ? e + 1 : e;
    }
}
#endif
