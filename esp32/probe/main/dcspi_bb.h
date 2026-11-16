// Bit-banged W5500-style SPI slave for the ESP32-S3 probe (and its host test).
//
// The includer provides (pin snapshot layout: bit 0 MOSI, bit 1 clock, bit 2 chip select):
//   DC_RISE_CS(v, l)  wait for clock high (v = the pin snapshot that saw it); goto l when
//                     chip select goes high first. Every wait checks chip select: frames are
//                     whole bytes, so a slave that missed one edge and only looked at byte
//                     boundaries stayed one bit off for ever (seen on hardware at 16 MHz).
//   DC_FALL()         wait for clock low
//   DC_OUT(w)         put out MISO = bit 31 of w
//   DC_IDLE()         MISO high; DC_WAIT_CS_HIGH(), DC_WAIT_CS_LOW()
//   DC_KEEP(x)        x must be computed by this point (stops the compiler moving the work)
//   DCSPI_OM          declarations at the top of the function
// Frame: 16 address bits, 5 block-select bits, read/write, 2 mode bits, data bytes.
// MISO: every bit goes out right after the rising edge on which the master took the
// previous one (same rule as firmware/w5500_spi.pio).
// Memory model for the probe: mem[block & 3][address & 0xFF], no wrap inside a frame.
//
// Where the work sits (second version, after the first hardware runs):
// * Nothing is done while the clock is low: KOS's low phase can be a single port write
//   (~40 ns). All work follows a rising edge.
// * A bit costs one instruction: x = 2x + snapshot. The clock bit in every snapshot adds a
//   known constant (2 per bit), taken off when the value is used.
// * The answer lookup is spread over the high phases of bits 16, 21, 22 and 23.
// * The write path stores a byte after its 8th rising edge; the loop's jump follows the
//   next byte's first edge.
// A real frame engine also needs a bound on the capture buffer (one more instruction per
// byte); the probe's frames are short.
#ifndef DCSPI_BB_H
#define DCSPI_BB_H
#include <stdint.h>

#define DCSPI_CAP 256

#define DCSPI_ACC(x)   do { DC_RISE_CS(v, end); x = (x << 1) + v; DC_KEEP(x); } while (0)
#define DCSPI_BIT(x)   do { DCSPI_ACC(x); DC_FALL(); } while (0)
#define DCSPI_OUT(w)   do { DC_OUT(w); w <<= 1; DC_KEEP(w); } while (0)
#define DCSPI_OBIT(w)  do { DC_FALL(); DC_RISE_CS(v, end); DCSPI_OUT(w); } while (0)

static DCSPI_ATTR void dcspi_bb_responder(uint8_t *mem, uint8_t *cap, volatile uint32_t *frames,
                                          volatile int *run)
{
    DCSPI_OM
    DC_WAIT_CS_HIGH();
    for (;;) {
        uint32_t v, a = 0, b = 0, c, w, t;
        uint8_t *p, *pa, *q = cap, *dst = 0;
        DC_IDLE();
        DC_WAIT_CS_LOW();
        DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a);      // address, 16 bits
        DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a);
        DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a);
        DCSPI_BIT(a); DCSPI_BIT(a); DCSPI_BIT(a);
        DCSPI_ACC(a); pa = mem + ((a + 2) & 0xFF); DC_KEEP(pa); DC_FALL();
        DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b);      // block select, 5 bits
        DCSPI_ACC(b); t = ((b + 2) & 3) << 8; DC_KEEP(t); DC_FALL();
        DC_RISE_CS(v, end); c = v; p = pa + t; DC_KEEP(p); DC_FALL();                 // read/write
        DC_RISE_CS(v, end); w = (uint32_t)*p++ << 24; DC_KEEP(w); DC_FALL();          // mode bit
        if (!(c & 1)) {
            DC_RISE_CS(v, end);                          // 24th edge
            for (;;) {
                DCSPI_OUT(w);                            // bit 7, taken on the next edge
                DCSPI_OBIT(w); DCSPI_OBIT(w); DCSPI_OBIT(w); DCSPI_OBIT(w);
                DCSPI_OBIT(w); DCSPI_OBIT(w); DCSPI_OBIT(w);              // bit 0 is out
                w = (uint32_t)*p++ << 24; DC_KEEP(w);
                DC_FALL(); DC_RISE_CS(v, end);
            }
        } else {
            dst = p - 1;
            DC_RISE_CS(v, end);                          // 24th edge
            DC_FALL();
            DC_RISE_CS(v, end); b = v; DC_KEEP(b);       // first bit of the first data byte
            for (;;) {
                DC_FALL();
                DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b); DCSPI_BIT(b);
                DCSPI_ACC(b);                            // 8th edge
                *q++ = (uint8_t)(b + 2); DC_KEEP(q);
                DC_FALL();
                DC_RISE_CS(v, end); b = v; DC_KEEP(b);   // first bit of the next byte
            }
        }
end:
        for (uint8_t *s = cap; s != q; s++) *dst++ = *s; // frame end: store what was written
        *frames = *frames + 1;
        if (!*run) return;                               // chip select is high here
    }
}
#endif
