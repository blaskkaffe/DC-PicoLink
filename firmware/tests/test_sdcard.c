/* Host test of sdcard_model.h against a simulated SPI-mode SD card: init (SDHC with CSD
   2.0, SDSC with CSD 1.0 and byte addresses, no card), reads with latency, writes with
   busy, and the block source feeding sd.slot[] for sd_model.h's card side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define SD_PUSH(b) ((void)(b))
#define SD_DMA(b, l) ((void)(b), (void)(l))
#define SD_DMA_BUSY() false
#define SD_FLUSH() ((void)0)
#define SD_FILL(v) ((void)(v))
#define HOST_WRITE(p, n) false
#define HOST_WRITE_AVAIL() 0u
#include "../sd_model.h"
sd_shared_t sd; sd_card_t sdc; sd_host_t sdh;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the simulated card ---- */
#define PBLK 4096u
static uint8_t pimg[PBLK][512];
static struct {
    bool inserted, sdhc, v2, selected, app;
    int idle_left;             /* ACMD41 answers "busy" this many times */
    uint8_t cmd[6]; int cn;
    uint8_t out[700]; int on, op;
    int wr_state, wr_n; uint32_t wr_lba; uint8_t wr[512];
    int busy;                  /* bytes of busy after a write (kept across deselect) */
    int latency;               /* 0xFF bytes before a data token */
    int fail_reads;            /* the next N block reads answer an error */
    uint32_t cmds;
} card;
static uint32_t clock_us;

static void out_push(uint8_t b) { card.out[card.on++] = b; }
static void out_ffs(int n) { while (n--) out_push(0xFF); }

static uint32_t addr_blk(uint32_t a) { return card.sdhc ? a : a / 512; }

static void exec(void) {
    uint8_t c = card.cmd[0] & 0x3F;
    uint32_t a = (uint32_t)card.cmd[1] << 24 | (uint32_t)card.cmd[2] << 16 | (uint32_t)card.cmd[3] << 8 | card.cmd[4];
    card.cmds++;
    card.on = card.op = 0;
    out_push(0xFF);                                   /* Ncr = 1 */
    bool app = card.app; card.app = false;
    if (c == 0) { out_push(0x01); card.idle_left = 3; return; }
    if (c == 8) { if (!card.v2) { out_push(0x05); return; } out_push(0x01); out_push(0); out_push(0); out_push(0x01); out_push(0xAA); return; }
    if (c == 55) { card.app = true; out_push(card.idle_left ? 0x01 : 0x00); return; }
    if (c == 41 && app) { if (card.idle_left) { card.idle_left--; out_push(0x01); } else out_push(0x00); return; }
    if (c == 58) { out_push(0x00); out_push(card.sdhc ? 0xC0 : 0x80); out_push(0xFF); out_push(0x80); out_push(0x00); return; }
    if (c == 16) { out_push(card.sdhc ? 0x04 : 0x00); return; }
    if (c == 9) {
        uint8_t csd[16] = { 0 };
        if (card.sdhc) { csd[0] = 0x40; uint32_t cs = PBLK / 1024 - 1; csd[7] = (uint8_t)(cs >> 16); csd[8] = (uint8_t)(cs >> 8); csd[9] = (uint8_t)cs; }
        else {                                        /* 2 MiB: READ_BL_LEN 9, C_SIZE_MULT 7, C_SIZE 7 */
            csd[0] = 0x00; csd[5] = 0x09; uint32_t cs = 7; csd[6] = (uint8_t)(cs >> 10); csd[7] = (uint8_t)(cs >> 2); csd[8] = (uint8_t)(cs << 6);
            csd[9] = 0x03; csd[10] = 0x80;
        }
        out_push(0x00); out_ffs(2); out_push(0xFE);
        for (int i = 0; i < 16; i++) out_push(csd[i]);
        out_push(0); out_push(0);
        return;
    }
    if (c == 17) {
        uint32_t b = addr_blk(a);
        if (b >= PBLK) { out_push(0x40); return; }
        if (card.fail_reads) { card.fail_reads--; out_push(0x08); return; }      /* CRC/communication error */
        out_push(0x00); out_ffs(card.latency); out_push(0xFE);
        for (int i = 0; i < 512; i++) out_push(pimg[b][i]);
        uint16_t crc = sd_crc16(pimg[b], 512);
        out_push((uint8_t)(crc >> 8)); out_push((uint8_t)crc);
        return;
    }
    if (c == 24) {
        uint32_t b = addr_blk(a);
        if (b >= PBLK) { out_push(0x40); return; }
        out_push(0x00); card.wr_state = 1; card.wr_lba = b; return;
    }
    out_push(0x04);                                   /* illegal command */
}

