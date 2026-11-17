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
#endif
