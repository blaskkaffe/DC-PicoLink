/*
 * The SD card over the link for the ESP32's file access (sdlink_model.h), end to end on the
 * host: ESP-IDF's FatFs (as on the ESP32) on top of the ESP32 side, the link with little
 * room (so answers wait), the cable side, and a card image in RAM (formatted FAT32 here).
 * Built by tools/run-tests.sh when IDF_PATH is set (it copies FatFs from ESP-IDF).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ff.h"
#include "diskio.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the card and the cable ---- */
#define BLOCKS (300u * 2048u)                 /* 300 MB: FAT32 with 4 KB clusters (pages untouched cost nothing) */
static uint8_t *img;
static bool card_in = true, dc_has_it = true;
static uint32_t now_us, dc_last, card_reads, card_writes;
static int fail_lba = -1;

static bool rd(uint32_t lba, bool run, uint8_t *d) { (void)run; if (!card_in || lba >= BLOCKS || (int)lba == fail_lba) return false; memcpy(d, img + 512u * lba, 512); card_reads++; return true; }
static bool wr(uint32_t lba, const uint8_t *s) { if (!card_in || lba >= BLOCKS || (int)lba == fail_lba) return false; memcpy(img + 512u * lba, s, 512); card_writes++; return true; }

/* the link to the ESP32: at most 3 packets waiting (a full ring makes answers wait) */
static uint8_t q[3][600]; static size_t ql[3]; static int qn;
static uint32_t send_refused;
static bool send(const uint8_t *p, size_t n) { if (qn == 3) { send_refused++; return false; } memcpy(q[qn], p, n); ql[qn++] = n; return true; }

#define SDLK_SEND(p, n)      send((p), (n))
#define SDLK_READ(l, r, d)   rd((l), (r), (d))
#define SDLK_WRITE(l, s)     wr((l), (s))
#define SDLK_HAVE_CARD()     card_in
#define SDLK_BLOCKS()        BLOCKS
#define SDLK_PI()            false
#define SDLK_DC_LAST_US()    dc_last
#define SDLK_TAKE()          (dc_has_it = false)
#define SDLK_GIVE()          (dc_has_it = true)
#include "sdlink_model.h"

static sdlk_cable_t zc;
static sdlk_esp_t ze;
static uint16_t tag;

/* run the cable and deliver to the ESP32 side until done() or a 2 s timeout */
static void pump(bool (*done)(void)) {
    for (int i = 0; i < 100000 && !done(); i++) {
        now_us += 50;
        sdlk_cable_poll(&zc, now_us);
        int take = qn && i % 3 == 0 ? 1 : 0;      /* the ESP32 takes them slower than the cable sends */
        for (int k = 0; k < take && qn; k++) {
            sdlk_esp_rx(&ze, q[0], ql[0]);
            memmove(q[0], q[1], sizeof q[0] * 2); memmove(ql, ql + 1, sizeof ql[0] * 2); qn--;
        }
    }
}
static uint8_t ask;
static bool got_state(void) { return ze.ask == ask && ze.state_seq; }
static bool got_all(void) { return ze.failed || ze.got == ze.n; }
static bool got_ack(void) { return ze.failed || ze.written; }
static void to_cable(const uint8_t *p, size_t n) { CHECK(sdlk_cable_rx(&zc, p, n, now_us)); }

static bool take(bool on) {
    uint8_t m[3] = { 'T', on, ++ask };
    ze.ask = (uint8_t)(ask - 1);
    to_cable(m, 3);
    pump(got_state);
    return (ze.state & SDLK_TAKEN) != 0;
}

/* ---- FatFs disk on the ESP32 side: as esp32/main/sdfs.c ---- */
DSTATUS disk_status(BYTE pd) { (void)pd; return (ze.state & SDLK_TAKEN) ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pd) { return disk_status(pd); }
DRESULT disk_read(BYTE pd, BYTE *buf, LBA_t lba, UINT count) {
    (void)pd;
    while (count) {
        UINT k = count > SDLK_MAX_RUN ? SDLK_MAX_RUN : count;
        uint8_t m[8] = { 'R', 0, 0, 0, 0, 0, 0, (uint8_t)k };
        ze.tag = ++tag; ze.lba0 = (uint32_t)lba; ze.n = (uint8_t)k; ze.got = 0; ze.dst = buf; ze.failed = false;
        m[1] = (uint8_t)tag; m[2] = (uint8_t)(tag >> 8); sdlk_put32(m + 3, (uint32_t)lba);
        to_cable(m, 8);
        pump(got_all);
        ze.dst = NULL;
        if (ze.failed || ze.got != k) return RES_ERROR;
        buf += 512u * k; lba += k; count -= k;
    }
    return RES_OK;
}
DRESULT disk_write(BYTE pd, const BYTE *buf, LBA_t lba, UINT count) {
    (void)pd;
    for (UINT i = 0; i < count; i++) {
        static uint8_t m[7 + 512];
        m[0] = 'W'; ze.tag = ++tag; ze.failed = false; ze.written = false;
        m[1] = (uint8_t)tag; m[2] = (uint8_t)(tag >> 8); sdlk_put32(m + 3, (uint32_t)lba + i);
        memcpy(m + 7, buf + 512u * i, 512);
        to_cable(m, sizeof m);
        pump(got_ack);
        if (!ze.written) return RES_ERROR;
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pd, BYTE cmd, void *b) {
    (void)pd;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)b = ze.blocks; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)b = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)b = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
const PARTITION VolToPart[FF_VOLUMES] = { { 0, 0 }, { 1, 0 } };   /* as ESP-IDF's diskio.c */
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (11u << 21) | (19u << 16); }
void *ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }

int main(void) {
    img = calloc(BLOCKS, 512);
    /* the Dreamcast used the card just now: refused, with the reason */
    now_us = 10000000; dc_last = now_us - 1000000;
    CHECK(!take(true) && strstr(ze.why, "Dreamcast is using") && dc_has_it);
    CHECK(ze.blocks == BLOCKS && (ze.state & SDLK_CARD) && (ze.state & SDLK_DC_BUSY));
    /* blocks before the take: answered as failed, nothing read */
    { uint8_t b[512]; CHECK(disk_read(0, b, 0, 1) == RES_ERROR && card_reads == 0); }
    /* quiet for 3 s: taken; the Dreamcast loses the card */
    now_us += SDLK_DC_QUIET_US;
    CHECK(take(true) && !dc_has_it);

    /* format (a computer would have done that), mount, folders and files as openMenu keeps them */
    FATFS fs; static BYTE work[4096];
    MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 4096 };   /* 4 KB clusters: runs of 8 blocks */
    CHECK(f_mkfs("0:", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "0:", 1) == FR_OK && fs.fs_type == FS_FAT32);
    CHECK(f_mkdir("0:/VMU") == FR_OK && f_mkdir("0:/VMU/Phantasy Star Online Ver.2") == FR_OK);
    static uint8_t data[150000];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 31 + (i >> 9));
    FIL f; UINT bw = 0, br = 0;
    CHECK(f_open(&f, "0:/VMU/Phantasy Star Online Ver.2/vmu_save_A1.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    CHECK(f_write(&f, data, sizeof data, &bw) == FR_OK && bw == sizeof data);
    CHECK(f_close(&f) == FR_OK);
    /* read back */
    static uint8_t back[sizeof data];
    CHECK(f_open(&f, "0:/VMU/Phantasy Star Online Ver.2/vmu_save_A1.bin", FA_READ) == FR_OK);
    CHECK(f_read(&f, back, sizeof back, &br) == FR_OK && br == sizeof data && !memcmp(back, data, sizeof data));
    CHECK(f_close(&f) == FR_OK);
    /* a listing */
    FF_DIR d; FILINFO fi; int files = 0; bool seen = false;
    CHECK(f_opendir(&d, "0:/VMU/Phantasy Star Online Ver.2") == FR_OK);
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) { files++; if (!strcmp(fi.fname, "vmu_save_A1.bin") && fi.fsize == sizeof data) seen = true; }
    f_closedir(&d);
    CHECK(files == 1 && seen);
    /* answers had to wait for room on the link at times, and nothing was lost */
    CHECK(send_refused > 0);
    printf("  %u blocks read, %u written; the link was full %u times\n", card_reads, card_writes, send_refused);

    /* a block the card cannot read: an error for that file, the card stays usable */
    f_unmount("0:");
    FIL g; CHECK(f_mount(&fs, "0:", 1) == FR_OK);
    CHECK(f_open(&g, "0:/VMU/Phantasy Star Online Ver.2/vmu_save_A1.bin", FA_READ) == FR_OK);
    fail_lba = (int)(fs.database + (g.obj.sclust - 2) * fs.csize);   /* its first block */
    CHECK(f_read(&g, back, 512, &br) != FR_OK);
    f_close(&g);
    fail_lba = -1;
    CHECK(f_stat("0:/VMU", &fi) == FR_OK);
    /* delete */
    CHECK(f_unlink("0:/VMU/Phantasy Star Online Ver.2/vmu_save_A1.bin") == FR_OK);
    CHECK(f_stat("0:/VMU/Phantasy Star Online Ver.2/vmu_save_A1.bin", &fi) == FR_NO_FILE);
    f_unmount("0:");

    /* give back: the Dreamcast has its card again */
    CHECK(!take(false) && dc_has_it);
    /* taken and then forgotten: back to the Dreamcast after SDLK_IDLE_US */
    now_us += SDLK_DC_QUIET_US; dc_last = 0;
    CHECK(take(true) && !dc_has_it);
    now_us += SDLK_IDLE_US + 1000;
    sdlk_cable_poll(&zc, now_us);
    CHECK(dc_has_it && !zc.taken);
    /* the card pulled while taken: back to the Dreamcast's side, and the ESP32 is told */
    now_us += SDLK_DC_QUIET_US;
    CHECK(take(true));
    card_in = false;
    { uint32_t s0 = ze.state_seq; for (int i = 0; i < 1000 && (ze.state_seq == s0 || (ze.state & SDLK_TAKEN)); i++) pump(got_state), now_us += 1000, sdlk_cable_poll(&zc, now_us), pump(got_state); }
    while (qn) { sdlk_esp_rx(&ze, q[0], ql[0]); memmove(q[0], q[1], sizeof q[0] * 2); memmove(ql, ql + 1, sizeof ql[0] * 2); qn--; }
    CHECK(!(ze.state & SDLK_TAKEN) && !(ze.state & SDLK_CARD) && dc_has_it);
    card_in = true;
    /* a write request while busy is held by the link (false), not lost */
    now_us += SDLK_DC_QUIET_US;
    CHECK(take(true));
    zc.op = 'R'; zc.left = 1;
    { uint8_t m[8] = { 'R', 1, 0, 0, 0, 0, 0, 1 }; CHECK(!sdlk_cable_rx(&zc, m, 8, now_us)); }
    zc.op = 0;
    take(false);

    free(img);
    printf(fails ? "sdlink: %d FAILED\n" : "sdlink: all ok (take/give, Dreamcast busy, FAT32 write/read/list/delete over the link, read error, idle give-back, card pulled)\n", fails);
    return fails != 0;
}
