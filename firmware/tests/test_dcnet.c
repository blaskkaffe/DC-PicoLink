/* Host test of dcnet_model.h: which calls go to DCNet, the port rule, discovery packets and
   the '~' fix (compared with a copy of dcnet.c's loop). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dcnet_model.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* dcnet.c (flyinghead), the inbuf -> inbuffixed loop, one byte at a time */
static size_t ref_fix(int *marker, const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        if (*marker) { *marker = 0; if (in[i] != '~') { out[o++] = '~'; continue; } }
        else if (in[i] == '~') *marker = 1;
        out[o++] = in[i++];
    }
    return o;
}

int main(void) {
    char d[32], num[32];
    CHECK(dcnet_digits("T*69,0053600100 05", d, sizeof d) == 15 && !strcmp(d, "*690053600100" "05"));
    CHECK(dcnet_digits("P1-800-555", d, sizeof d) == 7 && !strcmp(d, "1800555"));

    CHECK(dcnet_wanted(DCNET_PREFIX, "*6912345", false, num, sizeof num) && !strcmp(num, "12345"));
    CHECK(!dcnet_wanted(DCNET_PREFIX, "12345", false, num, sizeof num) && !strcmp(num, "12345"));
    CHECK(dcnet_wanted(DCNET_PREFIX, "12345", true, num, sizeof num));
    CHECK(dcnet_wanted(DCNET_ALWAYS, "12345", false, num, sizeof num));
    CHECK(!dcnet_wanted(DCNET_OFF, "*6912345", true, num, sizeof num) && !strcmp(num, "12345"));

    CHECK(dcnet_port_for("005360010001") == DCNET_PORT_ALT);
    CHECK(dcnet_port_for("005360010010") == DCNET_PORT_ALT);
    CHECK(dcnet_port_for("005360010013") == DCNET_PORT_ALT);
    CHECK(dcnet_port_for("005360010016") == DCNET_PORT_ALT);
    CHECK(dcnet_port_for("005360010011") == DCNET_PORT);
    CHECK(dcnet_port_for("005360010000") == DCNET_PORT);
    CHECK(dcnet_port_for("005360010017") == DCNET_PORT);
    CHECK(dcnet_port_for("0053600100011") == DCNET_PORT);
    CHECK(dcnet_port_for("1234") == DCNET_PORT);

    uint8_t b[64];
    CHECK(dcnet_disco_query(b) == 5 && !memcmp(b, "\x01\xC0\x15\xDC\x03", 5));
    CHECK(dcnet_ping(b, 0x1122334455667788ull) == 13 && b[4] == 1 && b[5] == 0x88 && b[12] == 0x11);
    const uint8_t ans[] = { 1, 0xC0, 0x15, 0xDC, 3,  10, 0, 0, 1, 6, 'E','u','r','o','p','e',  10, 0, 0, 2, 7, 'U','S',' ','E','a','s','t' };
    uint32_t ip[4]; char name[4][32];
    CHECK(dcnet_disco_answer(ans, sizeof ans, ip, name, 4) == 2);
    CHECK(!memcmp(&ip[0], "\x0a\x00\x00\x01", 4) && !strcmp(name[0], "Europe") && !strcmp(name[1], "US East"));
    CHECK(dcnet_disco_answer(ans, 20, ip, name, 4) == 1);                  /* cut short */
    CHECK(dcnet_disco_answer(ans + 1, sizeof ans - 1, ip, name, 4) == 0);  /* no magic */

    /* the '~' fix: random streams, random chunking, same as dcnet.c */
    srand(7);
    for (int t = 0; t < 2000; t++) {
        uint8_t in[300], a[700], r[700];
        size_t n = (size_t)(rand() % 300);
        for (size_t i = 0; i < n; i++) in[i] = (rand() % 3) ? '~' : (uint8_t)(rand() % 256);
        bool seen = false; int mk = 0; size_t na = 0, nr = 0, p = 0;
        while (p < n) {
            size_t c = 1 + (size_t)(rand() % 40); if (c > n - p) c = n - p;
            na += dcnet_fix_to_dc(&seen, in + p, c, a + na);
            nr += ref_fix(&mk, in + p, c, r + nr);
            p += c;
        }
        CHECK(na == nr && !memcmp(a, r, na));
        if (fails) break;
    }
    const uint8_t s1[] = "~}#~~x~y";
    uint8_t o1[32]; bool sn = false;
    size_t k = dcnet_fix_to_dc(&sn, s1, 8, o1);
    CHECK(k == 10 && !memcmp(o1, "~~}#~~x~~y", 10));

    printf(fails ? "dcnet: %d FAILED\n" : "dcnet: all ok\n", fails);
    return fails != 0;
}
