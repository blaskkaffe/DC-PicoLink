/*
 * SD card emulation, SPI mode (SDHC, block addressing), byte level.
 * Shared by the RP2040 firmware (core1 = card side, core0 = USB side) and the
 * host-side tests.
 *
 * Card side, the includer defines:
 *   SD_PUSH(b)        queue one MISO byte (goes out before any later SD_DMA data)
 *   SD_DMA(buf,len)   stream len bytes after what is queued; buf must stay valid
 *   SD_DMA_BUSY()     true while an SD_DMA stream is still being fed
 *   SD_FLUSH()        drop everything queued/streaming
 *   SD_FILL(v)        byte sent whenever nothing is queued (0xFF idle, 0x00 busy)
 *
 * The PIO shifter pulls the MISO byte for slot k+1 the moment slot k finished, so
 * anything the card side queues while handling byte k goes out in slot k+2 or
 * later. SD allows that for command responses (Ncr = 1..8 bytes). The one place
 * with no slack is the data-response token after a written block. KOS reads it in
 * the very next byte, so it is pre-scheduled with a DMA stream started on the
 * start token.
 *
 * Storage lives on the Pi. Core0 fetches blocks into sd.slot[] ahead of the
 * card side (read pipeline) and forwards written blocks (one at a time; the card
 * signals busy until the Pi confirms the write).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SD_SLOTS     8
#define SD_SLOT_HDR  3                       /* FF FF FE */
#define SD_SLOT_LEN  (SD_SLOT_HDR + 512 + 2) /* + data + CRC16 */

typedef struct {
    /* backend state (core0 writes) */
    volatile uint32_t present;      /* Pi daemon connected and image usable */
    volatile uint32_t blocks;       /* capacity, 512-byte blocks */
    volatile uint32_t readonly;

    /* read stream: card side publishes, core0 fills */
    volatile uint32_t rd_gen;       /* new value = new stream (or stop)      */
    volatile uint32_t rd_start;     /* first LBA                              */
    volatile uint32_t rd_count;     /* 0 = stopped, 1 = CMD17, ~0 = CMD18     */
    volatile uint32_t rd_consumed;  /* blocks completely handed to DMA        */
    volatile uint32_t rd_ready;     /* (gen << 16) | blocks fetched (core0)   */
    uint8_t slot[SD_SLOTS][SD_SLOT_LEN];

    /* write: one block in flight */
    volatile uint32_t wr_seq;       /* bumped by card side when wr_buf is full */
    volatile uint32_t wr_ack;       /* set to wr_seq by core0 once written     */
    volatile uint32_t wr_lba;
    uint8_t wr_buf[514];            /* data + CRC as received */
} sd_shared_t;

extern sd_shared_t sd;

/* ---- CRCs ---------------------------------------------------------------- */
static inline uint8_t sd_crc7(const uint8_t *p, int n) {
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t d = p[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80) crc ^= 0x09;
            d <<= 1;
        }
    }
    return crc & 0x7F;
}

