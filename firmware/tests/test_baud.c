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
    dc_effective_baud(115200);
    if (baud_wrapped) { printf("FAIL wrap flag stuck\n"); fails++; }
    printf(fails ? "FAILED: %u\n" : "OK: %u requests match dcload's DC rate\n", fails ? fails : n);
    return fails != 0;
}
