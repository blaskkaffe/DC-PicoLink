// Host test of esp32/probe/main/dcspi_bb.h: the logic of the bit-banged W5500-style slave
// (frame parsing, MISO bit order and timing rule, frame ends, aborted frames). Timing on the
// real chip is what the probe firmware measures; this only proves the logic.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXT (1 << 24)
static uint8_t wave[MAXT];              // per tick: bit0 clock, bit1 MOSI, bit2 chip select
static uint32_t wlen, now;
static int miso = 1;
static volatile int sim_run = 1;
static uint8_t sampled[MAXT / 16];      // MISO at every rising edge, in order
static uint32_t nsampled;
static unsigned jitter;

static uint32_t sim_in(void)
{
    unsigned steps = 1 + (jitter ? (unsigned)rand() % (jitter + 1) : 0);
    while (steps--) {
        if (now + 1 >= wlen) { sim_run = 0; return 4; }
        now++;
        if ((wave[now] & 1) && !(wave[now - 1] & 1)) sampled[nsampled++] = (uint8_t)miso;
    }
    return wave[now];
}

#define DCSPI_ATTR
#define DCSPI_OM
#define DC_RISE(v)        do { v = sim_in(); } while (!(v & 1) && sim_run)
#define DC_RISE_CS(v, l)  do { v = sim_in(); if (v & 4) goto l; } while (!(v & 1))
#define DC_FALL()         do { } while ((sim_in() & 1) && sim_run)
#define DC_MOSI(v)        (((v) >> 1) & 1)
#define DC_OUT(w)         (miso = (int)((w) >> 31))
#define DC_IDLE()         (miso = 1)
#define DC_WAIT_CS_HIGH() do { } while (!(sim_in() & 4))
#define DC_WAIT_CS_LOW()  do { } while ((sim_in() & 4) && sim_run)
#define DC_KEEP(x)        ((void)0)
#include "../../probe/main/dcspi_bb.h"

static void seg(unsigned n, unsigned pins) { while (n--) wave[wlen++] = (uint8_t)pins; }
static unsigned ph(unsigned lo) { return lo + (unsigned)rand() % 4; }

static void byte_out(unsigned b, unsigned lo)
{
    for (int i = 7; i >= 0; i--) {
        unsigned m = ((b >> i) & 1) << 1;
        seg(ph(lo), m);            // clock low, data set up
        seg(ph(lo), m | 1);        // clock high
    }
}

int main(void)
{
    int fails = 0;
    for (int variant = 0; variant < 6; variant++) {
        static uint8_t mem[4 * 256 + 32], want[4 * 256 + 32], cap[DCSPI_CAP];
        static struct { uint32_t first_edge; unsigned idx, len; uint8_t rd; uint8_t data[40]; } fr[4000];
        unsigned nfr = 0, lo = 3 + (unsigned)variant % 3;
        volatile uint32_t frames = 0;
        srand(1234 + variant);
        jitter = lo - 1 > 2 ? 2 : lo - 1;       // polls may skip ticks, but never a whole phase
        if (variant == 5) jitter = 0;
        wlen = 0; now = 0; nsampled = 0; miso = 1; sim_run = 1;
        for (unsigned i = 0; i < sizeof mem; i++) want[i] = mem[i] = (uint8_t)(i * 7 + 3);
        uint32_t edges = 0;
        seg(20, 4);
        for (int f = 0; f < 3000; f++) {
            unsigned bsb = rand() & 3, addr = rand() & 0xFFFF, len = 1 + rand() % 30, rd = rand() & 1;
            if ((addr & 0xFF) > 220) addr = (addr & 0xFF00) | 220;
            if (f % 97 == 50) {                 // an aborted frame: chip select rises mid-header
                seg(ph(lo), 0);
                unsigned nb = 1 + rand() % 2;
                for (unsigned k = 0; k < nb; k++) byte_out(rand() & 0xFF, lo);
                edges += 8 * nb;
                seg(ph(lo), 0); seg(ph(lo) + 2, 4);
                continue;
            }
            seg(ph(lo), 0);                     // chip select low, clock low
            fr[nfr].first_edge = edges; fr[nfr].idx = (bsb << 8) + (addr & 0xFF);
            fr[nfr].len = len; fr[nfr].rd = (uint8_t)rd;
            byte_out(addr >> 8, lo); byte_out(addr & 0xFF, lo); byte_out(bsb << 3 | (rd ? 0 : 4), lo);
            for (unsigned k = 0; k < len; k++) {
                unsigned d = rd ? 0xFF : (unsigned)rand() & 0xFF;
                fr[nfr].data[k] = (uint8_t)d;
                byte_out(d, lo);
            }
            edges += 8 * (3 + len);
            nfr++;
            seg(ph(lo), 0);
            seg(ph(lo) + 1, 4);                 // chip select high: shortest gap is a few ticks
        }
        seg(40, 4);
        dcspi_bb_responder(mem, cap, &frames, &sim_run);
        // replay: what should every read have returned, what should the memory hold
        int bad_read = 0, bad_mem = 0;
        for (unsigned i = 0; i < nfr; i++) {
            if (fr[i].rd) {
                for (unsigned k = 0; k < fr[i].len; k++) {
                    unsigned got = 0;
                    for (int b = 0; b < 8; b++) got = got << 1 | sampled[fr[i].first_edge + 24 + 8 * k + b];
                    if (got != want[fr[i].idx + k]) bad_read++;
                }
            } else {
                memcpy(want + fr[i].idx, fr[i].data, fr[i].len);
            }
        }
        if (memcmp(mem, want, sizeof mem)) bad_mem = 1;
        int ok = !bad_read && !bad_mem && nsampled == edges && frames >= nfr;
        printf("dcspi_bb variant %d (phases >= %u ticks, poll jitter %u): %u frames, %d bad read bytes, memory %s, %s\n",
               variant, lo, jitter, nfr, bad_read, bad_mem ? "WRONG" : "ok", ok ? "ok" : "FAIL");
        if (!ok) fails++;
    }
    printf(fails ? "dcspi_bb: FAILED\n" : "dcspi_bb: all ok\n");
    return fails != 0;
}
