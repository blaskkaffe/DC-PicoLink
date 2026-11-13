/* pubip_model.h: the myip.opendns.com query and its answer */
#include <stdio.h>
#include <stdlib.h>
#include "../pubip_model.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int main(void) {
    uint8_t q[64], a[128];
    size_t n = pubip_query(q, 0x1234);
    /* the bytes dig sends for this name (header aside) */
    const uint8_t name[] = {4,'m','y','i','p',7,'o','p','e','n','d','n','s',3,'c','o','m',0, 0,1, 0,1};
    CHECK(n == 12 + sizeof name && !memcmp(q + 12, name, sizeof name) && q[0] == 0x12 && q[5] == 1);
    /* an answer: the question, then a compressed-name A record 81.82.83.84 */
    memcpy(a, q, n); a[2] = 0x81; a[3] = 0x80; a[7] = 1;
    const uint8_t rr[] = {0xC0,0x0C, 0,1, 0,1, 0,0,0,0, 0,4, 81,82,83,84};
    memcpy(a + n, rr, sizeof rr);
    CHECK(pubip_answer(a, n + sizeof rr, 0x1234) == 0x51525354);
    CHECK(pubip_answer(a, n + sizeof rr, 0x9999) == 0);                  /* not our id */
    CHECK(pubip_answer(a, n + sizeof rr - 1, 0x1234) == 0);              /* cut short */
    a[3] = 0x83; CHECK(pubip_answer(a, n + sizeof rr, 0x1234) == 0);     /* NXDOMAIN */
    a[3] = 0x80; a[n + 12] = 192; a[n + 13] = 168;                       /* a private address: not public */
    CHECK(pubip_answer(a, n + sizeof rr, 0x1234) == 0);
    a[n + 12] = 100; a[n + 13] = 64;                                     /* carrier-grade NAT range */
    CHECK(pubip_answer(a, n + sizeof rr, 0x1234) == 0);
    printf("pubip: ok\n");
    return 0;
}
