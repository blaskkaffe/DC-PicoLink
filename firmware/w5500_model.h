/*
 * W5500 register/buffer model (MACRAW on socket 0), shared between the RP2040
 * SPI-slave loop (core1) and the host-side test harness.
 *
 * Transaction flow, as delivered by the PIO SPI slave:
 *   w5_hdr_addr(addr16)     after 16 header bits
 *   w5_hdr_bsb(bsb5)        after 21 bits  -> pushes first MISO byte
 *   w5_hdr_ctl(rwb_om3)     after 24 bits  -> pushes 3 more MISO bytes
 *   w5_data(byte)           after each data byte -> pushes 1 MISO byte
 *   w5_end()                CS deasserted
 * W5_PUSH(byte) must be defined by the includer (queue a MISO byte).
 *
 * Variable-length data mode is continuous, exactly like the real chip: bytes
 * keep auto-incrementing the address until CS rises. KOS's w5500_exec_cmd()
 * relies on this, and on MISO reading 0x00 during a write burst.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef W5_FAST
#define W5_FAST
#endif
/* MR.RST from the host. The includer may supply a faster version (the next
   frame can start ~3 us later); it must have the same effect as w5_reset(). */
#ifndef W5_RESET
#define W5_RESET() w5_reset()
#endif
/* VERSIONR as put on the wire. Diagnostic builds change it to find out where
   the Dreamcast samples MISO (one bit early or late turns 0x04 into 0x08/0x02). */
#ifndef W5_VERSIONR_VALUE
#define W5_VERSIONR_VALUE 0x04
#endif

#define W5_BUF_MAX       16384u
#define W5_TXQ_LEN       32u

/* common register offsets */
#define W5C_MR       0x00
#define W5C_SHAR     0x09
#define W5C_PHYCFGR  0x2E
#define W5C_VERSIONR 0x39
/* socket register offsets */
#define W5S_MR       0x00
#define W5S_CR       0x01
#define W5S_IR       0x02
#define W5S_SR       0x03
#define W5S_RXBUF    0x1E
#define W5S_TXBUF    0x1F
#define W5S_TX_FSR   0x20
#define W5S_TX_RD    0x22
#define W5S_TX_WR    0x24
#define W5S_RX_RSR   0x26
#define W5S_RX_RD    0x28
#define W5S_RX_WR    0x2A

typedef struct { uint16_t start, len; } w5_txd_t;

typedef struct {
    uint8_t common[64];
    uint8_t sreg[8][64];
    uint8_t s0tx[W5_BUF_MAX];
    uint8_t s0rx[W5_BUF_MAX];

    /* shared with the network side (core0) */
    volatile uint32_t rx_wr;        /* bytes written into s0rx (core0 owns)   */
    volatile uint32_t rx_rd;        /* bytes released by RECV  (core1 owns)   */
    volatile uint32_t tx_sent;      /* bytes queued by SEND    (core1 owns)   */
    volatile uint32_t tx_done;      /* bytes consumed          (core0 owns)   */
    volatile uint16_t rx_mask, tx_mask;
    volatile uint8_t  sock_open;    /* socket 0 open in MACRAW */
    volatile uint8_t  mac_filter;   /* Sn_MR.MFEN latched at OPEN */
    volatile uint8_t  link_up;      /* reported in PHYCFGR (core0 sets) */
    volatile uint32_t txq_head;     /* core1 */
    volatile uint32_t txq_tail;     /* core0 */
    w5_txd_t txq[W5_TXQ_LEN];

    /* transaction state (core1 only) */
    uint16_t addr;
    uint8_t  bsb;
    uint8_t  phase;                 /* 0 idle, 1 got addr, 2 got bsb, 3 data */
    bool     write;
    uint16_t raddr;                 /* next address to prefetch for MISO */
    uint16_t addr0;                 /* frame start address (diagnostics) */

    /* diagnostics (core1 writes, core0 reports) */
    volatile uint32_t n_open, n_send, n_recv, n_close, n_recv_bad;
    /* first RX resyncs of a session: why (1 RX_RD beyond the data, 2 no sane
       length header there), the RX_RD the DC wrote, the read/write positions
       and the header found at that spot */
    volatile uint8_t n_rsd;
    struct { uint32_t base, wr; uint16_t rd, hl; uint8_t why; } rsd[4];
    volatile uint8_t  last_open_mr;
} w5_t;

