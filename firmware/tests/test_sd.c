/* Host test: KallistiOS sd.c (SCIF-SPI) transcribed byte-for-byte against the SD
   card model, with the RP2040 PIO behaviour (MISO for slot k+1 is pulled before
   the CPU sees byte k, 4-deep TX FIFO refilled by DMA) and a simulated Pi with
   configurable latency. Optionally talks to the real dcsd.py over a pty. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

/* ---- simulated PIO TX path ---- */
static uint8_t fifo[4]; static int fn;
static const uint8_t *dma_p; static int dma_n;
static uint8_t fill = 0xFF;
static void dma_refill(void) { while (fn < 4 && dma_n > 0) { fifo[fn++] = *dma_p++; dma_n--; } }
#define SD_PUSH(b)      do { assert(dma_n == 0 && fn < 4); fifo[fn++] = (b); } while (0)
#define SD_DMA(buf,len) do { assert(dma_n == 0); dma_p = (buf); dma_n = (len); dma_refill(); } while (0)
#define SD_DMA_BUSY()   (dma_n > 0)
#define SD_FLUSH()      do { fn = 0; dma_n = 0; } while (0)
#define SD_FILL(v)      (fill = (v))

/* ---- device <-> Pi byte pipes ---- */
static uint8_t to_pi[1 << 20]; static size_t tp_w, tp_r;
static uint8_t to_dev[1 << 22]; static size_t td_w, td_r;
#define HOST_WRITE_AVAIL() ((uint32_t)4096 - (uint32_t)(tp_w - tp_r) > 4096 ? 0 : (uint32_t)4096 - (uint32_t)(tp_w - tp_r))
static void compact(uint8_t *b, size_t *r, size_t *w) {
    if (*r == *w) { *r = *w = 0; return; }
    if (*r > 65536) { memmove(b, b + *r, *w - *r); *w -= *r; *r = 0; }
}
static bool host_write(const uint8_t *p, uint32_t n) {
    compact(to_pi, &tp_r, &tp_w);
    if (HOST_WRITE_AVAIL() < n) return false;
    memcpy(&to_pi[tp_w], p, n); tp_w += n; return true;
}
#define HOST_WRITE(p,n) host_write((const uint8_t *)(p), (n))

#include "../sd_model.h"
sd_shared_t sd; sd_card_t sdc; sd_host_t sdh;

/* ---- simulated (or real) Pi ---- */
#define NBLK (8u * 1024u)                 /* 4 MiB image */
static uint8_t img[NBLK][512];
static int pi_latency = 0;                /* bytes of SPI time per Pi action */
static int pi_clock;
static int real_fd = -1;                  /* pty to dcsd.py */

static void pi_sim(void) {
    if (++pi_clock < pi_latency) return;
    pi_clock = 0;
    while (tp_r < tp_w) {
        uint8_t t = to_pi[tp_r];
        size_t avail = tp_w - tp_r;
        if (t == 'H') {
            tp_r++;
            uint8_t m[6] = { 'I', NBLK & 255, (NBLK >> 8) & 255, (NBLK >> 16) & 255, NBLK >> 24, 0 };
            memcpy(&to_dev[td_w], m, 6); td_w += 6;
        } else if (t == 'R') {
            if (avail < 7) return;
            uint32_t lba = sdh_le32(&to_pi[tp_r + 1]);
            uint32_t k = to_pi[tp_r + 5] | (to_pi[tp_r + 6] << 8);
            tp_r += 7;
            for (uint32_t i = 0; i < k; i++) {
                uint8_t *d = &to_dev[td_w];
                d[0] = 'D'; memcpy(&d[1], &(uint32_t){lba + i}, 4);
                memcpy(&d[5], img[lba + i], 512);
                uint16_t c = sd_crc16(img[lba + i], 512);
                d[517] = c >> 8; d[518] = (uint8_t)c;
                td_w += 519;
            }
            return;                       /* one request per latency period */
        } else if (t == 'W') {
            if (avail < 517) return;
            uint32_t lba = sdh_le32(&to_pi[tp_r + 1]);
            memcpy(img[lba], &to_pi[tp_r + 5], 512);
            tp_r += 517;
            uint8_t a[6] = { 'A', 0, 0, 0, 0, 0 };
            memcpy(&a[1], &lba, 4);
            memcpy(&to_dev[td_w], a, 6); td_w += 6;
            return;
        } else { fprintf(stderr, "bad byte to pi %02x\n", t); abort(); }
    }
}

