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

/* One window of 32 low-pulse widths (system clocks) -> the DC's rate. false: not usable (equal pulses,
   too few 1-bit pulses, or the pulses are not whole bit counts). clean: >= 25% of them are 1-bit. */
static bool abw_measure(const uint32_t *win, float clk_hz, float *rate, bool *clean) {
    if (!abw_usable(win, 32)) return false;        /* all pulses equal: a run of zero bytes looks like a slow DC */
    uint32_t m = 0xFFFFFFFFu;
    for (int i = 0; i < 32; i++) if (win[i] < m) m = win[i];
    uint64_t sum = 0; uint32_t cnt = 0;
    for (int i = 0; i < 32; i++) if (win[i] <= m + m / 3) { sum += win[i]; cnt++; }
    if (cnt < 4) return false;
    float bit = (float)sum / (float)cnt;
    uint32_t fit = 0;                              /* pulses ~ whole bit counts? */
    for (int i = 0; i < 32; i++) {
        float q = (float)win[i] / bit, f = q - (float)(int)(q + 0.5f);
        if (f < 0) f = -f;
        if (f < 0.2f) fit++;
    }
    if (fit < 26) return false;
    *rate = clk_hz / bit;
    *clean = cnt >= 8;
    return true;
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

/* The nearest rate the Dreamcast's SCIF can make (Pclk/(32 * 4^n * (N+1)), Pclk 50 MHz), within 2 %. */
static float snap_dc_rate(float r) {
    float best = r, err = 0.02f;
    for (int n = 0; n < 4; n++) {
        float base = 50e6f / (32.0f * (float)(1 << (2 * n)));
        int k = (int)(base / r + 0.5f);
        for (int kk = k - 1; kk <= k + 1; kk++) {
            if (kk < 1 || kk > 256) continue;
            float c = base / (float)kk, e = (c - r) / r;
            if (e < 0) e = -e;
            if (e < err) { err = e; best = c; }
        }
    }
    return best;
}
