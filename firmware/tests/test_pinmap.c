/* Zero-stack pin map: the cable's pins (firmware/main.c) against the ESP32's (esp32/sdkconfig.zero-stack).
 * Both the soldered stack and the stripboard use this one map, so it is checked for:
 *  - the SD card on hardware SPI1 (SCK 10/14/26, MOSI 11/15/27, MISO 8/12/28) and the link on SPI0
 *  - no pin used twice on the cable, none of the Dreamcast's GP9-13, the sense pin GP29 or the LED GP16
 *  - in the stack (Zero face up over a face-down SuperMini, both counted from the USB end):
 *      Zero right GP0..GP8 sits over ESP32 43,44,1,2,3,4,5,6,7, so every link signal must be a
 *      straight pair GPn <-> ESP32 pin over it; Zero left GP29,28,27,26,15,14 sits over ESP32
 *      13,12,11,10,9,8: the W5500 pins live there and are never joined, so no link signal may. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static int in(int v, const int *a, int n) { for (int i = 0; i < n; i++) if (a[i] == v) return 1; return 0; }
#define IN(v, ...) in((v), (int[]){ __VA_ARGS__ }, (int)(sizeof((int[]){ __VA_ARGS__ }) / sizeof(int)))

static int def_in_block(const char *src, const char *name, int zero_stack) {
    /* value of "#define name N" in main.c; zero_stack: inside "#if DC_PIN_BASE == 9" ... "#else" if there is one */
    const char *p = src; char key[64]; snprintf(key, sizeof key, "#define %s ", name);
    int inz = 0, found = -1;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[256]; snprintf(line, sizeof line, "%.*s", (int)(n < 255 ? n : 255), p);
        if (!strncmp(line, "#if DC_PIN_BASE == 9", 20)) inz = 1;
        else if (inz && !strncmp(line, "#else", 5)) inz = 2;
        else if (!strncmp(line, "#endif", 6)) inz = 0;
        if (!strncmp(line, key, strlen(key)) && (inz == 0 || (inz == 1) == zero_stack))
            found = atoi(line + strlen(key));
        if (!e) break;
        p = e + 1;
    }
    return found;
}
static char *slurp(const char *f) {
    FILE *fp = fopen(f, "rb"); if (!fp) { printf("cannot open %s\n", f); exit(1); }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
    char *b = malloc(n + 1); if (fread(b, 1, n, fp) != (size_t)n) exit(1); b[n] = 0; fclose(fp); return b;
}
static int cfg(const char *s, const char *name) {
    char key[96]; snprintf(key, sizeof key, "\n%s=", name);
    const char *p = strstr(s, key); CHECK(p); return atoi(p + strlen(key));
}

int main(void) {
    char *m = slurp("main.c"), *c = slurp("../esp32/sdkconfig.zero-stack");
    int sck = def_in_block(m, "PIN_SDC_SCK", 1), mosi = def_in_block(m, "PIN_SDC_MOSI", 1);
    int miso = def_in_block(m, "PIN_SDC_MISO", 1), cs = def_in_block(m, "PIN_SDC_CS", 1);
    int l_dr = def_in_block(m, "PIN_LSPI_DR", 1), l_hs = def_in_block(m, "PIN_LSPI_HS", 1);
    int l_miso = def_in_block(m, "PIN_LSPI_MISO", 1), l_cs = def_in_block(m, "PIN_LSPI_CS", 1);
    int l_sck = def_in_block(m, "PIN_LSPI_SCK", 1), l_mosi = def_in_block(m, "PIN_LSPI_MOSI", 1);
    printf("SD  sck %d mosi %d miso %d cs %d | link dr %d hs %d miso %d cs %d sck %d mosi %d\n",
           sck, mosi, miso, cs, l_dr, l_hs, l_miso, l_cs, l_sck, l_mosi);
    /* hardware SPI: the SD card on SPI1, the link on SPI0 */
    CHECK(IN(sck, 10, 14, 26) && IN(mosi, 11, 15, 27) && IN(miso, 8, 12, 28));
    CHECK(IN(l_miso, 0, 4, 16, 20) && IN(l_sck, 2, 6, 18, 22) && IN(l_mosi, 3, 7, 19, 23));
    /* the cable's pins are all different and clear of the Dreamcast (GP9-13), the sense pin (GP29) and the LED (GP16) */
    int pins[] = { sck, mosi, miso, cs, l_dr, l_hs, l_miso, l_cs, l_sck, l_mosi };
    int np = (int)(sizeof pins / sizeof pins[0]);
    for (int i = 0; i < np; i++) {
        CHECK(pins[i] >= 0 && pins[i] <= 28 && pins[i] != 16 && !(pins[i] >= 9 && pins[i] <= 13));
        for (int j = i + 1; j < np; j++) CHECK(pins[i] != pins[j]);
    }
    /* the ESP32's side */
    int e_dr = cfg(c, "CONFIG_DCL_LSPI_DR"), e_hs = cfg(c, "CONFIG_DCL_LSPI_HS"), e_miso = cfg(c, "CONFIG_DCL_LSPI_MISO");
    int e_cs = cfg(c, "CONFIG_DCL_LSPI_CS"), e_sck = cfg(c, "CONFIG_DCL_LSPI_SCLK"), e_mosi = cfg(c, "CONFIG_DCL_LSPI_MOSI");
    int w[] = { cfg(c, "CONFIG_DCL_W5500_MOSI"), cfg(c, "CONFIG_DCL_W5500_SCLK"), cfg(c, "CONFIG_DCL_W5500_CS"),
                cfg(c, "CONFIG_DCL_W5500_INT"), cfg(c, "CONFIG_DCL_W5500_MISO") };
    /* the stack: Zero GPn (n = 0..8) sits over this ESP32 pin; each link signal is a straight pair */
    static const int over[9] = { 43, 44, 1, 2, 3, 4, 5, 6, 7 };
    CHECK(e_dr == over[l_dr] && e_hs == over[l_hs] && e_miso == over[l_miso]);
    CHECK(e_cs == over[l_cs] && e_sck == over[l_sck] && e_mosi == over[l_mosi]);
    /* the six link pins are one run: GP3..GP8 over GPIO2..7 */
    int lo = 99, hi = -1; int lp[] = { l_dr, l_hs, l_miso, l_cs, l_sck, l_mosi };
    for (int i = 0; i < 6; i++) { if (lp[i] < lo) lo = lp[i]; if (lp[i] > hi) hi = lp[i]; }
    CHECK(hi - lo == 5);
    /* the W5500 is on the ESP32's right column (GPIO13..8, the row of 3V3/GND), five different pins, none a link pin */
    int ep[] = { e_dr, e_hs, e_miso, e_cs, e_sck, e_mosi };
    for (int i = 0; i < 5; i++) {
        CHECK(w[i] >= 8 && w[i] <= 13);
        for (int j = i + 1; j < 5; j++) CHECK(w[i] != w[j]);
        for (int j = 0; j < 6; j++) CHECK(w[i] != ep[j]);
    }
    /* the Zero pads over them (GP29,28,27,26,15,14 over GPIO13..8) carry the SD card and the sense wire: never soldered */
    CHECK(!IN(l_dr, 29, 28, 27, 26, 15, 14) && !IN(l_hs, 29, 28, 27, 26, 15, 14));
    printf("zero-stack pin map ok: SD SPI1 %d/%d/%d cs %d, link GP%d..GP%d over GPIO%d..%d, W5500 GPIO%d,%d,%d,%d,%d\n",
           sck, mosi, miso, cs, lo, hi, over[lo], over[hi], w[0], w[1], w[2], w[3], w[4]);
    return 0;
}
