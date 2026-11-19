/*
 * A real SD card on the RP2040's own SPI bus, as the block source of the Dreamcast's SD
 * emulation (sd_model.h): instead of the Pi's dcsd program over USB, core0 reads the card
 * into sd.slot[] ahead of the card side and writes the Dreamcast's blocks to it.
 * The Pi, when its dcsd program is connected, takes precedence.
 *
 * The card is driven in SPI mode: CMD0, CMD8, ACMD41 (HCS), CMD58 (SDHC/SDXC or SDSC),
 * CMD9 (size); reads: CMD18 streams while the Dreamcast reads blocks in a row (one command
 * for the whole run, the card keeps its place between calls), CMD17 for single blocks;
 * writes: CMD24. Each block's CRC16 is checked here: a block damaged on the wires is read
 * again instead of reaching the Dreamcast; the card's own CRC goes on with the data.
 *
 * The includer defines:
 *   SDL_XFER(b)      clock one byte out, return the byte read
 *   SDL_READ(p,n), SDL_WRITE(p,n)  optional: n bytes in one go (0xFF out while reading);
 *                    default: SDL_XFER per byte
 *   SDL_CS(on)       true = card selected (CS low)
 *   SDL_SLOW()       400 kHz (card init)   SDL_FAST()  full speed (25 MHz or less)
 *   SDL_NOW_US()     a microsecond clock (uint32_t, wraps)
 * Host-compilable: tests/test_sdcard.c drives it against a simulated card.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef SDL_READ
#define SDL_READ(p, n)  do { for (uint32_t i_ = 0; i_ < (uint32_t)(n); i_++) (p)[i_] = SDL_XFER(0xFF); } while (0)
#endif
#ifndef SDL_WRITE
#define SDL_WRITE(p, n) do { for (uint32_t i_ = 0; i_ < (uint32_t)(n); i_++) SDL_XFER((p)[i_]); } while (0)
#endif

typedef struct {
    bool present, sdhc, busy;
    bool streaming;             /* a CMD18 is running; CS stays low between calls */
    uint32_t st_next;           /* the block it delivers next */
    uint32_t busy_since;        /* when the last write started programming */
    uint32_t blocks;            /* 512-byte blocks */
    uint32_t reads, writes, errors, crc_bad, cmds;
    uint8_t  last_r1;
    const char *why;            /* why the last init failed */
} sdl_card_t;

static inline uint8_t sdl_crc7(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        uint8_t d = p[i];
        for (int b = 0; b < 8; b++) {
            c <<= 1;
            if ((d ^ c) & 0x80) c ^= 0x09;
            d <<= 1;
        }
    }
    return (uint8_t)((c << 1) | 1);
}

/* CRC16-CCITT (XMODEM) of a block, by table: ~16 us for 512 bytes at 250 MHz */
static inline uint16_t sdl_crc16(const uint8_t *p, int n) {
    static uint16_t tab[256];
    if (!tab[1]) {
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
            tab[i] = c;
        }
    }
    uint16_t c = 0;
    for (int i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ tab[(uint8_t)((c >> 8) ^ p[i])]);
    return c;
}
static inline bool sdl_block_ok(const uint8_t *b514) {
    uint16_t c = sdl_crc16(b514, 512);
    return b514[512] == (uint8_t)(c >> 8) && b514[513] == (uint8_t)c;
}

/* wait until the card stops holding MISO low (busy); false after ms */
static inline bool sdl_ready(uint32_t ms) {
    uint32_t t0 = SDL_NOW_US();
    do { if (SDL_XFER(0xFF) == 0xFF) return true; } while ((uint32_t)(SDL_NOW_US() - t0) < ms * 1000u);
    return false;
}

/* one command (CS already low); returns R1, 0xFF = no answer */
static inline uint8_t sdl_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd != 0 && cmd != 12 && !sdl_ready(300)) return 0xFF;
    uint8_t f[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0 };
    f[5] = sdl_crc7(f, 5);
    SDL_WRITE(f, 6);
    if (cmd == 12) SDL_XFER(0xFF);                   /* stuff byte */
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r = SDL_XFER(0xFF)) & 0x80; i++) { }
    return r;
}

