/* uf2_model.h: the cable's update over the ESP32's USB port, against a simulated RP2040 flash
   (erase = 0xFF, a write can only clear bits, as on the chip). With a .uf2 and its .bin as
   arguments the real build is written and compared too:
     test_uf2 build/dc_usb_serial.uf2 build/dc_usb_serial.bin */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "../uf2_model.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define FL (2u * 1024u * 1024u)
static uint8_t flash[FL];
static int erases, writes;

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void mkblock(uint8_t *b, uint32_t flags, uint32_t addr, uint32_t no, uint32_t n, uint32_t fam, uint8_t fill) {
    memset(b, 0, 512);
    put32(b, UF2_MAGIC0); put32(b + 4, UF2_MAGIC1); put32(b + 8, flags); put32(b + 12, addr); put32(b + 16, 256);
    put32(b + 20, no); put32(b + 24, n); put32(b + 28, fam);
    for (int i = 0; i < 256; i++) b[32 + i] = (uint8_t)(fill + i * 3);
    put32(b + 508, UF2_MAGICE);
}
/* cableflash.c's block() against the simulated flash; returns the result */
static uf2_res_t flash_block(uf2_erased_t *e, const uint8_t *b, uint32_t *total, uint32_t *written) {
    uint32_t addr, nblk, sec;
    uf2_res_t r = uf2_parse(b, &addr, &nblk);
    if (r != UF2_OK) return r;
    if (!*total) *total = nblk;
    if (uf2_needs_erase(e, addr, &sec)) { if (sec - UF2_FLASH_BASE < FL) memset(flash + (sec - UF2_FLASH_BASE), 0xFF, 4096); erases++; }
    if (addr - UF2_FLASH_BASE + 256 <= FL) for (int i = 0; i < 256; i++) flash[addr - UF2_FLASH_BASE + i] &= b[32 + i];
    writes++;
    (*written)++;
    return UF2_OK;
}

int main(int argc, char **argv) {
    uint8_t b[512];
    uint32_t a, n;
    /* single blocks */
    mkblock(b, UF2_F_FAMILY, 0x10000000, 0, 1, UF2_RP2040, 1);
    CHECK(uf2_parse(b, &a, &n) == UF2_OK && a == 0x10000000 && n == 1, "a plain block");
    mkblock(b, UF2_F_FAMILY, 0x10000000, 0, 1, 0xADA52840, 1);           /* nRF52840 */
    CHECK(uf2_parse(b, &a, &n) == UF2_WRONG_FAMILY, "another chip's UF2 must be refused");
    mkblock(b, UF2_F_FAMILY, 0x20000000, 0, 1, UF2_RP2040, 1);           /* RAM */
    CHECK(uf2_parse(b, &a, &n) == UF2_OUTSIDE, "a block for RAM must be refused");
    mkblock(b, UF2_F_FAMILY, 0x10000080, 0, 1, UF2_RP2040, 1);
    CHECK(uf2_parse(b, &a, &n) == UF2_OUTSIDE, "a block not on a 256-byte page");
    mkblock(b, UF2_F_FAMILY | UF2_F_NOFLASH, 0x10000000, 0, 1, UF2_RP2040, 1);
    CHECK(uf2_parse(b, &a, &n) == UF2_SKIP, "a not-for-flash block is skipped");
    mkblock(b, 0, 0x10000000, 0, 1, 0, 1); b[508] = 0;
    CHECK(uf2_parse(b, &a, &n) == UF2_BAD_END, "a damaged end magic");
    memcpy(b, "MZ\x90\x00", 4);
    CHECK(uf2_parse(b, &a, &n) == UF2_BAD_MAGIC, "not a UF2 at all");

    /* a file whose blocks come out of order and write over old contents: what was there before must go */
    memset(flash, 0x00, sizeof flash);                                   /* old firmware: all bits low */
    uf2_erased_t e; memset(&e, 0, sizeof e);
    uint32_t total = 0, written = 0;
    static const uint32_t order[] = { 5, 0, 17, 1, 16, 2, 3, 4 };       /* pages; 16/17 are in sector 1 */
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
        mkblock(b, UF2_F_FAMILY, 0x10000000 + order[i] * 256, i, 8, UF2_RP2040, (uint8_t)order[i]);
        CHECK(flash_block(&e, b, &total, &written) == UF2_OK, "block %u", i);
    }
    bool same = true;
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
        uint8_t want[256]; for (int k = 0; k < 256; k++) want[k] = (uint8_t)(order[i] + k * 3);
        if (memcmp(flash + order[i] * 256, want, 256)) same = false;
    }
    CHECK(same, "out-of-order blocks over old contents read back different");
    CHECK(erases == 2 && writes == 8 && written == total, "erases %d (want 2), writes %d, %u/%u", erases, writes, written, total);
    /* untouched pages of an erased sector are blank (0xFF), not the old firmware */
    CHECK(flash[6 * 256] == 0xFF && flash[15 * 256 + 255] == 0xFF, "rest of an erased sector is not blank");
    CHECK(flash[2 * 4096] == 0x00, "a sector no block lands in must stay as it was");

    /* the real build */
    if (argc > 2) {
        FILE *fu = fopen(argv[1], "rb"), *fb = fopen(argv[2], "rb");
        CHECK(fu && fb, "cannot open %s / %s", argv[1], argv[2]);
        if (fu && fb) {
            memset(flash, 0x5A, sizeof flash); memset(&e, 0, sizeof e); total = written = 0; erases = writes = 0;
            int bad = 0;
            while (fread(b, 1, 512, fu) == 512) if (flash_block(&e, b, &total, &written) != UF2_OK) bad++;
            static uint8_t bin[FL]; size_t bn = fread(bin, 1, sizeof bin, fb);
            bool ok = !bad && written == total && bn > 0 && memcmp(flash, bin, bn) == 0;
            printf("%s the real %s: %u blocks, %d erases, flash == .bin (%zu bytes)\n", ok ? "PASS" : "FAIL", argv[1], written, erases, bn);
            fails += !ok;
        }
        if (fu) fclose(fu);
        if (fb) fclose(fb);
    }
    printf(fails ? "uf2: %d failures\n" : "uf2: all ok\n", fails);
    return fails != 0;
}
