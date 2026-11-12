/* Wire rate for a host baud request (see main.c). Host-testable: test/test_baud.c.
   Needs DC_BAUD_MATCH, <stdint.h>, <stdbool.h>. */
#pragma once
/* Above 115200 the rate dcload-serial really sets on the DC. dcload's scif_init:
   SCBRR = 50000000 / (32 * bps) - 1, integer math, stored in the 8-bit SCBRR.
   Above 1562500 that is 0 - 1, which wraps to 255: the DC runs at 6103 baud
   (e.g. -b 1600000 from CP2102N guides). The cable follows that too, so it
   always matches the DC, and logs a hint (see autobaud_task). */
static bool baud_wrapped;
static float dc_effective_baud(uint32_t req) {
#if DC_BAUD_MATCH
    if (req > 115200) {                          /* <= 115200 stays exact, like FTDI/CP210x */
        uint8_t scbrr = (uint8_t)(1562500u / req - 1u);
        baud_wrapped = req > 1562500u;
        return 1562500.0f / (float)(scbrr + 1u);
    }
#endif
    baud_wrapped = false;
    return (float)req;
}

/* Auto-baud guard (autobaud_task). The meter only measures LOW periods, so a window in which every
   pulse has the same width cannot tell a bit time from a run of bits: all-zero bytes are 9-bit lows
   (120192 baud would look like 13355), 0xFF bytes are 1-bit lows. A window is usable only if its
   pulse widths vary (the longest at least 1.5x the shortest). */
static bool abw_usable(const uint32_t *w, int n) {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < n; i++) { if (w[i] < lo) lo = w[i]; if (w[i] > hi) hi = w[i]; }
    return n > 0 && hi * 2u >= lo * 3u;
}

/* A measured rate that is (nearly) a whole fraction of the current wire rate (1/2, 1/3, ... 1/12 ...) is
   usually a window without 1-bit pulses (the meter only sees LOW periods), not a Dreamcast that changed
   speed: the cable follows such a candidate only after many agreeing windows. 115200 vs 120192, 1.5 Mbaud
   vs 1.5625 Mbaud and other small differences are not affected. */
static bool ab_fraction(float wire, float rate) {
    if (rate <= 0 || wire <= rate) return false;
    float k = wire / rate, r = (float)(int)(k + 0.5f);
    if (r < 2.0f) return false;
    float d = k > r ? k - r : r - k;
    return d < 0.06f * r;
}
