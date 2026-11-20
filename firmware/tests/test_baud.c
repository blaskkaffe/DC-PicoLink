/* Host test: the cable's wire rate equals the rate dcload-serial really sets on
   the DC, for every host request above 115200.
   gcc -O1 -I. test/test_baud.c && ./a.out */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#define DC_BAUD_MATCH 1
#include "baud_model.h"

/* dcload-serial target-src/dcload/scif.c: *SCBRR2 = (50 * 1000000) / (32 * bps) - 1; */
static double dcload_rate(unsigned int bps) {
    volatile unsigned char scbrr = (50 * 1000000) / (32 * bps) - 1;
    return 50e6 / (32.0 * (scbrr + 1));
}

int main(void) {
    unsigned fails = 0, n = 0;
    for (uint32_t req = 115201; req <= 4000000; req++, n++) {
        double want = dcload_rate(req), got = dc_effective_baud(req);
        if (fabs(got / want - 1.0) > 1e-6) { if (fails++ < 10) printf("FAIL %u: cable %.1f, DC %.1f\n", req, got, want); }
    }
    struct { uint32_t req; double rate; bool wrap; } spot[] = {
        { 230400, 260416.7, false }, { 500000, 520833.3, false }, { 781250, 781250, false },
        { 1500000, 1562500, false }, { 1562500, 1562500, false }, { 1600000, 6103.5, true }, { 3000000, 6103.5, true } };
    for (unsigned i = 0; i < sizeof spot / sizeof *spot; i++) {
        double got = dc_effective_baud(spot[i].req);
        bool ok = fabs(got - spot[i].rate) < 1 && baud_wrapped == spot[i].wrap;
        printf("%s -b %u -> %.1f baud%s\n", ok ? "PASS" : "FAIL", spot[i].req, got, baud_wrapped ? " (wrapped, warned)" : "");
        fails += !ok;
    }
    {   /* auto-baud guard: windows of 32 low-pulse widths at 250 MHz, 16 loop counts per us... widths in meter cycles */
        uint32_t bit = 250000000u / 120192u, w[32];
        for (int i = 0; i < 32; i++) w[i] = 9 * bit;                       /* all-zero bytes: only 9-bit lows */
        bool ok = !abw_usable(w, 32);
        for (int i = 0; i < 32; i++) w[i] = bit;                           /* 0xFF bytes: only 1-bit lows */
        ok = ok && !abw_usable(w, 32);
        for (int i = 0; i < 32; i++) w[i] = (1 + i % 3) * bit;             /* ordinary data: 1..3 bit lows */
        ok = ok && abw_usable(w, 32);
        for (int i = 0; i < 32; i++) w[i] = (i % 2 ? 2 : 1) * bit;         /* 0x7E flags: 1 and 2 bit lows */
        ok = ok && abw_usable(w, 32);
        printf("%s auto-baud ignores windows of equal pulses (zero bytes), accepts mixed data\n", ok ? "PASS" : "FAIL");
        fails += !ok;
    }
    {
        bool ok = ab_fraction(120192, 57870) && ab_fraction(120192, 60096) && ab_fraction(120192, 9766) && ab_fraction(1562500, 520833)
                  && !ab_fraction(120192, 115200) && !ab_fraction(1562500, 1500000) && !ab_fraction(120192, 156250)
                  && !ab_fraction(57870, 120192) && !ab_fraction(520833, 781250) && !ab_fraction(260417, 223214);
        printf("%s auto-baud is slow to follow a rate that is a whole fraction of the wire rate\n", ok ? "PASS" : "FAIL");
        fails += !ok;
    }
    {   /* follow decision: a game at 111607 (115200 asked) on a 57600 wire, bursts whose windows
           alternate between the true rate and half of it (no 1-bit pulses), framing errors now and then */
        abc_t c = {0}; uint32_t fe = 0; int followed = -1;
        for (int i = 0; i < 20 && followed < 0; i++) {
            float r = i % 2 ? 55800.0f : 111186.0f;
            if (i % 3 == 2) fe++;
            if (abc_step(&c, r, 57600, false, fe)) followed = i;
        }
        bool ok = followed >= 0 && followed < 6;
        /* the same with half-rate windows first */
        abc_t c2 = {0}; fe = 0; int f2 = -1;
        for (int i = 0; i < 20 && f2 < 0; i++) { if (i % 3 == 2) fe++; if (abc_step(&c2, i % 2 ? 111186.0f : 55800.0f, 57600, false, fe)) f2 = i; }
        ok = ok && f2 >= 0 && f2 < 7;
        /* DC at exactly twice the wire: its half-rate windows read as the wire rate and must not reset it */
        abc_t c3 = {0}; fe = 0; int f3 = -1;
        for (int i = 0; i < 20 && f3 < 0; i++) { if (i % 3 == 2) fe++; if (abc_step(&c3, i % 2 ? 57600.0f : 115200.0f, 57600, false, fe)) f3 = i; }
        ok = ok && f3 >= 0 && f3 < 7;
        /* a DC at the wire rate is never followed, whatever single odd windows say */
        abc_t c4 = {0}; bool any = false;
        for (int i = 0; i < 40; i++) any |= abc_step(&c4, i % 7 == 3 ? 28800.0f : 57600.0f, 57600, true, (uint32_t)i);
        ok = ok && !any;
        /* one clean mis-measured window cannot retune a working link (needs 4 agreeing) */
        abc_t c5 = {0}; int f5 = -1;
        for (int i = 0; i < 4 && f5 < 0; i++) if (abc_step(&c5, 120192, 115200, true, 0)) f5 = i;
        ok = ok && f5 == 3;
        printf("%s auto-baud follows a DC whose windows alternate with half its rate (followed after %d/%d/%d windows)\n",
               ok ? "PASS" : "FAIL", followed + 1, f2 + 1, f3 + 1);
        fails += !ok;
    }
    dc_effective_baud(115200);
    if (baud_wrapped) { printf("FAIL wrap flag stuck\n"); fails++; }
    printf(fails ? "FAILED: %u\n" : "OK: %u requests match dcload's DC rate\n", fails ? fails : n);
    return fails != 0;
}
