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


/* an IPv4 packet: 20-byte header, src a, dst b, protocol, l4 bytes; checksums valid */
static size_t pkt(uint8_t *ip, uint32_t a, uint32_t b, uint8_t proto, const uint8_t *l4, size_t l4n) {
    memset(ip, 0, 20 + l4n);
    ip[0] = 0x45; ip[2] = (uint8_t)((20 + l4n) >> 8); ip[3] = (uint8_t)(20 + l4n); ip[8] = 64; ip[9] = proto;
    ipf__put32(ip + 12, a); ipf__put32(ip + 16, b);
    memcpy(ip + 20, l4, l4n);
    ipf__fix_sums(ip, 20, ip + 20, l4n, proto == 6 ? 16 : 6);
    return 20 + l4n;
}
/* independent check: header sum and pseudo-header sum both fold to 0xFFFF */
static int sums_ok(const uint8_t *ip) {
    size_t tot = (size_t)(ip[2] << 8 | ip[3]);
    if (ipf__sum(ip, 20) != 0) return 0;
    uint32_t s = 0;
    for (int i = 12; i < 20; i += 2) s += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    s += ip[9]; s += (uint32_t)(tot - 20);
    for (size_t i = 20; i + 1 < tot; i += 2) s += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    if ((tot - 20) & 1) s += (uint32_t)ip[tot - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
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


    /* dcgamespy: SYN+PSH from the DC loses PSH; SYN alone and SYN+ACK+PSH untouched */
    {
        uint8_t ip[100], tcp[20] = {0x04,0x00, 0x1A,0x0A, 0,0,0,1, 0,0,0,0, 0x50, 0x0A, 0x20,0x00};
        size_t m = pkt(ip, 0x0A400002, 0x08080808, 6, tcp, 20);
        CHECK(ipf_packet(&h, ip, m, 5000) && ip[33] == 0x02 && sums_ok(ip) && h.gamespy_fixed == 1);
        tcp[13] = 0x02; m = pkt(ip, 0x0A400002, 0x08080808, 6, tcp, 20);
        CHECK(ipf_packet(&h, ip, m, 5000) && ip[33] == 0x02 && h.gamespy_fixed == 1);
        tcp[13] = 0x1A; m = pkt(ip, 0x0A400002, 0x08080808, 6, tcp, 20);
        CHECK(ipf_packet(&h, ip, m, 5000) && ip[33] == 0x1A && h.gamespy_fixed == 1);
    }
    /* dcvoip: UDP 1285->1285, UDP length 64: address at IP offset 70 */
    {
        uint8_t ip[200], u[64]; memset(u, 0x11, sizeof u);
        u[0] = 0x05; u[1] = 0x05; u[2] = 0x05; u[3] = 0x05; u[4] = 0; u[5] = 64;
        size_t m = pkt(ip, 0x0A400002, 0x05060708, 17, u, 64);
        CHECK(ipf_packet(&h, ip, m, 6000) && ipf__get32(ip + 70) == 0x11111111);   /* public IP unknown: one-way */
        h.public_ip = 0x51525354;
        m = pkt(ip, 0x0A400002, 0x05060708, 17, u, 64);
        CHECK(ipf_packet(&h, ip, m, 6000) && ipf__get32(ip + 70) == 0x51525354 && sums_ok(ip));
        m = pkt(ip, 0x05060708, 0x0A400002, 17, u, 64);                       /* to the DC: the sender's address */
        ipf_to_dc(&h, ip, m);
        CHECK(ipf__get32(ip + 70) == 0x05060708 && sums_ok(ip));
        u[5] = 32; m = pkt(ip, 0x05060708, 0x0A400002, 17, u, 32);            /* UDP length 32: offset 42 */
        ipf_to_dc(&h, ip, m);
        CHECK(ipf__get32(ip + 42) == 0x05060708 && sums_ok(ip));
        u[5] = 40; m = pkt(ip, 0x05060708, 0x0A400002, 17, u, 40);            /* other length: untouched */
        uint8_t g2[200]; memcpy(g2, ip, m); ipf_to_dc(&h, ip, m);
        CHECK(!memcmp(g2, ip, m) && h.voip_fixed == 3);
    }
    /* dc2k2: TCP to the DC, dport 5011, PSH+ACK, message type 1 gets the sender's address */
    {
        uint8_t ip[200], t2[60]; memset(t2, 0, sizeof t2);
        t2[0] = 0x13; t2[1] = 0x93; t2[2] = 0x13; t2[3] = 0x93; t2[12] = 0x50; t2[13] = 0x18;
        t2[20] = 1; t2[21] = 10; t2[22] = 0; t2[23] = 0; t2[24] = 5;          /* 40 payload bytes */
        size_t m = pkt(ip, 0xC0A80164, 0x0A400002, 6, t2, 60);
        ipf_to_dc(&h, ip, m);
        CHECK(ipf__get32(ip + 41) == 0xC0A80164 && sums_ok(ip) && h.k2_fixed == 1);
        t2[20] = 2; m = pkt(ip, 0xC0A80164, 0x0A400002, 6, t2, 60);           /* other message: untouched */
        ipf_to_dc(&h, ip, m); CHECK(ipf__get32(ip + 41) == 0x0A000005 && h.k2_fixed == 1);
        t2[20] = 1; t2[13] = 0x10; m = pkt(ip, 0xC0A80164, 0x0A400002, 6, t2, 60);   /* no PSH */
        ipf_to_dc(&h, ip, m); CHECK(h.k2_fixed == 1);
        t2[13] = 0x18; m = pkt(ip, 0xC0A80164, 0x0A400002, 6, t2, 20 + 25); /* 25 payload bytes: too short for the helper */
        ipf_to_dc(&h, ip, m); CHECK(h.k2_fixed == 1);
        m = pkt(ip, 0xC0A80164, 0x0A400002, 6, t2, 20 + 26);               /* 26: long enough */
        ipf_to_dc(&h, ip, m); CHECK(h.k2_fixed == 2);
    }
    printf("ipfilter: ok (ip %u, ttl fixed %u, syn dropped %u)\n", h.ip_packets, h.ttl_fixed, h.syn_dropped);
    return 0;
}
