/* ethtrace_model.h: one-line frame descriptions for the Ethernet session log */
#include <string.h>
#include "../ethtrace_model.h"

static int fails;
static void expect(const uint8_t *f, size_t n, const char *want) {
    char o[160];
    ethtrace_line(f, n, o, sizeof o);
    if (strcmp(o, want)) { printf("FAIL got \"%s\" want \"%s\"\n", o, want); fails++; }
}
static void ip_hdr(uint8_t *f, uint8_t proto, unsigned tot, const uint8_t s[4], const uint8_t d[4]) {
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *p = f + 14;
    p[0] = 0x45; p[2] = (uint8_t)(tot >> 8); p[3] = (uint8_t)tot; p[8] = 64; p[9] = proto;
    memcpy(p + 12, s, 4); memcpy(p + 16, d, 4);
}

int main(void) {
    static const uint8_t dc[4] = { 192, 168, 98, 2 }, box[4] = { 192, 168, 98, 1 }, far[4] = { 1, 2, 3, 4 };
    uint8_t f[600];
    /* ARP request and reply */
    memset(f, 0, sizeof f); f[12] = 0x08; f[13] = 0x06;
    f[14 + 7] = 1; memcpy(f + 14 + 14, dc, 4); memcpy(f + 14 + 24, box, 4);
    expect(f, 42, "ARP who has 192.168.98.1? (from 192.168.98.2)");
    f[14 + 7] = 2; memcpy(f + 14 + 14, box, 4);
    for (int i = 0; i < 6; i++) f[14 + 8 + i] = (uint8_t)(0xa0 + i);
    expect(f, 42, "ARP 192.168.98.1 is at a0:a1:a2:a3:a4:a5");
    /* DNS question and answer */
    memset(f, 0, sizeof f); ip_hdr(f, 17, 60, dc, box);
    uint8_t *u = f + 34; u[0] = 0x04; u[1] = 0x00; u[3] = 53;
    expect(f, 34 + 8 + 30, "UDP 192.168.98.2:1024 > 192.168.98.1:53 DNS question, 60 bytes");
    memset(f, 0, sizeof f); ip_hdr(f, 17, 80, box, dc);
    u = f + 34; u[1] = 53; u[2] = 0x04; u[8 + 3] = 0x83; u[8 + 7] = 0;
    expect(f, 34 + 8 + 40, "UDP 192.168.98.1:53 > 192.168.98.2:1024 DNS answer (rcode 3, 0 answers), 80 bytes");
    /* DHCP DISCOVER */
    memset(f, 0, sizeof f); static const uint8_t z[4] = { 0, 0, 0, 0 }, bc[4] = { 255, 255, 255, 255 };
    ip_hdr(f, 17, 300, z, bc);
    u = f + 34; u[1] = 68; u[3] = 67;
    uint8_t *opt = u + 8 + 240; opt[0] = 53; opt[1] = 1; opt[2] = 1; opt[3] = 255;
    expect(f, 34 + 8 + 248, "DHCP DISCOVER 0.0.0.0 > 255.255.255.255");
    if (!ethtrace_is_discover(f, 34 + 8 + 248)) { printf("FAIL discover not seen\n"); fails++; }
    opt[2] = 5;
    expect(f, 34 + 8 + 248, "DHCP ACK 0.0.0.0 > 255.255.255.255");
    if (ethtrace_is_discover(f, 34 + 8 + 248)) { printf("FAIL ack taken for a discover\n"); fails++; }
    /* TCP SYN, ICMP, other */
    memset(f, 0, sizeof f); ip_hdr(f, 6, 44, dc, far);
    uint8_t *t = f + 34; t[0] = 0x10; t[1] = 0x00; t[3] = 80; t[13] = 0x02;
    expect(f, 34 + 24, "TCP 192.168.98.2:4096 > 1.2.3.4:80 SYN, 44 bytes");
    t[13] = 0x12;
    expect(f, 34 + 24, "TCP 192.168.98.2:4096 > 1.2.3.4:80 SYN ACK, 44 bytes");
    memset(f, 0, sizeof f); ip_hdr(f, 1, 56, box, dc); f[34] = 3; f[35] = 3;
    expect(f, 34 + 8, "ICMP 192.168.98.1 > 192.168.98.2 type 3 code 3");
    memset(f, 0, sizeof f); f[12] = 0x86; f[13] = 0xdd;
    expect(f, 60, "type 86dd, 60 bytes");
    expect(f, 10, "short frame (10 bytes)");
    /* a truncated DHCP-looking frame must not read past the end */
    memset(f, 0, sizeof f); ip_hdr(f, 17, 40, z, bc); u = f + 34; u[1] = 68; u[3] = 67;
    expect(f, 34 + 8 + 10, "UDP 0.0.0.0:68 > 255.255.255.255:67, 40 bytes");
    printf(fails ? "ethtrace: %d failures\n" : "ethtrace: all ok\n", fails);
    return fails != 0;
}
