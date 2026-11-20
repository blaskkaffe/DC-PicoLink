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

/* The follow decision for one usable window measured at `rate` (autobaud_task). A window can only
   come out too slow, never too fast (a window without 1-bit pulses reads 1/2, 1/3 ... of the rate),
   so a window at a whole fraction of the candidate is ignored instead of starting a new candidate,
   and a faster window whose fraction the candidate is replaces it. Without that a game that sends
   short bursts at 115200 to a wire at 57600 gave windows at ~111600 and ~55800 in turn: each reset
   the other, and the cable never followed (serial over the cable, Nov 2026). */
typedef struct { float cand; uint32_t n, fe_mark; } abc_t;
static bool abc_step(abc_t *c, float rate, float wire, bool clean, uint32_t fe_count) {
    bool below = c->cand != 0 && rate < c->cand && ab_fraction(c->cand, rate);
    if (below) return false;                                    /* a slow-looking window of the same DC */
    float off = rate / wire - 1.0f; if (off < 0) off = -off;
    if (off < 0.025f) { c->cand = 0; c->n = 0; return false; }  /* the wire is right */
    if (c->cand != 0 && rate > c->cand && ab_fraction(rate, c->cand)) { c->cand = rate; c->n = 1; return false; }
    float d = c->cand != 0 ? rate / c->cand - 1.0f : 1.0f; if (d < 0) d = -d;
    if (d > 0.015f) { c->cand = rate; c->n = 1; c->fe_mark = fe_count; return false; }
    c->n++;
    bool errors = fe_count != c->fe_mark;
    if (ab_fraction(wire, rate) && c->n < 16) return false;    /* 1/2, 1/3 ... of the wire rate: be sure first */
    if (!(errors && c->n >= 2) && !(clean && c->n >= 4)) return false;
    c->cand = 0; c->n = 0;
    return true;
}