extern w5_t w5;

static inline void w5_put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline uint16_t w5_get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline uint32_t w5_cap(uint8_t kb) {
    if (kb == 0) return 0;
    if (kb > 16) kb = 16;
    /* real chip only accepts powers of two; round down */
    uint32_t c = 1; while ((c << 1) <= kb) c <<= 1;
    return c * 1024u;
}

static inline void w5_reset(void) {
    memset(w5.common, 0, sizeof(w5.common));
    memset(w5.sreg, 0, sizeof(w5.sreg));
    w5.common[0x13] = 0x00;                     /* INTLEVEL */
    w5_put16(&w5.common[0x19], 0x07D0);         /* RTR */
    w5.common[0x1B] = 0x08;                     /* RCR */
    w5.common[0x1C] = 0x28;                     /* PTIMER */
    w5.common[W5C_PHYCFGR] = 0xB8;
    w5.common[W5C_VERSIONR] = W5_VERSIONR_VALUE;
    for (int s = 0; s < 8; s++) {
        uint8_t *r = w5.sreg[s];
        memset(&r[0x06], 0xFF, 6);              /* DHAR */
        r[0x16] = 0x80;                         /* TTL */
        r[W5S_RXBUF] = 2; r[W5S_TXBUF] = 2;
        w5_put16(&r[W5S_TX_FSR], 2048);
        r[0x2C] = 0xFF;                         /* IMR */
        w5_put16(&r[0x2D], 0x4000);             /* FRAG */
    }
    w5.sock_open = 0;
    w5.mac_filter = 0;
    w5.tx_mask = 2048 - 1;
    w5.rx_mask = 2048 - 1;
    w5.rx_rd = w5.rx_wr;                        /* frames already queued for TX still go out */
}

/* Recompute the registers that reflect live state. Called between transactions. */
static inline W5_FAST void w5_refresh(void) {
    uint8_t *r = w5.sreg[0];
    uint32_t txcap = w5_cap(r[W5S_TXBUF]);
    uint32_t inflight = w5.tx_sent - w5.tx_done;
    w5_put16(&r[W5S_TX_FSR], (uint16_t)(inflight >= txcap ? 0 : txcap - inflight));
    uint32_t wr = w5.rx_wr;
    uint32_t rsr = wr - w5.rx_rd;
    w5_put16(&r[W5S_RX_RSR], (uint16_t)rsr);
    w5_put16(&r[W5S_RX_WR], (uint16_t)wr);
    if (rsr) r[W5S_IR] |= 0x04;                 /* RECV */
    w5.common[W5C_PHYCFGR] = (uint8_t)((w5.common[W5C_PHYCFGR] & 0xF8) | (w5.link_up ? 0x07 : 0x00));
}

