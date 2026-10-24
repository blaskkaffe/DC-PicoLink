/* Host test: replays KallistiOS w5500_adapter.c SPI traffic, byte for byte,
   through the W5500 model the way the PIO slave delivers it. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t mq[64]; static int mq_n, mq_r;
#define W5_PUSH(b) do { assert(mq_n - mq_r < 4 + 1); mq[(mq_n++) & 63] = (b); } while (0)
#include "../w5500_model.h"
w5_t w5;

/* ---- SPI frame simulator (one CS-low period) ---- */
static int bitpos;          /* header bits seen */
static uint32_t hacc;
static void cs_low(void) { bitpos = 0; hacc = 0; mq_n = mq_r = 0; }
static void cs_high(void) { w5_end(); w5_refresh(); }
static uint8_t xfer(uint8_t mosi) {
    uint8_t miso = 0xFF;
    if (bitpos < 24) {                                  /* header byte */
        for (int i = 7; i >= 0; i--) {
            hacc = (hacc << 1) | ((mosi >> i) & 1);
            bitpos++;
            if (bitpos == 16) { w5_hdr_addr(hacc & 0xFFFF); hacc = 0; }
            if (bitpos == 21) { w5_hdr_bsb(hacc & 0x1F); hacc = 0; }
            if (bitpos == 24) { w5_hdr_ctl(hacc & 7); hacc = 0; }
        }
        return miso;
    }
    assert(mq_r < mq_n && "MISO byte not ready");
    miso = mq[(mq_r++) & 63];
    w5_data(mosi);
    return miso;
}

/* ---- KOS driver, transcribed onto the simulator ---- */
static void spi_write_data(const uint8_t *d, int n) { while (n--) xfer(*d++); }
static uint8_t spi_read_byte(void) { return xfer(0xFF); }
static void spi_read_data(uint8_t *d, int n) { while (n--) *d++ = xfer(0xFF); }

static uint8_t read_reg(uint8_t blk, uint16_t a) {
    uint8_t c[3] = { a >> 8, a, (uint8_t)(blk << 3) };
    cs_low(); spi_write_data(c, 3); uint8_t r = spi_read_byte(); cs_high(); return r;
}
static void write_reg(uint8_t blk, uint16_t a, uint8_t v) {
    uint8_t c[4] = { a >> 8, a, (uint8_t)(blk << 3 | 4), v };
    cs_low(); spi_write_data(c, 4); cs_high();
}
static uint16_t read_reg16(uint8_t blk, uint16_t a) {
    uint8_t c[3] = { a >> 8, a, (uint8_t)(blk << 3) }, d[2];
    cs_low(); spi_write_data(c, 3); spi_read_data(d, 2); cs_high(); return d[0] << 8 | d[1];
}
static void write_reg16(uint8_t blk, uint16_t a, uint16_t v) {
    uint8_t c[5] = { a >> 8, a, (uint8_t)(blk << 3 | 4), v >> 8, v };
    cs_low(); spi_write_data(c, 5); cs_high();
}
static void read_buf(uint8_t blk, uint16_t a, uint8_t *b, int n) {
    uint8_t c[3] = { a >> 8, a, (uint8_t)(blk << 3) };
    cs_low(); spi_write_data(c, 3); spi_read_data(b, n); cs_high();
}
static void write_buf(uint8_t blk, uint16_t a, const uint8_t *b, int n) {
    uint8_t c[3] = { a >> 8, a, (uint8_t)(blk << 3 | 4) };
    cs_low(); spi_write_data(c, 3); spi_write_data(b, n); cs_high();
}
static uint16_t read_reg16_safe(uint8_t blk, uint16_t a) {
    uint16_t v = 0, v1;
    do { v1 = read_reg16(blk, a); if (v1) v = read_reg16(blk, a); } while (v != v1);
    return v;
}
static int exec_cmd(uint8_t blk, uint8_t cmd) {
    uint8_t b[4] = { 0x00, 0x01, (uint8_t)(blk << 3 | 4), cmd };
    cs_low(); spi_write_data(b, 4);
    b[2] = (uint8_t)(blk << 3);
    for (int i = 0;; i++) {
        spi_write_data(b, 3);
        if (spi_read_byte() == 0) break;
        if (i > 1000) { cs_high(); return -1; }
    }
    cs_high(); return 0;
}

