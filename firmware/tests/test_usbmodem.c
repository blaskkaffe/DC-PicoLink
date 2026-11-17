/* Host test of usbmodem_model.h: answer lines, dialled keys in voice mode, NO CARRIER in the
   data, and the dial tone table. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "usbmodem_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int lines(usbm_rx_t *r, const char *s, int *events, int cap) {
    int n = 0;
    for (; *s; s++) { int e = usbm_line_byte(r, (uint8_t)*s); if (e && n < cap) events[n++] = e; }
    return n;
}

int main(void) {
    usbm_rx_t r; int ev[8];
    usbm_init(&r);
    CHECK(lines(&r, "\r\nOK\r\n", ev, 8) == 1 && ev[0] == USBM_OK);
    CHECK(lines(&r, "ATZ0\r\r\nOK\r\n", ev, 8) == 1 && ev[0] == USBM_OK);          /* echo still on */
    CHECK(lines(&r, "\r\nCONNECT 33600/ARQ/V34\r\n", ev, 8) == 1 && ev[0] == USBM_CONNECT);
    CHECK(!strcmp(r.line, "CONNECT 33600/ARQ/V34"));
    CHECK(lines(&r, "\r\nERROR\r\n\r\nNO CARRIER\r\n\r\nBUSY\r\nRING\r\n", ev, 8) == 4 &&
          ev[0] == USBM_ERROR && ev[1] == USBM_NO_CARRIER && ev[2] == USBM_BUSY && ev[3] == USBM_RING);
    CHECK(lines(&r, "\r\nOKAY\r\nCONNECTED?\r\n", ev, 8) == 1 && ev[0] == USBM_CONNECT);
    char big[200]; memset(big, 'x', sizeof big - 1); big[sizeof big - 3] = '\r'; big[sizeof big - 2] = '\n'; big[sizeof big - 1] = 0;
    CHECK(lines(&r, big, ev, 8) == 0);
    CHECK(lines(&r, "OK\r", ev, 8) == 1);

    /* voice: plain DLE+digit, the wrapped form, DLE DLE, other codes */
    usbm_init(&r);
    const uint8_t v[] = { 0x80, 0x10, '5', 0x7f, 0x10, '/', 0x10, '~', 0x10, '1', 0x10, '~', 0x10, 0x10, '7', 0x10, 's', 0x10, '#', '9' };
    char got[8]; int g = 0;
    for (size_t i = 0; i < sizeof v; i++) { char c = usbm_voice_byte(&r, v[i]); if (c) got[g++] = c; }
    got[g] = 0;
    CHECK(!strcmp(got, "51#"));

    /* data: NO CARRIER inside PPP bytes, also after a lone CR; not without the line ends */
    usbm_init(&r);
    const char *d = "~\x7d#\r\r\nNO CARRIER\r\n";
    int hits = 0;
    for (const char *p = d; *p; p++) hits += usbm_data_byte(&r, (uint8_t)*p);
    CHECK(hits == 1);
    hits = 0;
    for (const char *p = "xxNO CARRIERxx\r\nNO CARRIE\r\n"; *p; p++) hits += usbm_data_byte(&r, (uint8_t)*p);
    CHECK(hits == 0);

    /* tone: both tones present, never DLE, repeats every 800 samples, stays in range */
    static int16_t sine[USBM_TONE_N];
    for (int k = 0; k < USBM_TONE_N; k++) sine[k] = (int16_t)lrint(sin(2 * M_PI * k / USBM_TONE_N) * 32767);
    double p350 = 0, q350 = 0, p440 = 0, q440 = 0, p400 = 0, q400 = 0;
    for (uint32_t i = 0; i < 8000; i++) {
        uint8_t s = usbm_tone_sample(sine, i);
        CHECK(s != USBM_DLE && s >= 40 && s <= 216);
        CHECK(s == usbm_tone_sample(sine, i + USBM_TONE_N));
        double x = s - 128.0;
        p350 += x * sin(2 * M_PI * 350 * i / 8000.0); q350 += x * cos(2 * M_PI * 350 * i / 8000.0);
        p440 += x * sin(2 * M_PI * 440 * i / 8000.0); q440 += x * cos(2 * M_PI * 440 * i / 8000.0);
        p400 += x * sin(2 * M_PI * 400 * i / 8000.0); q400 += x * cos(2 * M_PI * 400 * i / 8000.0);
        if (fails > 5) break;
    }
    double a350 = hypot(p350, q350) / 4000, a440 = hypot(p440, q440) / 4000, a400 = hypot(p400, q400) / 4000;
    CHECK(a350 > 35 && a350 < 45 && a440 > 35 && a440 < 45 && a400 < 2);

    printf(fails ? "usbmodem: %d FAILED\n" : "usbmodem: all ok\n", fails);
    return fails != 0;
}
