/* Host test for the W5500 -> SD switch rule in spi_slave.c (sd_detect): a frame
   is an SD command when it starts with >= 2 bytes of 0xFF (KOS sd_wait_ready()
   always clocks at least two) followed by a command byte 01xxxxxx, 4 argument
   bytes and a matching CRC7. det_step() mirrors sd_detect(); keep them in step.
   gcc -O1 -I. test/test_sd_detect.c && ./a.out */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define SD_PUSH(b) ((void)(b))
#define SD_DMA(b, l) ((void)(b), (void)(l))
#define SD_DMA_BUSY() false
#define SD_FLUSH() ((void)0)
#define SD_FILL(v) ((void)(v))
#define HOST_WRITE(p, n) false
#define HOST_WRITE_AVAIL() 0u
#include "sd_model.h"
sd_shared_t sd; sd_card_t sdc; sd_host_t sdh;

static int min_ff = 2;      /* the rule under test (net7: 1) */
/* returns true if the frame would be taken for an SD command */
static bool det_frame(const uint8_t *f, int len) {
    int n = 0, ff = 0; uint8_t buf[6];
    for (int i = 0; i < len; i++) {
        uint8_t b = f[i];
        if (n == 0) {
            if (b == 0xFF) { if (++ff > 32) return false; continue; }
            if (ff < min_ff || (b & 0xC0) != 0x40) return false;
        }
        buf[n++] = b;
        if (n < 6) continue;
        return ((sd_crc7(buf, 5) << 1) | 1) == buf[5];
    }
    return false;
}

/* KOS w5500_adapter frames: addr16, control (bsb<<3 | rw<<2 | om 00), data.
   MOSI is 0xFF while reading. Socket 0 only: bsb 0 common, 1 regs, 2 TX, 3 RX. */
static unsigned count_w5500_false(void) {
    unsigned hits = 0;
    uint8_t f[16];
    for (uint32_t a = 0; a < 0x10000; a++)
        for (int bsb = 0; bsb < 4; bsb++)
            for (int rw = 0; rw < 2; rw++)
                for (int len = 1; len <= 8; len++) {
                    f[0] = a >> 8; f[1] = (uint8_t)a; f[2] = (uint8_t)(bsb << 3 | rw << 2);
                    if (rw) for (int k = 0; k < len; k++) f[3 + k] = (uint8_t)(a * 7 + k * 13);   /* written data */
                    else memset(f + 3, 0xFF, len);
                    hits += det_frame(f, 3 + len);
                }
    return hits;
}

/* KOS sd.c sd_send_cmd(): sd_wait_ready() = one 0xFF, then 0xFF until the card
   answers 0xFF (>= 1 more), then the 6-byte packet, then 0xFF for the response. */
static unsigned count_sd_missed(void) {
    static const uint8_t cmds[] = { 0, 8, 9, 10, 12, 13, 16, 17, 18, 24, 25, 55, 41, 58 };
    unsigned missed = 0;
    for (unsigned c = 0; c < sizeof cmds; c++)
        for (int extra = 1; extra <= 4; extra++)
            for (uint32_t arg = 0; arg < 4096; arg += 7) {
                uint8_t f[32]; int n = 0;
                for (int k = 0; k < 1 + extra; k++) f[n++] = 0xFF;
                uint8_t p[6] = { (uint8_t)(0x40 | cmds[c]), arg >> 24, arg >> 16, arg >> 8, (uint8_t)arg };
                p[5] = (uint8_t)(sd_crc7(p, 5) << 1 | 1);
                memcpy(f + n, p, 6); n += 6;
                f[n++] = 0xFF; f[n++] = 0xFF;
                missed += !det_frame(f, n);
            }
    return missed;
}

int main(void) {
    int fails = 0;
    min_ff = 1;
    unsigned old_false = count_w5500_false();
    printf("net7 rule (>= 1 x 0xFF): %u W5500 frame shapes taken for SD commands\n", old_false);
    min_ff = 2;
    unsigned new_false = count_w5500_false(), missed = count_sd_missed();
    printf("%s new rule (>= 2 x 0xFF): %u W5500 frame shapes taken for SD commands\n", new_false ? "FAIL" : "PASS", new_false);
    printf("%s new rule: %u KOS SD commands missed\n", missed ? "FAIL" : "PASS", missed);
    fails += new_false != 0; fails += missed != 0; fails += old_false == 0;   /* the old bug must reproduce */
    printf(fails ? "FAILED\n" : "OK: no false SD switches from W5500 frames, every KOS SD command detected\n");
    return fails != 0;
}
