/*
 * DC-PicoLink <-> ESP32 link over SPI (RP2040-Zero stack, cmake -DDC_BOARD=zero-stack).
 * Shared by both firmwares and tests/test_link_spi.c (host-compilable).
 *
 * The packets and their framing are exactly those of link_model.h (COBS, CRC32,
 * channels); only the wire underneath changes. The UART link tops out around
 * 300 KB/s each way; the Dreamcast's W5500 path needs ~1000 KB/s (DC -> net) and
 * ~550 KB/s (net -> DC), so the stack uses SPI instead:
 *
 *   RP2040 (master)          ESP32-S3 (slave)
 *   GP7  SPI0 TX   ------->  MOSI  (GPIO6)
 *   GP4  SPI0 RX   <-------  MISO  (GPIO3)
 *   GP6  SPI0 SCK  ------->  SCLK  (GPIO5)
 *   GP5  CS (GPIO) ------->  CS    (GPIO4)
 *   GP3  HS        <-------  GPIO2   "ready": a transaction is loaded, both buffers armed
 *   GP2  DR        <-------  GPIO1   "data ready": the ESP32 has bytes for the cable
 *
 * (In the back-to-back stack each of these pairs sits directly over the other. GP0/GP1
 * over ESP32 GPIO43/44 are soldered for strength only and stay inputs: GPIO43 prints the
 * ESP32's ROM boot messages and GPIO44 has a pull-up during its boot, so neither is a
 * good handshake line.)
 *
 * Every transaction is exactly LINK_SPI_CHUNK bytes, full duplex. Each side fills its
 * chunk from its queue of wire bytes (whole COBS packets, so the queue only ever runs
 * dry at a packet boundary) and pads the rest with 0x00. To the receiver 0x00 is just
 * the packet delimiter, so padding is a run of empty packets that link_rx_byte()
 * ignores: the byte stream on each side is the same stream the UART link carries,
 * chopped into chunks. A lost or garbled chunk costs the packets in it (CRC), and the
 * receiver is back in step at the next 0x00.
 *
 * Who may clock when:
 *  - The ESP32 can only take part in a transaction it has loaded (spi_slave queue).
 *    It raises HS when the hardware is loaded (post_setup) and drops it when the
 *    transaction ends (post_trans). With two transactions queued the drop and the next
 *    raise come microseconds apart, so the RP2040 doesn't poll HS for the drop: it
 *    clears the GPIO's falling-edge latch when it starts a transaction and, before the
 *    next one, needs that latch set (the ESP32 finished with the last transaction) and
 *    HS high (it has loaded the next). A transaction is never clocked into an ESP32
 *    that isn't ready for it.
 *  - The RP2040 clocks when it has bytes to send or DR is high, and only after it has
 *    fed all of the previous chunk into its receiver (its own back-pressure: it just
 *    stops clocking). The ESP32's back-pressure is HS.
 *  - At boot nothing is outstanding, so the first transaction only needs HS high. If
 *    the link is down and HS has stayed high with no falling edge for LINK_SPI_PROBE_US
 *    since our last transaction ended (an ESP32 stuck with HS high), one probe
 *    transaction per LINK_SPI_PROBE_US gets it going again. (Measured from the end of
 *    the last transaction, not just "link down": right after a reboot the link is down
 *    until the first packet, and a probe then would beat the ESP32's post_trans and
 *    clock into the chunk it just finished. tests/test_link_spi.c found that one.)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifndef LINK_SPI_CHUNK
#define LINK_SPI_CHUNK     1024u          /* bytes per transaction, both directions (multiple of 4) */
#endif
#ifndef LINK_SPI_HZ
#define LINK_SPI_HZ        16000000u      /* requested clock; the RP2040 picks <= this from clk_peri */
#endif
#define LINK_SPI_MODE      1              /* CPOL 0, CPHA 1 */
#define LINK_SPI_PROBE_US  1000000u
#define LINK_SPI_CS_US     1u             /* CS low -> first clock, last clock -> CS high */

/* Fill one chunk from a power-of-two ring (rd/wr free-running, mask = size - 1): up to
   cap queued bytes, the rest 0x00. Returns the number of queued bytes taken. */
static inline size_t link_chunk_fill(const uint8_t *ring, uint32_t mask, uint32_t *rd, uint32_t wr,
                                     uint8_t *out, size_t cap) {
    size_t n = wr - *rd;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) out[i] = ring[(*rd + (uint32_t)i) & mask];
    *rd += (uint32_t)n;
    memset(out + n, 0, cap - n);
    return n;
}

/* ---- the RP2040's decision (master) ------------------------------------------- */
typedef struct {
    bool busy;           /* a transaction is running */
    bool need_fall;      /* the ESP32 hasn't been seen finishing our last transaction yet */
    uint32_t last_probe, last_done;
    uint32_t starts, probes;
} link_spi_master_t;

static inline void link_spi_master_init(link_spi_master_t *m) { memset(m, 0, sizeof *m); }

/* hs: HS now. hs_fell: HS's falling-edge latch (cleared by the caller when it starts a
   transaction). want: bytes to send, DR high, and the previous chunk fully consumed.
   true = start one now: the caller clears the latch, then asserts CS and clocks a chunk. */
static inline bool link_spi_master_start(link_spi_master_t *m, uint32_t now, bool hs, bool hs_fell,
                                         bool want, bool link_up) {
    if (m->busy || !hs || !want) return false;
    if (m->need_fall && !hs_fell) {
        if (link_up || (uint32_t)(now - m->last_done) < LINK_SPI_PROBE_US ||
            (uint32_t)(now - m->last_probe) < LINK_SPI_PROBE_US) return false;
        m->last_probe = now;
        m->probes++;
    }
    m->busy = true;
    m->need_fall = true;
    m->starts++;
    return true;
}

static inline void link_spi_master_done(link_spi_master_t *m, uint32_t now) { m->busy = false; m->last_done = now; }