static uint8_t xfer(uint8_t mosi) {
    clock_us += 1;
    if (!card.inserted || !card.selected) return 0xFF;
    if (card.wr_state == 1) { if (card.op < card.on) return card.out[card.op++]; if (mosi == 0xFE) { card.wr_state = 2; card.wr_n = 0; } return 0xFF; }
    if (card.wr_state == 2) {
        if (card.wr_n < 512) card.wr[card.wr_n] = mosi;
        if (++card.wr_n == 514) {
            memcpy(pimg[card.wr_lba], card.wr, 512);
            card.wr_state = 3;
        }
        return 0xFF;
    }
    if (card.wr_state == 3) { card.wr_state = 0; card.busy = 300; return 0xE5; }   /* xxx0 0101 accepted */
    if (card.busy) { card.busy--; return 0x00; }
    uint8_t miso = card.op < card.on ? card.out[card.op++] : 0xFF;
    if (card.cn == 0 && (mosi & 0xC0) == 0x40) card.cmd[card.cn++] = mosi;
    else if (card.cn) { card.cmd[card.cn++] = mosi; if (card.cn == 6) { card.cn = 0; exec(); } }
    return miso;
}

static int speed;
#define SDL_XFER(b) xfer(b)
#define SDL_CS(on) (card.selected = (on), card.cn = card.selected ? card.cn : 0)
#define SDL_SLOW() (speed = 1)
#define SDL_FAST() (speed = 2)
#define SDL_NOW_US() (clock_us)
#include "../sdcard_model.h"

static void fill_img(uint32_t seed) {
    for (uint32_t b = 0; b < PBLK; b++) for (int i = 0; i < 512; i++) pimg[b][i] = (uint8_t)(b * 7 + (uint32_t)i * 13 + seed);
}