static inline uint16_t sd_crc16(const uint8_t *p, int n) {     /* CCITT/XMODEM */
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---- card side ------------------------------------------------------------ */
enum { SDS_CMD, SDS_WR_TOKEN, SDS_WR_DATA };

typedef struct {
    uint8_t  state, n, idle, app, multi, busy, rd_active, rd_dma;
    uint8_t  cmd[6];
    uint32_t wr_lba, rd_sent, rd_count;
    uint16_t wr_idx;
    uint8_t  resp[24];
    uint8_t  wr_resp[514];          /* 513 x FF + data-response token */
} sd_card_t;

extern sd_card_t sdc;

static inline void sd_card_init(void) {
    memset(&sdc, 0, sizeof(sdc));
    sdc.idle = 1;
    memset(sdc.wr_resp, 0xFF, sizeof(sdc.wr_resp));
    for (int i = 0; i < SD_SLOTS; i++) {
        sd.slot[i][0] = 0xFF; sd.slot[i][1] = 0xFF; sd.slot[i][2] = 0xFE;
    }
}

static inline void sd_stop_stream(void) {
    sdc.rd_active = 0;
    sdc.rd_dma = 0;
    sd.rd_count = 0;
    __sync_synchronize();
    sd.rd_gen++;
}

static inline void sd_start_stream(uint32_t lba, uint32_t count) {
    sdc.rd_active = 1;
    sdc.rd_dma = 0;
    sdc.rd_sent = 0;
    sdc.rd_count = count;
    sd.rd_start = lba;
    sd.rd_count = count;
    sd.rd_consumed = 0;
    __sync_synchronize();
    sd.rd_gen++;
}

/* End of a CS frame: abort transfers, keep busy state (a real card stays busy). */
static inline void sd_frame_end(void) {
    if (sdc.rd_active) sd_stop_stream();
    if (sdc.state == SDS_WR_DATA) SD_FLUSH();
    sdc.state = SDS_CMD;
    sdc.n = 0;
}

static inline void sd_send(const uint8_t *p, int len) {
    memcpy(sdc.resp, p, (size_t)len);
    SD_DMA(sdc.resp, len);
}

static inline void sd_reg_block(uint8_t r1, uint8_t *reg16) {    /* CSD / CID */
    uint8_t b[21];
    reg16[15] = (uint8_t)((sd_crc7(reg16, 15) << 1) | 1);
    b[0] = r1; b[1] = 0xFF; b[2] = 0xFE;
    memcpy(&b[3], reg16, 16);
    uint16_t c = sd_crc16(reg16, 16);
    b[19] = (uint8_t)(c >> 8); b[20] = (uint8_t)c;
    sd_send(b, 21);
}

static inline void sd_exec(void) {
    uint8_t cmd = sdc.cmd[0] & 0x3F;
    uint32_t arg = ((uint32_t)sdc.cmd[1] << 24) | ((uint32_t)sdc.cmd[2] << 16) |
                   ((uint32_t)sdc.cmd[3] << 8) | sdc.cmd[4];
    bool app = sdc.app;
    sdc.app = 0;
    uint8_t r1 = sdc.idle ? 0x01 : 0x00;
    const uint8_t ILLEGAL = 0x04;

    switch (cmd) {
    case 0:                                         /* GO_IDLE_STATE */
        sd_frame_end();
        SD_FLUSH();
        sdc.idle = 1;
        SD_PUSH(0x01);
        return;
    case 8: {                                       /* SEND_IF_COND */
        uint8_t b[5] = { r1, 0, 0, (uint8_t)((arg >> 8) & 0x0F), (uint8_t)arg };
        sd_send(b, 5);
        return;
    }
    case 55:  sdc.app = 1; SD_PUSH(r1); return;   /* APP_CMD */
    case 41:                                        /* (ACMD41) SD_SEND_OP_COND */
        if (app && sd.present) { sdc.idle = 0; SD_PUSH(0x00); }
        else SD_PUSH(r1 | ILLEGAL);                 /* no backend: fail fast, "no card" */
        return;
    case 58: {                                      /* READ_OCR: powered up, SDHC */
        uint8_t b[5] = { r1, 0xC0, 0xFF, 0x80, 0x00 };
        sd_send(b, 5);
        return;
    }
    case 59: case 16:  SD_PUSH(r1); return;       /* CRC_ON_OFF, SET_BLOCKLEN */
    case 23:  SD_PUSH(app ? r1 : (uint8_t)(r1 | ILLEGAL)); return;   /* ACMD23 */
    case 13: { uint8_t b[2] = { r1, 0 }; sd_send(b, 2); return; }   /* SEND_STATUS */
    case 9: {                                       /* SEND_CSD (v2.0) */
        uint32_t cs = sd.blocks / 1024u; if (cs) cs--;
        uint8_t c[16] = { 0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00,
                          (uint8_t)((cs >> 16) & 0x3F), (uint8_t)(cs >> 8), (uint8_t)cs,
                          0x7F, 0x80, 0x0A, 0x40, 0x00, 0 };
        sd_reg_block(r1, c);
        return;
    }
    case 10: {                                      /* SEND_CID */
        uint8_t c[16] = { 0xDC, 'P', 'I', 'D', 'C', 'S', 'D', ' ', 0x10,
                          0x12, 0x34, 0x56, 0x78, 0x01, 0x9A, 0 };
        sd_reg_block(r1, c);
        return;
    }
    case 12:                                        /* STOP_TRANSMISSION */
        sd_stop_stream();
        SD_FLUSH();
        SD_PUSH(0x00);
        return;
    case 17: case 18:                               /* READ_SINGLE / MULTIPLE */
        if (sdc.idle || !sd.present) { SD_PUSH(r1 | ILLEGAL); return; }
        if (arg >= sd.blocks) { SD_PUSH(0x40); return; }    /* parameter error */
        SD_PUSH(0x00);
        sd_start_stream(arg, cmd == 17 ? 1u : 0xFFFFFFFFu);
        return;
    case 24: case 25:                               /* WRITE_BLOCK / MULTIPLE */
        if (sdc.idle || !sd.present) { SD_PUSH(r1 | ILLEGAL); return; }
        if (arg >= sd.blocks) { SD_PUSH(0x40); return; }
        SD_PUSH(0x00);
        sdc.wr_lba = arg;
        sdc.multi = cmd == 25;
        sdc.state = SDS_WR_TOKEN;
        return;
    default:
        SD_PUSH(r1 | ILLEGAL);
        return;
    }
}

/* One byte received from the host (MOSI). */
static inline void sd_rx(uint8_t b) {
    switch (sdc.state) {
    case SDS_WR_DATA:
        sd.wr_buf[sdc.wr_idx++] = b;
        if (sdc.wr_idx == 514) {
            if (sdc.wr_resp[513] == 0x05) {
                sd.wr_lba = sdc.wr_lba;
                __sync_synchronize();
                sd.wr_seq++;
            } else {
                sdc.busy = 0;                       /* rejected: no busy phase */
                SD_FILL(0xFF);
            }
            sdc.wr_lba++;
            sdc.state = sdc.multi ? SDS_WR_TOKEN : SDS_CMD;
        }
        return;
    case SDS_WR_TOKEN:
        if (b == 0xFE || b == 0xFC) {
            /* The token was byte T; slot T+1 is already pulled. Data = T+1..T+512,
               CRC = T+513..T+514, response = T+515, busy from T+516. */
            bool ok = sd.present && !sd.readonly && sdc.wr_lba < sd.blocks;
            sdc.wr_resp[513] = ok ? 0x05 : 0x0D;
            sdc.wr_idx = 0;
            sdc.busy = 1;
            SD_FILL(0x00);                          /* used once the stream is done */
            SD_DMA(sdc.wr_resp, 514);
            sdc.state = SDS_WR_DATA;
            return;
        }
        if (b == 0xFD) { sdc.state = SDS_CMD; return; }   /* stop token */
        if ((b & 0xC0) != 0x40) return;
        sdc.state = SDS_CMD;
        __attribute__((fallthrough));               /* a command */
    case SDS_CMD:
    default:
        if (sdc.n == 0 && (b & 0xC0) != 0x40) return;
        sdc.cmd[sdc.n++] = b;
        if (sdc.n == 6) { sdc.n = 0; sd_exec(); }
        return;
    }
}

/* Call often: streams fetched blocks, ends busy once the Pi confirmed a write. */
static inline void sd_poll(void) {
    if (sdc.busy && sd.wr_ack == sd.wr_seq) {
        sdc.busy = 0;
        SD_FILL(0xFF);
    }
    if (!sdc.rd_active || SD_DMA_BUSY()) return;
    if (sdc.rd_dma) {                               /* previous block fully queued */
        sdc.rd_dma = 0;
        sd.rd_consumed++;
        if (++sdc.rd_sent == sdc.rd_count) { sdc.rd_active = 0; return; }
    }
    uint32_t r = sd.rd_ready;
    if ((r >> 16) != (sd.rd_gen & 0xFFFF)) return;
    if ((uint16_t)((r & 0xFFFF) - (uint16_t)sd.rd_consumed) == 0) return;
    __sync_synchronize();
    SD_DMA(sd.slot[sd.rd_consumed % SD_SLOTS], SD_SLOT_LEN);
    sdc.rd_dma = 1;
}

/* ---- USB side (core0): block link to the Pi daemon ------------------------
 * Device -> Pi:  'H'                          hello (on connect)
 *                'R' lba:u32 count:u16        read count blocks
 *                'W' lba:u32 data[512]        write one block
 * Pi -> device:  'I' blocks:u32 flags:u8      image info (bit0 = read-only)
 *                'D' lba:u32 data[512] crc:u16(BE, CRC16-XMODEM of data)
 *                'A' lba:u32 status:u8        write done (0 = ok)
 *                'M' mode:u8                  force a bus mode (0 auto, 1 serial,
 *                                             2 SPI, 3 W5500 only, 4 SD only)
 *                'C' x:u8                     arm the logic-analyzer capture
 * All integers little-endian except the CRC (sent as the card sends it).
 * The includer defines HOST_WRITE(buf,len) -> bool (all-or-nothing) and
 * HOST_WRITE_AVAIL() -> free bytes.
 */
#define SDH_PEND 16
typedef struct {
    uint32_t gen, start, count, issued, ready;
    struct { uint32_t gen, idx, n; } pend[SDH_PEND];
    uint32_t ph, pt;                 /* pend head/tail */
    uint32_t wr_sent;                /* last wr_seq sent */
    bool wr_out;
    /* rx parser */
    uint8_t  hdr[7]; uint32_t hn, need; uint8_t type;
    uint8_t *dst; uint32_t dn, dlen;
    uint8_t  scratch[514];
} sd_host_t;

extern sd_host_t sdh;

static inline void sdh_reset(void) {
    uint32_t keep_ws = sdh.wr_sent;
    memset(&sdh, 0, sizeof(sdh));
    sdh.wr_sent = keep_ws;
    sdh.gen = sd.rd_gen;
    sd.rd_ready = (sdh.gen & 0xFFFF) << 16;
}

static inline void sdh_disconnected(void) {
    sd.present = 0;
    if (sd.wr_ack != sd.wr_seq) sd.wr_ack = sd.wr_seq;   /* don't hang the DC */
    sdh_reset();
    sdh.wr_sent = sd.wr_seq;
}

static inline void sdh_connected(void) {
    sdh_reset();
    sdh.wr_sent = sd.wr_seq;
    uint8_t h = 'H';
    HOST_WRITE(&h, 1);
}

static inline uint32_t sdh_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bytes received from the Pi. */
static inline void sdh_rx(const uint8_t *p, uint32_t n) {
    while (n) {
        if (sdh.dst) {                                   /* bulk payload */
            uint32_t k = sdh.dlen - sdh.dn; if (k > n) k = n;
            memcpy(sdh.dst + sdh.dn, p, k);
            sdh.dn += k; p += k; n -= k;
            if (sdh.dn == sdh.dlen) {
                if (sdh.dst != sdh.scratch) {
                    sdh.ready++;
                    __sync_synchronize();
                    sd.rd_ready = ((sdh.gen & 0xFFFF) << 16) | (sdh.ready & 0xFFFF);
                }
                sdh.dst = NULL;
            }
            continue;
        }
        if (sdh.hn == 0) {
            sdh.type = *p++; n--;
            sdh.need = sdh.type == 'I' ? 5 : sdh.type == 'D' ? 4 : sdh.type == 'A' ? 5 : sdh.type == 'M' ? 1 : sdh.type == 'C' ? 1 : 0;
            if (!sdh.need) continue;                    /* unknown byte: resync */
            sdh.hn = 1;
            continue;
        }
        sdh.hdr[sdh.hn - 1] = *p++; n--;
        if (sdh.hn++ < sdh.need) continue;
        sdh.hn = 0;
        if (sdh.type == 'C') {
#ifdef SDH_CAPTURE_HOOK
            SDH_CAPTURE_HOOK(sdh.hdr[0]);
#endif
        } else if (sdh.type == 'M') {
#ifdef SDH_MODE_HOOK
            SDH_MODE_HOOK(sdh.hdr[0]);
#endif
        } else if (sdh.type == 'I') {
            sd.blocks = sdh_le32(sdh.hdr) & ~1023u;    /* whole 512 KiB units (CSD) */
            sd.readonly = sdh.hdr[4] & 1;
            __sync_synchronize();
            sd.present = sd.blocks != 0;
        } else if (sdh.type == 'A') {
            if (sdh.wr_out) { sdh.wr_out = false; sd.wr_ack = sdh.wr_sent; }
        } else {                                         /* 'D' */
            uint8_t *dst = sdh.scratch;
            if (sdh.pt != sdh.ph) {
                __typeof__(sdh.pend[0]) *e = &sdh.pend[sdh.pt % SDH_PEND];
                if (e->gen == sdh.gen && sdh.ready == e->idx)
                    dst = &sd.slot[e->idx % SD_SLOTS][SD_SLOT_HDR];
                e->idx++;
                if (--e->n == 0) sdh.pt++;
            }
            sdh.dst = dst; sdh.dn = 0; sdh.dlen = 514;
        }
    }
}

/* Issue read-ahead requests and forward a written block. */
static inline void sdh_poll(void) {
    uint32_t g = sd.rd_gen;
    if (g != sdh.gen) {
        __sync_synchronize();
        sdh.gen = g; sdh.start = sd.rd_start; sdh.count = sd.rd_count;
        sdh.issued = 0; sdh.ready = 0;
        sd.rd_ready = (g & 0xFFFF) << 16;
    }
    if (sd.present && sdh.count) {
        uint32_t lim = sd.rd_consumed + SD_SLOTS;
        if (sdh.count != 0xFFFFFFFFu && lim > sdh.count) lim = sdh.count;
        if (sdh.issued < lim && sdh.ph - sdh.pt < SDH_PEND && HOST_WRITE_AVAIL() >= 7) {
            uint32_t k = lim - sdh.issued;
            uint32_t lba = sdh.start + sdh.issued;
            if (lba + k > sd.blocks) k = lba < sd.blocks ? sd.blocks - lba : 0;
            if (k) {
                uint8_t m[7] = { 'R', (uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16),
                                 (uint8_t)(lba >> 24), (uint8_t)k, (uint8_t)(k >> 8) };
                if (HOST_WRITE(m, 7)) {
                    __typeof__(sdh.pend[0]) *e = &sdh.pend[sdh.ph % SDH_PEND];
                    e->gen = g; e->idx = sdh.issued; e->n = k;
                    sdh.ph++;
                    sdh.issued += k;
                }
            }
        }
    }
    if (!sdh.wr_out && sd.wr_seq != sdh.wr_sent && HOST_WRITE_AVAIL() >= 517) {
        uint32_t seq = sd.wr_seq;
        __sync_synchronize();
        uint32_t lba = sd.wr_lba;
        uint8_t m[5] = { 'W', (uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24) };
        if (HOST_WRITE(m, 5) && HOST_WRITE(sd.wr_buf, 512)) {
            sdh.wr_sent = seq;
            sdh.wr_out = true;
        }
    }
}