static void pi_real(void) {
    if (tp_r < tp_w) { ssize_t k = write(real_fd, &to_pi[tp_r], tp_w - tp_r); if (k > 0) tp_r += (size_t)k; }
    uint8_t buf[4096];
    ssize_t k = read(real_fd, buf, sizeof buf);
    if (k > 0) { memcpy(&to_dev[td_w], buf, (size_t)k); td_w += (size_t)k; }
}

static void core0(void) {
    compact(to_dev, &td_r, &td_w);
    if (real_fd >= 0) pi_real(); else pi_sim();
    if (td_r < td_w) {                    /* deliver in USB-ish chunks */
        uint32_t n = (uint32_t)(td_w - td_r); if (n > 64) n = 64;
        sdh_rx(&to_dev[td_r], n); td_r += n;
    }
    sdh_poll();
}

/* ---- the SPI wire ---- */
static bool cs;
static uint8_t osr = 0xFF;                /* MISO byte for the current slot */
static long bytes_clocked;
#include <time.h>
static uint8_t spi_rw_byte(uint8_t mosi) {
    bytes_clocked++;
    if (real_fd >= 0) {                   /* real daemon: ~2 us per byte, like the DC */
        static struct timespec last; struct timespec now;
        do clock_gettime(CLOCK_MONOTONIC, &now);
        while ((now.tv_sec - last.tv_sec) * 1000000000L + (now.tv_nsec - last.tv_nsec) < 2000);
        last = now;
    }
    uint8_t miso = 0xFF;
    if (cs) {
        miso = osr;
        /* PIO: push RX, then immediately pull the next slot */
        if (fn) { osr = fifo[0]; memmove(fifo, fifo + 1, (size_t)--fn); dma_refill(); }
        else osr = fill;
        sd_rx(mosi);                      /* core1 sees byte k after slot k+1 is taken */
        sd_poll();
    }
    core0();
    return miso;
}
static void spi_set_cs(bool on) {         /* true = asserted (low) */
    if (cs && !on) {                      /* CS rise: firmware restarts the SM */
        sd_frame_end(); SD_FLUSH();
    }
    if (!cs && on) {                      /* SM restarted: first slot pulls fresh */
        if (fn) { osr = fifo[0]; memmove(fifo, fifo + 1, (size_t)--fn); dma_refill(); } else osr = fill;
    }
    cs = on;
}
static void spi_read_data(uint8_t *b, int n) { while (n--) *b++ = spi_rw_byte(0xFF); }
static void spi_write_data(const uint8_t *b, int n) { while (n--) spi_rw_byte(*b++); }
#define spi_read_byte() spi_rw_byte(0xFF)
#define spi_write_byte(b) spi_rw_byte(b)

