/* ethsrc_dc_ip: a fixed-address Dreamcast is found from its frames. */
#include <stdio.h>
#include "ethsrc_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

static uint32_t be(int a, int b, int c, int d) { uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d }; uint32_t v; memcpy(&v, x, 4); return v; }
static void frame(uint8_t *f, int a, int b, int c, int d) {
    memset(f, 0, 60); f[12] = 0x08; f[13] = 0x00; f[14] = 0x45;
    f[26] = (uint8_t)a; f[27] = (uint8_t)b; f[28] = (uint8_t)c; f[29] = (uint8_t)d;
}

int main(void) {
    uint8_t f[60];
    uint32_t gw = be(192, 168, 98, 1), mask = be(255, 255, 255, 0);
    frame(f, 192, 168, 98, 60);  CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == be(192, 168, 98, 60));
    frame(f, 192, 168, 98, 1);   CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == 0);    /* the box itself */
    frame(f, 0, 0, 0, 0);        CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == 0);    /* DHCP discover */
    frame(f, 192, 168, 1, 98);   CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == 0);    /* another network */
    frame(f, 192, 168, 98, 255); CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == 0);    /* broadcast */
    frame(f, 192, 168, 98, 60); f[12] = 0x08; f[13] = 0x06;                          /* ARP */
    CHECK(ethsrc_dc_ip(f, sizeof f, gw, mask) == 0);
    frame(f, 192, 168, 98, 60);  CHECK(ethsrc_dc_ip(f, 20, gw, mask) == 0);          /* too short */
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