static inline void sdl_deselect(void) { SDL_CS(false); SDL_XFER(0xFF); }

/* Find and set up a card. true = ready (c->blocks, c->sdhc set). */
static inline bool sdl_init(sdl_card_t *c) {
    memset(c, 0, sizeof *c);
    SDL_SLOW();
    SDL_CS(false);
    for (int i = 0; i < 10; i++) SDL_XFER(0xFF);   /* >= 74 clocks with CS high */
    SDL_CS(true);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 0x01; i++) r = sdl_cmd(0, 0);
    if (r != 0x01) { sdl_deselect(); c->why = r == 0xFF ? "no card (no answer)" : "the card did not go idle"; c->last_r1 = r; return false; }
    bool v2 = false;
    r = sdl_cmd(8, 0x1AA);
    if (r == 0x01) {
        uint8_t e[4]; for (int i = 0; i < 4; i++) e[i] = SDL_XFER(0xFF);
        if ((e[2] & 0x0F) != 0x01 || e[3] != 0xAA) { sdl_deselect(); c->why = "unusable voltage (CMD8)"; return false; }
        v2 = true;
    }
    uint32_t t0 = SDL_NOW_US();
    do {
        r = sdl_cmd(55, 0);
        if (r > 1) break;
        r = sdl_cmd(41, v2 ? 0x40000000u : 0);
    } while (r == 0x01 && (uint32_t)(SDL_NOW_US() - t0) < 1000000u);
    if (r != 0x00) { sdl_deselect(); c->why = "the card stayed busy (ACMD41)"; c->last_r1 = r; return false; }
    c->sdhc = false;
    if (v2) {
        if (sdl_cmd(58, 0) != 0x00) { sdl_deselect(); c->why = "no OCR (CMD58)"; return false; }
        uint8_t o[4]; for (int i = 0; i < 4; i++) o[i] = SDL_XFER(0xFF);
        c->sdhc = (o[0] & 0x40) != 0;
    }
    if (!c->sdhc && sdl_cmd(16, 512) != 0x00) { sdl_deselect(); c->why = "block length 512 refused"; return false; }
    /* CSD: the size */
    if (sdl_cmd(9, 0) != 0x00) { sdl_deselect(); c->why = "no CSD (CMD9)"; return false; }
    uint8_t t = 0xFF;
    t0 = SDL_NOW_US();
    while ((t = SDL_XFER(0xFF)) == 0xFF && (uint32_t)(SDL_NOW_US() - t0) < 100000u) { }
    if (t != 0xFE) { sdl_deselect(); c->why = "no CSD data"; return false; }
    uint8_t csd[16]; for (int i = 0; i < 16; i++) csd[i] = SDL_XFER(0xFF);
    SDL_XFER(0xFF); SDL_XFER(0xFF);
    if ((csd[0] >> 6) == 1) {                        /* CSD 2.0 */
        uint32_t cs = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        c->blocks = (cs + 1) * 1024u;
    } else {                                         /* CSD 1.0 */
        uint32_t rbl = csd[5] & 0x0F;
        uint32_t cs = ((uint32_t)(csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        c->blocks = (cs + 1) << (mult + 2 + rbl - 9);
    }
    sdl_deselect();
    SDL_FAST();
    c->present = c->blocks != 0;
    if (!c->present) c->why = "size 0";
    return c->present;
}

/* the data token of a block, then 512 data + 2 CRC bytes into dst (CS low) */
static inline bool sdl_get_block(uint8_t *dst) {
    uint32_t t0 = SDL_NOW_US();
    uint8_t t;
    while ((t = SDL_XFER(0xFF)) == 0xFF && (uint32_t)(SDL_NOW_US() - t0) < 200000u) { }
    if (t != 0xFE) return false;
    SDL_READ(dst, 514);
    return true;
}

/* end a running CMD18 (before any other command) */
static inline void sdl_stream_stop(sdl_card_t *c) {
    if (!c->streaming) return;
    c->streaming = false;
    SDL_CS(true);
    c->cmds++;
    sdl_cmd(12, 0);
    sdl_ready(250);                                  /* busy after the stop */
    sdl_deselect();
}

/* Read one block (CMD17): dst gets 512 data bytes and the card's 2 CRC bytes. A block with
   a bad CRC is read once more. */
static inline bool sdl_read(sdl_card_t *c, uint32_t lba, uint8_t *dst) {
    sdl_stream_stop(c);
    bool ok = false;
    uint8_t r = 0xFF;
    for (int k = 0; k < 2 && !ok; k++) {
        SDL_CS(true);
        c->cmds++;
        r = sdl_cmd(17, c->sdhc ? lba : lba * 512u);
        bool got = r == 0x00 && sdl_get_block(dst);
        sdl_deselect();
        if (!got) break;
        if (sdl_block_ok(dst)) ok = true; else c->crc_bad++;
    }
    if (ok) c->reads++; else { c->errors++; c->last_r1 = r; }
    return ok;
}

/* Read the block lba of a run: continues the running CMD18 when lba is the next one,
   else starts one there. CS stays low between calls. false = start the run over with
   sdl_read (the stream is stopped). */
static inline bool sdl_stream_read(sdl_card_t *c, uint32_t lba, uint8_t *dst) {
    if (c->streaming && c->st_next != lba) sdl_stream_stop(c);
    if (!c->streaming) {
        SDL_CS(true);
        c->cmds++;
        uint8_t r = sdl_cmd(18, c->sdhc ? lba : lba * 512u);
        if (r != 0x00) { sdl_deselect(); c->errors++; c->last_r1 = r; return false; }
        c->streaming = true;
        c->st_next = lba;
    }
    if (!sdl_get_block(dst)) { sdl_stream_stop(c); c->errors++; return false; }
    c->st_next++;
    if (!sdl_block_ok(dst)) { c->crc_bad++; sdl_stream_stop(c); return false; }
    c->reads++;
    return true;
}

/* Start writing one block; then call sdl_write_done() until it is not busy any more.
   false = refused (the card is then treated as gone by the caller). */
static inline bool sdl_write(sdl_card_t *c, uint32_t lba, const uint8_t *data) {
    sdl_stream_stop(c);
    SDL_CS(true);
    c->cmds++;
    uint8_t r = sdl_cmd(24, c->sdhc ? lba : lba * 512u);
    if (r != 0x00) { sdl_deselect(); c->errors++; c->last_r1 = r; return false; }
    SDL_XFER(0xFF);
    SDL_XFER(0xFE);
    SDL_WRITE(data, 512);
    SDL_XFER(0xFF); SDL_XFER(0xFF);                  /* CRC: not checked in SPI mode */
    uint8_t resp = SDL_XFER(0xFF);
    if ((resp & 0x1F) != 0x05) { sdl_deselect(); c->errors++; c->last_r1 = resp; return false; }
    sdl_deselect();                                   /* the card stays busy by itself */
    c->busy = true;
    c->busy_since = SDL_NOW_US();
    c->writes++;
    return true;
}

/* true while the card is still programming the last written block (one look, no wait).
   A card busy for over 1 s (spec: 250 ms for SDXC) is stuck: c->errors goes up and *stuck is set. */
static inline bool sdl_write_busy_t(sdl_card_t *c, bool *stuck) {
    if (!c->busy) return false;
    if ((uint32_t)(SDL_NOW_US() - c->busy_since) > 1000000u) { c->busy = false; c->errors++; if (stuck) *stuck = true; return false; }
    SDL_CS(true);
    bool b = SDL_XFER(0xFF) != 0xFF;
    sdl_deselect();
    if (!b) c->busy = false;
    return b;
}
static inline bool sdl_write_busy(sdl_card_t *c) { return sdl_write_busy_t(c, 0); }

/* ---- as the block source of sd_model.h (sd_shared_t sd) ----
   The same contract as sdh_poll(): fill sd.slot[] for the current read stream at most
   SD_SLOTS blocks ahead of sd.rd_consumed, publish sd.rd_ready, and acknowledge written
   blocks once the card has them. A few blocks per call so the main loop keeps moving. */
#ifdef SD_SLOTS
typedef struct {
    uint32_t gen, start, count, ready;
    uint32_t wr_sent;           /* last wr_seq taken */
    bool wr_out;
} sdl_src_t;

static inline void sdl_src_attach(sdl_src_t *s, sdl_card_t *c) {
    memset(s, 0, sizeof *s);
    s->wr_sent = sd.wr_seq;
    s->gen = sd.rd_gen;
    sd.rd_ready = (s->gen & 0xFFFF) << 16;
    sd.blocks = c->blocks & ~1023u;                  /* whole 512 KiB units (CSD of the emulation) */
    sd.readonly = 0;
    __sync_synchronize();
    sd.present = sd.blocks != 0;
}

/* returns false when the card stopped answering (the caller detaches it) */
static inline bool sdl_src_poll(sdl_src_t *s, sdl_card_t *c, int max_blocks) {
    /* a written block first: the Dreamcast waits (busy) until it is acknowledged */
    if (s->wr_out) {
        bool stuck = false;
        if (sdl_write_busy_t(c, &stuck)) return true;
        s->wr_out = false;
        sd.wr_ack = s->wr_sent;
        if (stuck) return false;                      /* never finished: treat the card as gone (the Dreamcast is not left waiting) */
    }
    if (sd.wr_seq != s->wr_sent) {
        uint32_t seq = sd.wr_seq;
        __sync_synchronize();
        s->wr_sent = seq;
        /* one retry: a single refused write would otherwise drop the card (and the block) */
        if (sd.wr_lba >= sd.blocks || (!sdl_write(c, sd.wr_lba, sd.wr_buf) && !sdl_write(c, sd.wr_lba, sd.wr_buf))) { sd.wr_ack = seq; return sd.wr_lba >= sd.blocks; }
        s->wr_out = true;
        return true;
    }
    uint32_t g = sd.rd_gen;
    if (g != s->gen) {
        __sync_synchronize();
        s->gen = g; s->start = sd.rd_start; s->count = sd.rd_count; s->ready = 0;
        sd.rd_ready = (g & 0xFFFF) << 16;
    }
    for (int k = 0; k < max_blocks && s->count; k++) {
        uint32_t lim = sd.rd_consumed + SD_SLOTS;
        if (s->count != 0xFFFFFFFFu && lim > s->count) lim = s->count;
        uint32_t lba = s->start + s->ready;
        if (s->ready >= lim || lba >= sd.blocks) break;
        uint8_t *slot = &sd.slot[s->ready % SD_SLOTS][SD_SLOT_HDR];
        /* a run (CMD18 from the Dreamcast): one stream on the card too; a single block, or a
           stream that failed: CMD17 (which reads a bad-CRC block once more) */
        bool ok = (s->count != 1 && sdl_stream_read(c, lba, slot)) || sdl_read(c, lba, slot);
        if (!ok && !sdl_read(c, lba, slot)) return false;   /* one more try before the card counts as gone */
        if (sd.rd_gen != g) return true;               /* the Dreamcast moved on meanwhile */
        s->ready++;
        __sync_synchronize();
        sd.rd_ready = ((g & 0xFFFF) << 16) | (s->ready & 0xFFFF);
    }
    return true;
}

static inline void sdl_src_detach(sdl_src_t *s, sdl_card_t *c) {
    sdl_stream_stop(c);
    sd.present = 0;
    if (sd.wr_ack != sd.wr_seq) sd.wr_ack = sd.wr_seq;   /* don't hang the Dreamcast */
    s->wr_out = false;
}
#endif