/* ---- KOS sd.c, transcribed ---- */
#define CMD(n) ((n) | 0x40)
static bool byte_mode, initted;
static int sd_wait_ready(void) {
    spi_rw_byte(0xFF);
    for (long i = 0; i < 400000; i++) if (spi_rw_byte(0xFF) == 0xFF) return 0;
    return -1;
}
static int sd_send_cmd(uint8_t cmd, uint32_t arg) {
    if (sd_wait_ready()) return -1;
    uint8_t pkt[6] = { cmd, arg >> 24, arg >> 16, arg >> 8, arg, 0 };
    pkt[5] = (uint8_t)((sd_crc7(pkt, 5) << 1) | 1);
    for (int i = 0; i < 6; i++) spi_rw_byte(pkt[i]);
    if (cmd == CMD(12)) spi_rw_byte(0xFF);
    uint8_t rv; int i = 0;
    do { rv = spi_rw_byte(0xFF); ++i; } while ((rv & 0x80) && i < 20);
    if (rv & 0x80) return -1;
    return rv;
}
static int acmd41_loop(uint32_t arg) {
    int i = 0, rv;
    while (i++ < 5000) {
        if (sd_send_cmd(CMD(55), 0) > 1) return -1;
        if ((rv = sd_send_cmd(CMD(41), arg)) > 1) return -1;
        if (rv == 0) break;
    }
    return i == 5000 ? -2 : 0;
}
static int sd_init(void) {
    uint8_t buf[4];
    for (int i = 0; i < 10; ++i) spi_rw_byte(0xFF);
    spi_set_cs(true);
    if (sd_send_cmd(CMD(0), 0) != 1) { spi_set_cs(false); return -1; }
    if (sd_send_cmd(CMD(8), 0x1AA) == 1) {
        for (int i = 0; i < 4; i++) buf[i] = spi_rw_byte(0xFF);
        if ((buf[2] & 0x0F) != 1 || buf[3] != 0xAA) { spi_set_cs(false); return -2; }
        if (acmd41_loop(0x40000000)) { spi_set_cs(false); return -3; }
        if (sd_send_cmd(CMD(58), 0)) { spi_set_cs(false); return -4; }
        for (int i = 0; i < 4; i++) buf[i] = spi_rw_byte(0xFF);
        if (!(buf[0] & 0x40)) byte_mode = true;
    } else { spi_set_cs(false); return -5; }
    if (sd_send_cmd(CMD(59), 1)) { spi_set_cs(false); return -6; }
    spi_set_cs(false); spi_rw_byte(0xFF);
    initted = true;
    return 0;
}
static int read_data(int bytes, uint8_t *buf) {
    uint8_t b; int i = 0;
    do { b = spi_rw_byte(0xFF); ++i; } while (b == 0xFF && i < 50000);
    if (b != 0xFE) return -1;
    spi_read_data(buf, bytes);
    uint16_t crc = (uint16_t)((spi_read_byte() << 8) | spi_read_byte());
    return crc != sd_crc16(buf, bytes);
}
static int sd_read_blocks(uint32_t block, int count, uint8_t *buf) {
    int rv = 0;
    spi_set_cs(true);
    if (count == 1) {
        if (sd_send_cmd(CMD(17), block) || read_data(512, buf)) rv = -1;
    } else {
        if (sd_send_cmd(CMD(18), block)) rv = -1;
        else {
            while (count--) { if (read_data(512, buf)) { rv = -2; break; } buf += 512; }
            sd_send_cmd(CMD(12), 0);
        }
    }
    spi_set_cs(false); spi_rw_byte(0xFF);
    return rv;
}
static int write_data(uint8_t tag, const uint8_t *buf) {
    if (sd_wait_ready()) return -1;
    spi_write_byte(tag);
    uint16_t crc = sd_crc16(buf, 512);
    spi_write_data(buf, 512);
    spi_write_byte(crc >> 8); spi_write_byte((uint8_t)crc);
    uint8_t rv = spi_read_byte();
    return (rv & 0x1F) != 0x05 ? -1 : 0;
}
static int sd_write_blocks(uint32_t block, int count, const uint8_t *buf) {
    int rv = 0;
    spi_set_cs(true);
    if (count == 1) {
        if (sd_send_cmd(CMD(24), block) || write_data(0xFE, buf)) rv = -1;
    } else {
        sd_send_cmd(CMD(55), 0); sd_send_cmd(CMD(23), count);
        if (sd_send_cmd(CMD(25), block)) rv = -1;
        else {
            while (count--) { if (write_data(0xFC, buf)) { rv = -2; break; } buf += 512; }
            if (sd_wait_ready()) rv = -3;
            spi_rw_byte(0xFD);
        }
    }
    spi_set_cs(false); spi_rw_byte(0xFF);
    return rv;
}
static uint64_t sd_get_size(void) {
    uint8_t csd[16]; uint64_t rv = 0;
    spi_set_cs(true);
    if (sd_send_cmd(CMD(9), 0) || read_data(16, csd)) rv = (uint64_t)-1;
    else if ((csd[0] >> 6) == 1)
        rv = ((((uint64_t)csd[9]) | (uint64_t)(csd[8] << 8) | ((uint64_t)(csd[7] & 0x3F) << 16)) + 1) << 19;
    spi_set_cs(false); spi_rw_byte(0xFF);
    return rv;
}

