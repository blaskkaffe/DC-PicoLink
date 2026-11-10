/* IPv4 rules on Ethernet frames (ipfilter_model.h): TTL 64, PowerSmash double SYN, what must pass untouched */
#include <stdio.h>
#include <stdlib.h>
#include "../ipfilter_model.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static size_t frame(uint8_t *f, bool vlan, uint16_t type, uint8_t ttl, uint8_t proto, uint32_t dst, uint16_t sport, uint8_t flags) {
    size_t o = 0;
    memset(f, 0, 200);
    memset(f, 0xAA, 6); memset(f + 6, 0xBB, 6); o = 12;
    if (vlan) { f[o++] = 0x81; f[o++] = 0x00; f[o++] = 0x00; f[o++] = 0x05; }
    f[o++] = (uint8_t)(type >> 8); f[o++] = (uint8_t)type;
    uint8_t *ip = f + o;
    ip[0] = 0x45; ip[2] = 0; ip[3] = 40; ip[8] = ttl; ip[9] = proto;
    ip[12] = 192; ip[13] = 168; ip[14] = 99; ip[15] = 20;
    ip[16] = (uint8_t)(dst >> 24); ip[17] = (uint8_t)(dst >> 16); ip[18] = (uint8_t)(dst >> 8); ip[19] = (uint8_t)dst;
    uint16_t c = ipf__sum(ip, 20); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    ip[20] = (uint8_t)(sport >> 8); ip[21] = (uint8_t)sport; ip[33] = flags;
    return o + 40;
}

int main(void) {
    ipf_t h; memset(&h, 0, sizeof h);
    uint8_t f[200]; uint8_t g[200];

    size_t n = frame(f, false, 0x0800, 30, 17, 0x08080808, 5000, 0);
    memcpy(g, f, n);
    CHECK(ipf_ether(&h, f, n, 0));
    CHECK(f[14 + 8] == 65 && ipf__sum(f + 14, 20) == 0);                   /* checksum is still valid */
    CHECK(!memcmp(f, g, 14) && !memcmp(f + 14 + 12, g + 14 + 12, n - 14 - 12));   /* nothing else changed */
    CHECK(h.ttl_fixed == 1);
    CHECK(ipf_ether(&h, f, n, 0) && h.ttl_fixed == 1);                      /* already 65: no change */

    n = frame(f, true, 0x0800, 3, 1, 0x01010101, 0, 0);                     /* behind a VLAN tag */
    CHECK(ipf_ether(&h, f, n, 0) && f[18 + 8] == 65 && ipf__sum(f + 18, 20) == 0);

    n = frame(f, false, 0x0806, 3, 1, 0x01010101, 0, 0);                    /* ARP: untouched */
    memcpy(g, f, n); CHECK(ipf_ether(&h, f, n, 0) && !memcmp(f, g, n));
    n = frame(f, false, 0x86DD, 3, 1, 0x01010101, 0, 0);                    /* IPv6: untouched */
    memcpy(g, f, n); CHECK(ipf_ether(&h, f, n, 0) && !memcmp(f, g, n));
    CHECK(ipf_ether(&h, f, 13, 0) && ipf_ether(&h, f, 20, 0));              /* short: passes */
    n = frame(f, false, 0x0800, 3, 1, 0x01010101, 0, 0); f[14] = 0x65;     /* version 6 in an IPv4 frame */
    memcpy(g, f, n); CHECK(ipf_ether(&h, f, n, 0) && !memcmp(f, g, n));
    f[14] = 0x4F; CHECK(ipf_ether(&h, f, 40, 0));                           /* header longer than the frame: passes */

    /* PowerSmash */
    n = frame(f, false, 0x0800, 64, 6, 0x0A0B0C0D, 3203, 0x02);
    CHECK(ipf_ether(&h, f, n, 1000));
    n = frame(f, false, 0x0800, 64, 6, 0x0A0B0C0D, 3203, 0x02);
    CHECK(!ipf_ether(&h, f, n, 1100) && h.syn_dropped == 1);
    n = frame(f, false, 0x0800, 64, 6, 0x0A0B0C0D, 3203, 0x10);             /* an ACK is never looked at */
    CHECK(ipf_ether(&h, f, n, 1101));
    n = frame(f, false, 0x0800, 64, 6, 0x0A0B0C0D, 4000, 0x02);             /* other source port */
    CHECK(ipf_ether(&h, f, n, 1102));
    n = frame(f, true, 0x0800, 64, 6, 0x0A0B0C0D, 3203, 0x02);              /* and behind a VLAN tag */
    CHECK(!ipf_ether(&h, f, n, 1103) && h.syn_dropped == 2);
    n = frame(f, false, 0x0800, 64, 6, 0x0A0B0C0D, 3203, 0x02);
    CHECK(ipf_ether(&h, f, n, 2500));                                       /* more than a second after the last drop */

    printf("ipfilter: ok (ip %u, ttl fixed %u, syn dropped %u)\n", h.ip_packets, h.ttl_fixed, h.syn_dropped);
    return 0;
}
