/* UF2 blocks for the cable's update over the ESP32's USB port (esp32/main/cableflash.c): which
   blocks go to the RP2040's flash, and where. Host-testable: firmware/tests/test_uf2.c. */
#pragma once
#include <stdint.h>
#include <string.h>

#define UF2_MAGIC0     0x0A324655u
#define UF2_MAGIC1     0x9E5D5157u
#define UF2_MAGICE     0x0AB16F30u
#define UF2_F_NOFLASH  0x00000001u
#define UF2_F_FAMILY   0x00002000u
#define UF2_RP2040     0xE48BFF56u
#define UF2_FLASH_BASE 0x10000000u
#define UF2_FLASH_MAX  (16u * 1024u * 1024u)

typedef enum { UF2_OK, UF2_SKIP, UF2_BAD_MAGIC, UF2_BAD_END, UF2_WRONG_FAMILY, UF2_OUTSIDE } uf2_res_t;

static inline uint32_t uf2_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* one 512-byte block: UF2_OK with the flash address, the block count of the file and the 256 bytes
   at b + 32; UF2_SKIP for a block that is not for the flash */
static uf2_res_t uf2_parse(const uint8_t *b, uint32_t *addr, uint32_t *nblk) {
    if (uf2_u32(b) != UF2_MAGIC0 || uf2_u32(b + 4) != UF2_MAGIC1) return UF2_BAD_MAGIC;
    if (uf2_u32(b + 508) != UF2_MAGICE) return UF2_BAD_END;
    uint32_t flags = uf2_u32(b + 8), a = uf2_u32(b + 12), size = uf2_u32(b + 16), fam = uf2_u32(b + 28);
    if ((flags & UF2_F_FAMILY) && fam != UF2_RP2040) return UF2_WRONG_FAMILY;
    if (flags & UF2_F_NOFLASH) return UF2_SKIP;
    if (size != 256 || (a & 255) || a < UF2_FLASH_BASE || a - UF2_FLASH_BASE > UF2_FLASH_MAX - 256) return UF2_OUTSIDE;
    *addr = a; *nblk = uf2_u32(b + 24);
    return UF2_OK;
}
static inline const char *uf2_res_text(uf2_res_t r) {
    switch (r) {
    case UF2_BAD_MAGIC: return "not a UF2 file";
    case UF2_BAD_END: return "a damaged UF2 block";
    case UF2_WRONG_FAMILY: return "this UF2 is not for an RP2040";
    case UF2_OUTSIDE: return "a UF2 block outside the flash";
    default: return "ok";
    }
}

/* the 4 KB sectors erased so far: a sector is erased the first time a block lands in it */
typedef struct { uint8_t bits[UF2_FLASH_MAX / 4096 / 8]; } uf2_erased_t;
/* true: erase sector (addr's) before writing; it is marked erased */
static inline int uf2_needs_erase(uf2_erased_t *e, uint32_t addr, uint32_t *sector_addr) {
    uint32_t sec = (addr - UF2_FLASH_BASE) / 4096;
    *sector_addr = UF2_FLASH_BASE + sec * 4096;
    if (e->bits[sec / 8] & (1u << (sec % 8))) return 0;
    e->bits[sec / 8] |= (uint8_t)(1u << (sec % 8));
    return 1;
}