int main(int argc, char **argv) {
    uint32_t nblk = NBLK;
    if (argc > 2) {                        /* real daemon: test_sd dcsd.py image */
        real_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        grantpt(real_fd); unlockpt(real_fd);
        struct termios tio; int s = open(ptsname(real_fd), O_RDWR | O_NOCTTY);
        tcgetattr(s, &tio); cfmakeraw(&tio); tcsetattr(s, TCSANOW, &tio); close(s);
        FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); nblk = (uint32_t)(ftell(f) / 512) & ~1023u; fclose(f);
        if (fork() == 0) {
            execlp("python3", "python3", argv[1], "--image", argv[2], "--device", ptsname(real_fd), (char *)NULL);
            _exit(127);
        }
    }
    sd_card_init();
    sdh_reset();
    /* no backend yet: init must fail fast (openMenu probe case) */
    long t0 = bytes_clocked;
    int r = sd_init();
    printf("init without backend -> %d after %ld bytes\n", r, bytes_clocked - t0);
    assert(r < 0 && bytes_clocked - t0 < 200);

    sdh_connected();
    for (int i = 0; i < 20000 && !sd.present; i++) core0(), usleep(real_fd >= 0 ? 100 : 0);
    assert(sd.present);
    if (real_fd < 0) for (uint32_t b = 0; b < NBLK; b++) for (int i = 0; i < 512; i++) img[b][i] = (uint8_t)(b * 7 + i * 13 + (i >> 8));

    r = sd_init(); printf("init -> %d (byte_mode=%d)\n", r, byte_mode); assert(r == 0 && !byte_mode);
    uint64_t sz = sd_get_size(); printf("size %llu bytes\n", (unsigned long long)sz);
    assert(sz == (uint64_t)nblk * 512);

    static uint8_t buf[64 * 512], ref[64 * 512];
    srand(3);
    int lat[] = { 0, 3, 40, 400 };
    int rounds = real_fd >= 0 ? 300 : 1500;
    for (int round = 0; round < rounds; round++) {
        if (real_fd < 0) pi_latency = lat[round % 4];
        int cnt = 1 + (rand() % 3 == 0 ? 0 : rand() % 40);
        uint32_t lba = (uint32_t)rand() % (nblk - cnt);
        if (rand() & 1) {
            for (int i = 0; i < cnt * 512; i++) ref[i] = (uint8_t)rand();
            r = sd_write_blocks(lba, cnt, ref);
            assert(r == 0);
            memset(buf, 0, sizeof buf);
            r = sd_read_blocks(lba, cnt, buf);
            if (r || memcmp(buf, ref, cnt * 512)) { printf("verify fail round %d lba %u cnt %d r %d\n", round, lba, cnt, r); return 1; }
        } else {
            r = sd_read_blocks(lba, cnt, buf);
            if (r) { printf("read fail round %d r %d\n", round, r); return 1; }
            if (real_fd < 0 && memcmp(buf, img[lba], cnt * 512)) { printf("data mismatch round %d\n", round); return 1; }
        }
    }
    printf("OK: %d rounds of KOS reads/writes (1..40 blocks, Pi latency 0..400 byte-times), %ld SPI bytes\n",
           rounds, bytes_clocked);
    return 0;
}