int main(void) {
    sdl_card_t c;
    /* no card */
    memset(&card, 0, sizeof card);
    CHECK(!sdl_init(&c) && c.why && strstr(c.why, "no card"));

    /* SDHC */
    memset(&card, 0, sizeof card); card.inserted = card.sdhc = card.v2 = true; card.latency = 40;
    fill_img(1);
    CHECK(sdl_init(&c) && c.sdhc && c.blocks == PBLK && speed == 2);
    uint8_t b[514];
    CHECK(sdl_read(&c, 123, b) && !memcmp(b, pimg[123], 512));
    uint16_t crc = sd_crc16(pimg[123], 512);
    CHECK(b[512] == (uint8_t)(crc >> 8) && b[513] == (uint8_t)crc);
    uint8_t w[512]; memset(w, 0x5A, sizeof w);
    CHECK(sdl_write(&c, 77, w) && c.busy);
    int polls = 0; while (sdl_write_busy(&c)) polls++;
    CHECK(polls > 100 && !memcmp(pimg[77], w, 512));
    CHECK(!sdl_read(&c, PBLK + 5, b));                  /* out of range: error */

    /* SDSC (byte addresses, CSD 1.0, CMD16) */
    memset(&card, 0, sizeof card); card.inserted = true; card.v2 = false; card.sdhc = false;
    fill_img(9);
    CHECK(sdl_init(&c) && !c.sdhc && c.blocks == 4096);
    CHECK(sdl_read(&c, 3000, b) && !memcmp(b, pimg[3000], 512));

    /* as the block source: a multi-block read stream and a write, like the card side does */
    memset(&card, 0, sizeof card); card.inserted = card.sdhc = card.v2 = true; card.latency = 3;
    fill_img(3);
    CHECK(sdl_init(&c));
    sd_card_init();
    sdl_src_t s;
    sdl_src_attach(&s, &c);
    CHECK(sd.present && sd.blocks == PBLK);
    sd_start_stream(1000, 0xFFFFFFFFu);                 /* CMD18 at 1000 */
    uint32_t got = 0;
    for (int it = 0; it < 2000 && got < 40; it++) {
        CHECK(sdl_src_poll(&s, &c, 2));
        uint32_t r = sd.rd_ready;
        CHECK((r >> 16) == (sd.rd_gen & 0xFFFF));
        uint32_t avail = (uint16_t)((r & 0xFFFF) - (uint16_t)sd.rd_consumed);
        CHECK(avail <= SD_SLOTS);
        if (avail) {                                      /* the card side takes one */
            uint8_t *sl = sd.slot[sd.rd_consumed % SD_SLOTS];
            CHECK(sl[0] == 0xFF && sl[2] == 0xFE);
            CHECK(!memcmp(sl + 3, pimg[1000 + got], 512));
            uint16_t cc = sd_crc16(pimg[1000 + got], 512);
            CHECK(sl[515] == (uint8_t)(cc >> 8) && sl[516] == (uint8_t)cc);
            sd.rd_consumed++; got++;
        }
        if (fails) break;
    }
    CHECK(got == 40);
    sd_stop_stream();
    sd_start_stream(5, 1);                               /* CMD17 at 5: a new stream */
    for (int it = 0; it < 10; it++) sdl_src_poll(&s, &c, 2);
    CHECK((sd.rd_ready & 0xFFFF) == 1 && !memcmp(sd.slot[0] + 3, pimg[5], 512));
    memset(sd.wr_buf, 0xC3, 512); sd.wr_lba = 9; sd.wr_seq++;
    int n = 0; while (sd.wr_ack != sd.wr_seq && n < 10000) { sdl_src_poll(&s, &c, 2); n++; }
    CHECK(sd.wr_ack == sd.wr_seq && pimg[9][0] == 0xC3 && pimg[9][511] == 0xC3);
    /* one failed read is retried, the card stays */
    card.fail_reads = 1;
    sd_start_stream(60, 1);
    for (int it = 0; it < 10; it++) CHECK(sdl_src_poll(&s, &c, 2));
    CHECK((sd.rd_ready & 0xFFFF) == 1 && !memcmp(sd.slot[0] + 3, pimg[60], 512));
    /* two in a row: gone */
    card.fail_reads = 2;
    sd_start_stream(61, 1);
    CHECK(!sdl_src_poll(&s, &c, 2));
    sdl_src_attach(&s, &c);
    /* a card stuck busy after a write: after 1 s the write is acknowledged (the DC is not left
       waiting) and the card counts as gone */
    memset(sd.wr_buf, 0x5A, 512); sd.wr_lba = 11; sd.wr_seq++;
    CHECK(sdl_src_poll(&s, &c, 2));                      /* the write starts */
    card.busy = 1 << 30;
    bool gone = false; n = 0;
    while (!gone && n < 100000) { gone = !sdl_src_poll(&s, &c, 2); n++; clock_us += 100; }
    CHECK(gone && sd.wr_ack == sd.wr_seq);
    card.busy = 0;
    /* card pulled: the next read fails, the source reports it, detach keeps the DC from hanging */
    sd_start_stream(50, 0xFFFFFFFFu);
    card.inserted = false;
    CHECK(!sdl_src_poll(&s, &c, 2));
    sd.wr_seq++;
    sdl_src_detach(&s);
    CHECK(!sd.present && sd.wr_ack == sd.wr_seq);

    printf(fails ? "sdcard: %d FAILED\n" : "sdcard: all ok (SDHC, SDSC, none, reads, writes, block source, read retry, stuck write)\n", fails);
    return fails != 0;
}