static const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static int hw_init(void) {
    write_reg(0, 0x00, 0x80);
    int i = 0; while (read_reg(0, 0x00) & 0x80) if (i++ > 10) return -1;
    write_reg(0, 0x2E, 0x00); write_reg(0, 0x2E, 0xB8);
    if (read_reg(0, 0x39) != 0x04) return -2;
    write_reg(0, 0x2C, 0);
    for (i = 0; i < 8; i++) { write_reg(1 + 4 * i, 0x1E, 0); write_reg(1 + 4 * i, 0x1F, 0); }
    write_reg(1, 0x1E, 16); write_reg(1, 0x1F, 16);
    write_buf(0, 0x09, mac, 6);
    write_reg(1, 0x00, 0x84);
    if (exec_cmd(1, 0x01) < 0) return -3;
    if (read_reg(1, 0x03) != 0x42) return -4;
    if (!(read_reg(0, 0x2E) & 1)) return -5;
    return 0;
}

static int kos_tx(const uint8_t *p, int len) {
    uint16_t fsr = read_reg16_safe(1, 0x20);
    if (fsr < len) return -1;
    uint16_t wr = read_reg16(1, 0x24);
    write_buf(2, wr, p, len);
    write_reg16(1, 0x24, wr + len);
    return exec_cmd(1, 0x20);
}

static int kos_rx(uint8_t *out) {   /* returns length, 0 if none */
    uint16_t rsr = read_reg16_safe(1, 0x26);
    if (!rsr) return 0;
    uint16_t rd = read_reg16(1, 0x28);
    uint8_t h[2]; read_buf(3, rd, h, 2); rd += 2;
    uint16_t dl = (h[0] << 8 | h[1]) - 2;
    read_buf(3, rd, out, dl);
    rd += dl;
    write_reg16(1, 0x28, rd);
    exec_cmd(1, 0x40);
    return dl;
}

/* network side, as core0 would do */
static int host_drain_tx(uint8_t *out) {
    w5_txd_t *d = w5_net_tx_peek(); if (!d) return 0;
    int n = d->len; w5_net_tx_copy(d, out); w5_net_tx_pop(); return n;
}

int main(void) {
    w5.link_up = 1; w5.rx_mask = w5.tx_mask = 2047;
    w5_reset(); w5_refresh();
    int r = hw_init(); printf("hw_init -> %d\n", r); assert(r == 0);
    assert(w5.mac_filter && w5.sock_open && w5.rx_mask == 16383 && w5.tx_mask == 16383);

    srand(1);
    uint8_t f[1600], g[1600];
    long txb = 0, rxb = 0;
    for (int it = 0; it < 3000; it++) {
        /* DC -> host */
        int len = 42 + rand() % 1473;
        for (int i = 0; i < len; i++) f[i] = rand();
        assert(kos_tx(f, len) == 0);
        int n = host_drain_tx(g); assert(n == len && !memcmp(f, g, len));
        txb += len;
        /* host -> DC: a few frames to our MAC, broadcast, and others */
        int k = 1 + rand() % 3, expect = 0; uint8_t frames[3][1600]; int lens[3], want[3];
        for (int j = 0; j < k; j++) {
            lens[j] = 60 + rand() % 1455;
            for (int i = 0; i < lens[j]; i++) frames[j][i] = rand();
            int t = rand() % 3;
            if (t == 0) memcpy(frames[j], mac, 6);
            else if (t == 1) memset(frames[j], 0xFF, 6);
            else frames[j][0] &= 0xFE;                 /* unicast to someone else */
            want[j] = t != 2;
            assert(w5_net_rx(frames[j], lens[j]));
            w5_refresh();
            expect += want[j];
        }
        for (int j = 0; j < k; j++) {
            if (!want[j]) continue;
            int m = kos_rx(g);
            assert(m == lens[j] && !memcmp(g, frames[j], m));
            rxb += m;
        }
        assert(kos_rx(g) == 0);
    }
    /* fill the RX buffer: must refuse when full, then accept after draining */
    int stored = 0; memset(f, 0xFF, 6);
    while (w5_net_rx(f, 1514)) { stored++; w5_refresh(); if (stored > 20) break; }
    printf("frames held before full: %d\n", stored);
    assert(stored == 10);
    assert(kos_rx(g) == 1514);
    assert(w5_net_rx(f, 1514));
    /* soft reset + re-init works again */
    assert(hw_init() == 0);
    printf("OK: %ld bytes DC->host, %ld bytes host->DC, wraparound exercised\n", txb, rxb);
    return 0;
}