static inline W5_FAST void w5_command(int s, uint8_t cmd) {
    if (s != 0) {                               /* other sockets: accept, do nothing */
        if (cmd == 0x10) w5.sreg[s][W5S_SR] = 0;
        return;
    }
    uint8_t *r = w5.sreg[0];
    switch (cmd) {
    case 0x01: /* OPEN */
        w5.n_open++; w5.last_open_mr = r[W5S_MR];
        if ((r[W5S_MR] & 0x0F) == 0x04) {
            uint32_t txcap = w5_cap(r[W5S_TXBUF]), rxcap = w5_cap(r[W5S_RXBUF]);
            w5.tx_mask = (uint16_t)(txcap ? txcap - 1 : 0);
            w5.rx_mask = (uint16_t)(rxcap ? rxcap - 1 : 0);
            w5.mac_filter = (r[W5S_MR] & 0x80) != 0;
            uint32_t wr = w5.rx_wr;
            w5.rx_rd = wr;                      /* start with an empty RX buffer */
            w5_put16(&r[W5S_RX_RD], (uint16_t)wr);
            w5_put16(&r[W5S_TX_RD], w5_get16(&r[W5S_TX_WR]));
            r[W5S_SR] = 0x42;                   /* SOCK_MACRAW */
            w5.sock_open = 1;
        } else {
            r[W5S_SR] = 0x00;                   /* only MACRAW is emulated */
        }
        break;
    case 0x08: /* DISCON */
    case 0x10: /* CLOSE */
        w5.n_close++;
        r[W5S_SR] = 0x00;
        w5.sock_open = 0;
        break;
    case 0x20: { /* SEND */
        w5.n_send++;
        uint16_t rd = w5_get16(&r[W5S_TX_RD]);
        uint16_t wr = w5_get16(&r[W5S_TX_WR]);
        uint16_t len = (uint16_t)(wr - rd);
        if (w5.sock_open && len && len <= 1514 &&
            w5.txq_head - w5.txq_tail < W5_TXQ_LEN) {
            w5_txd_t *d = &w5.txq[w5.txq_head % W5_TXQ_LEN];
            d->start = rd; d->len = len;
            w5.tx_sent += len;
            __sync_synchronize();
            w5.txq_head++;
        }
        w5_put16(&r[W5S_TX_RD], wr);
        r[W5S_IR] |= 0x10;                      /* SENDOK */
        break;
    }
    case 0x40: { /* RECV */
        w5.n_recv++;
        uint16_t rd = w5_get16(&r[W5S_RX_RD]);
        uint32_t base = w5.rx_rd, wr = w5.rx_wr;
        uint32_t nrd = base + (uint16_t)(rd - (uint16_t)base);
        bool ok = nrd - base <= wr - base;
        uint32_t hl = 0;
        if (ok && nrd != wr) {
            /* More frames waiting: RX_RD must land on one's 2-byte length
               header. Garbage there means the DC's pointer slipped into the
               middle of a frame (a misread length or pointer). */
            uint16_t m = w5.rx_mask;
            hl = (uint32_t)w5.s0rx[nrd & m] << 8 | w5.s0rx[(nrd + 1) & m];
            ok = hl >= 2 + 14 && hl <= 2 + 1518 && hl <= wr - nrd;
        }
        if (ok) w5.rx_rd = nrd;
        else {
            /* Bad RECV: an RX_RD beyond the received data, or inside a frame.
               Drop what is buffered and restart at the write position, which
               is always a frame boundary. (net7 put the old read position back,
               but if that was itself inside a frame, KOS re-read the same
               garbage length forever: RuneScape, 23 Oct, 22k bad RECVs.) */
            w5.n_recv_bad++;
            uint8_t i = w5.n_rsd;
            if (i < 4) {
                w5.rsd[i].base = base; w5.rsd[i].wr = wr; w5.rsd[i].rd = rd;
                w5.rsd[i].hl = (uint16_t)hl; w5.rsd[i].why = nrd - base <= wr - base ? 2 : 1;
                w5.n_rsd = (uint8_t)(i + 1);
            }
            w5.rx_rd = wr;
            w5_put16(&r[W5S_RX_RD], (uint16_t)wr);
        }
        break;
    }
    default:
        break;
    }
    /* Like the real chip, the live registers (RX_RSR, RX_WR, TX_FSR, IR) follow a
       command at once: KOS reads RX_RSR again right after RECV, and a stale
       non-zero value makes it parse a "packet" beyond the received data. */
    w5_refresh();
}

static inline W5_FAST uint8_t w5_read(uint8_t bsb, uint16_t a) {
    switch (bsb & 3) {
    case 0:  return bsb ? 0 : w5.common[a & 0x3F];
    case 1:  return w5.sreg[bsb >> 2][a & 0x3F];
    case 2:  return bsb == 2 ? w5.s0tx[a & w5.tx_mask] : 0;
    default: return bsb == 3 ? w5.s0rx[a & w5.rx_mask] : 0;
    }
}

