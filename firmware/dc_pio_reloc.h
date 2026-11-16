/*
 * Moving the Dreamcast pins (host-compilable, tested in tests/test_link_spi.c).
 *
 * w5500_spi.pio and sd_spi.pio are written for the Dreamcast pins at GPIO2..5 and use
 * absolute 'wait gpio N' instructions (the tightest loops can't afford anything else).
 * For a board with the Dreamcast pins elsewhere (DC_PIN_BASE, see dcmode.h) the
 * firmware loads a copy of each program in which every WAIT with source GPIO has its
 * index moved by DC_PIN_BASE - 2. Nothing else changes: all other pin references in
 * these programs (in/out/set/jmp pins, 'mov ::pins') are relative to the state
 * machine's pin bases, which the C code sets from the same PIN_* names.
 *
 * WAIT encoding: 001 ddddd p ss iiiii  (p = polarity, ss = source: 00 GPIO, 01 PIN, 10 IRQ)
 */
#pragma once
#include <stdint.h>

#define DC_PIO_WAIT_OP_MASK   0xE000u
#define DC_PIO_WAIT_OP        0x2000u
#define DC_PIO_WAIT_SRC_MASK  0x0060u      /* bits 6:5 */
#define DC_PIO_WAIT_IDX_MASK  0x001Fu

/* Copy n instructions from src to dst, moving every 'wait gpio' by shift (mod 32).
   Returns the number of instructions changed. */
static inline unsigned dc_pio_relocate(const uint16_t *src, uint16_t *dst, unsigned n, int shift) {
    unsigned changed = 0;
    for (unsigned i = 0; i < n; i++) {
        uint16_t w = src[i];
        if ((w & DC_PIO_WAIT_OP_MASK) == DC_PIO_WAIT_OP && (w & DC_PIO_WAIT_SRC_MASK) == 0 && shift) {
            unsigned idx = ((w & DC_PIO_WAIT_IDX_MASK) + (unsigned)(shift + 32)) & 31u;
            w = (uint16_t)((w & ~DC_PIO_WAIT_IDX_MASK) | idx);
            changed++;
        }
        dst[i] = w;
    }
    return changed;
}
