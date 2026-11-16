// Bit-banged W5500-style SPI slave for the ESP32-S3 probe (and its host test).
//
// The includer provides:
//   DC_RISE(v)        wait for clock high; v = the pin snapshot that saw it
//   DC_RISE_CS(v, l)  the same, but goto l when chip select goes high first
//   DC_FALL()         wait for clock low
//   DC_OUT(bit31)     put out MISO = bit 31 of the argument
//   DC_IDLE()         MISO high; DC_WAIT_CS_HIGH(), DC_WAIT_CS_LOW()
//   DC_MOSI(v)        the MOSI bit (0/1) of a snapshot
//   DC_KEEP(x)        stops the compiler from moving the computation of x
//   DCSPI_OM          declarations at the top of the function (e.g. the output mask)
// Frame: 16 address bits, 5 block-select bits, read/write, 2 mode bits, data bytes.
// MISO: every bit goes out right after the rising edge on which the master took the
// previous one (same rule as firmware/w5500_spi.pio).
// Memory model for the probe: mem[block & 3][address & 0xFF], no wrap inside a frame.
#ifndef DCSPI_BB_H
#define DCSPI_BB_H
#include <stdint.h>

#define DCSPI_CAP 256

#define DCSPI_BIT(b)   do { DC_RISE(v); b = (b << 1) | DC_MOSI(v); DC_KEEP(b); DC_FALL(); } while (0)
#define DCSPI_BIT1(b)  do { DC_RISE_CS(v, end); b = DC_MOSI(v); DC_FALL(); } while (0)
#define DCSPI_BYTE(b)  do { DCSPI_BIT1(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); \
                            DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); } while (0)
#define DCSPI_OUT(w)   do { DC_OUT(w); w <<= 1; } while (0)

static DCSPI_ATTR void dcspi_bb_responder(uint8_t *mem, uint8_t *cap, volatile uint32_t *frames,
                                          volatile int *run)
{
    DCSPI_OM
    DC_WAIT_CS_HIGH();
    for (;;) {
        uint32_t v, a, b, c, w;
        uint8_t *p, *q = cap, *dst = 0;
        DC_IDLE();
        DC_WAIT_CS_LOW();
        DCSPI_BYTE(a);                                   // address high (unused by the model)
        DCSPI_BYTE(a);                                   // address low
        DCSPI_BIT1(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b);   // block select
        p = mem + ((b & 3) << 8) + (a & 0xFF);
        w = (uint32_t)*p++ << 24;                        // the answer, ready 3 bits early
        DC_KEEP(w);
        DC_RISE(v); c = DC_MOSI(v); DC_FALL();           // read/write
        DC_RISE(v); DC_FALL();                           // mode bits (variable length only)
        if (!c) {
            DC_RISE(v);                                  // 24th edge
            for (;;) {
                DCSPI_OUT(w);                            // bit 7, taken on the next edge
                DC_FALL(); DC_RISE_CS(v, end); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);
                DC_FALL(); DC_RISE(v); DCSPI_OUT(w);     // bit 0
                w = (uint32_t)*p++ << 24;
                DC_FALL(); DC_RISE(v);
            }
        } else {
            dst = p - 1;
            DC_RISE(v); DC_FALL();                       // 24th edge
            for (;;) {
                DCSPI_BYTE(b);
                *q = (uint8_t)b;
                if (q != cap + DCSPI_CAP - 1) q++;
            }
        }
end:
        for (uint8_t *s = cap; s != q; s++) *dst++ = *s; // frame end: store what was written
        *frames = *frames + 1;
        if (!*run) return;                               // chip select is high here
    }
}
#endif