static inline W5_FAST void w5_write(uint8_t bsb, uint16_t a, uint8_t v) {
    switch (bsb & 3) {
    case 0:
        if (bsb) return;
        a &= 0x3F;
        if (a == W5C_MR) {
            if (v & 0x80) { W5_RESET(); return; }
        } else if (a == W5C_VERSIONR || a >= 0x3A) {
            return;
        } else if (a == W5C_PHYCFGR) {
            v = (uint8_t)((v & 0xF8) | (w5.common[a] & 0x07));
        }
        w5.common[a] = v;
        return;
    case 1: {
        int s = bsb >> 2;
        uint8_t *r = w5.sreg[s];
        a &= 0x3F;
        switch (a) {
        case W5S_CR:  w5_command(s, v); return;          /* reads back 0 */
        case W5S_IR:  r[a] &= (uint8_t)~v; return;       /* write-1-to-clear */
        case W5S_SR:
        case W5S_TX_FSR: case W5S_TX_FSR + 1:
        case W5S_TX_RD:  case W5S_TX_RD + 1:
        case W5S_RX_RSR: case W5S_RX_RSR + 1:
        case W5S_RX_WR:  case W5S_RX_WR + 1:
            return;                                      /* read-only */
        default: r[a] = v; return;
        }
    }
    case 2: if (bsb == 2) w5.s0tx[a & w5.tx_mask] = v; return;
    default: if (bsb == 3) w5.s0rx[a & w5.rx_mask] = v; return;
    }
}

/* ---- SPI frame handlers ---------------------------------------------- */
static inline W5_FAST void w5_hdr_addr(uint32_t v) { w5.addr = w5.addr0 = (uint16_t)v; w5.phase = 1; }

static inline W5_FAST void w5_hdr_bsb(uint32_t v) {
    w5.bsb = (uint8_t)(v & 0x1F);
    W5_PUSH(w5_read(w5.bsb, w5.addr));     /* speculative: first data byte */
    w5.raddr = (uint16_t)(w5.addr + 1);
    w5.phase = 2;
}

static inline W5_FAST void w5_hdr_ctl(uint32_t v) {
    w5.write = (v >> 2) & 1;
    w5.phase = 3;
    for (int i = 0; i < 3; i++) {
        if (w5.write) W5_PUSH(0);
        else { W5_PUSH(w5_read(w5.bsb, w5.raddr)); w5.raddr++; }
    }
}

static inline W5_FAST void w5_data(uint8_t b) {
    if (w5.write) { w5_write(w5.bsb, w5.addr, b); W5_PUSH(0); }
    else          { W5_PUSH(w5_read(w5.bsb, w5.raddr)); w5.raddr++; }
    w5.addr++;
}

static inline W5_FAST void w5_end(void) { w5.phase = 0; }

/* ---- network side (core0) -------------------------------------------- */
/* Store a received Ethernet frame. Returns false if there is no room yet. */
static inline bool w5_net_rx(const uint8_t *f, uint16_t len) {
    if (!w5.sock_open) return true;                     /* drop */
    if (w5.mac_filter && len >= 6 && !(f[0] & 1) &&
        memcmp(f, &w5.common[W5C_SHAR], 6) != 0)
        return true;                                    /* not for us: drop */
    uint32_t cap = (uint32_t)w5.rx_mask + 1;
    uint32_t need = (uint32_t)len + 2;
    if (need > cap) return true;
    if (cap - (w5.rx_wr - w5.rx_rd) < need) return false;
    uint32_t p = w5.rx_wr;
    uint16_t m = w5.rx_mask;
    w5.s0rx[p & m] = (uint8_t)(need >> 8);
    w5.s0rx[(p + 1) & m] = (uint8_t)need;
    p += 2;
    for (uint16_t i = 0; i < len; i++) w5.s0rx[(p + i) & m] = f[i];
    __sync_synchronize();
    w5.rx_wr = p + len;
    return true;
}

/* Next frame waiting to go out, or NULL. */
static inline w5_txd_t *w5_net_tx_peek(void) {
    if (w5.txq_tail == w5.txq_head) return NULL;
    __sync_synchronize();
    return &w5.txq[w5.txq_tail % W5_TXQ_LEN];
}
static inline void w5_net_tx_copy(const w5_txd_t *d, uint8_t *dst) {
    uint16_t m = w5.tx_mask;
    for (uint16_t i = 0; i < d->len; i++) dst[i] = w5.s0tx[(uint16_t)(d->start + i) & m];
}
static inline void w5_net_tx_pop(void) {
    w5_txd_t *d = &w5.txq[w5.txq_tail % W5_TXQ_LEN];
    w5.tx_done += d->len;
    __sync_synchronize();
    w5.txq_tail++;
}
