/* The Dreamcast's home-network address (dclan_model.h): DreamPi's pick, the 1:1 rewrite, ARP. */
#include <stdio.h>
#include <stdlib.h>
#include "../dclan_model.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- the pick ---- */
static uint32_t used_set[8]; static int used_n;
static bool used(void *ctx, uint32_t ip) { (void)ctx; for (int i = 0; i < used_n; i++) if (used_set[i] == ip) return true; return false; }

/* ---- full checksums to compare against ---- */
static uint16_t sum16(const uint8_t *d, size_t n, uint32_t s) {
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)d[i] << 8 | d[i + 1];
    if (n & 1) s += (uint32_t)d[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)s;
}
static uint16_t ip_csum(const uint8_t *ip) { uint8_t h[60]; size_t hl = (ip[0] & 15) * 4u; memcpy(h, ip, hl); h[10] = h[11] = 0; return (uint16_t)~sum16(h, hl, 0); }
static uint16_t l4_csum(const uint8_t *ip, size_t n, int csum_off) {
    size_t hl = (ip[0] & 15) * 4u;
    uint8_t b[1600]; memcpy(b, ip + hl, n - hl);
    b[csum_off] = b[csum_off + 1] = 0;
    uint32_t s = 0;
    s += sum16(ip + 12, 8, 0);
    s += ip[9];
    s += (uint32_t)(n - hl);
    return (uint16_t)~sum16(b, n - hl, s);
}
static void fill(uint8_t *ip, size_t n, uint8_t proto, uint32_t src, uint32_t dst) {
    memset(ip, 0, n);
    for (size_t i = 20; i < n; i++) ip[i] = (uint8_t)(i * 7 + 3);
    ip[0] = 0x45; ip[2] = (uint8_t)(n >> 8); ip[3] = (uint8_t)n; ip[8] = 64; ip[9] = proto;
    for (int k = 0; k < 4; k++) { ip[12 + k] = (uint8_t)(src >> (24 - 8 * k)); ip[16 + k] = (uint8_t)(dst >> (24 - 8 * k)); }
    uint16_t c = ip_csum(ip); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    if (proto == 6) { c = l4_csum(ip, n, 16); ip[36] = (uint8_t)(c >> 8); ip[37] = (uint8_t)c; }
    if (proto == 17) { ip[24] = (uint8_t)((n - 20) >> 8); ip[25] = (uint8_t)(n - 20); c = l4_csum(ip, n, 6); if (!c) c = 0xffff; ip[26] = (uint8_t)(c >> 8); ip[27] = (uint8_t)c; }
}
static uint8_t g(void *c, size_t o) { return ((uint8_t *)c)[o]; }
static void pu(void *c, size_t o, uint8_t v) { ((uint8_t *)c)[o] = v; }

int main(void) {
    uint32_t t, d;
    /* the usual home network: .99 for the box's end of PPP, .98 for the Dreamcast (DreamPi's values) */
    used_n = 0;
    CHECK(dcl_pick(IP(192, 168, 1, 1), IP(192, 168, 1, 186), 0, used, NULL, &t, &d), "pick on an empty network");
    CHECK(t == IP(192, 168, 1, 99) && d == IP(192, 168, 1, 98), ".99 and .98 as on a DreamPi");
    /* .99 taken: the next ones down */
    used_n = 1; used_set[0] = IP(192, 168, 1, 99);
    CHECK(dcl_pick(IP(192, 168, 1, 1), IP(192, 168, 1, 186), 0, used, NULL, &t, &d) && t == IP(192, 168, 1, 98) && d == IP(192, 168, 1, 97), ".99 used -> .98/.97");
    /* .98 taken: the Dreamcast goes below it */
    used_n = 1; used_set[0] = IP(192, 168, 1, 98);
    CHECK(dcl_pick(IP(192, 168, 1, 1), IP(192, 168, 1, 186), 0, used, NULL, &t, &d) && t == IP(192, 168, 1, 99) && d == IP(192, 168, 1, 97), ".98 used -> .99/.97");
    /* the box itself and the gateway are never picked */
    used_n = 0;
    CHECK(dcl_pick(IP(10, 0, 0, 99), IP(10, 0, 0, 98), 0, used, NULL, &t, &d) && t == IP(10, 0, 0, 97) && d == IP(10, 0, 0, 96), "skips the gateway and the box's own address");
    /* a phone hotspot (iPhone: 172.20.10.0/28, gateway .1): .98 does not exist there */
    used_n = 0;
    CHECK(dcl_pick(IP(172, 20, 10, 1), IP(172, 20, 10, 2), 0xfffffff0u, used, NULL, &t, &d) && t == IP(172, 20, 10, 14) && d == IP(172, 20, 10, 13), "a /28: the highest free addresses in it");
    /* an Android hotspot (/24): as DreamPi */
    CHECK(dcl_pick(IP(192, 168, 43, 1), IP(192, 168, 43, 120), 0xffffff00u, used, NULL, &t, &d) && t == IP(192, 168, 43, 99) && d == IP(192, 168, 43, 98), "Android hotspot /24: .99/.98");
    /* a /16: still the gateway's /24, as DreamPi */
    CHECK(dcl_pick(IP(10, 0, 0, 1), IP(10, 0, 5, 7), 0xffff0000u, used, NULL, &t, &d) && t == IP(10, 0, 0, 99), "a /16: the gateway's /24 like DreamPi");
    /* a /30 has no room for two more */
    CHECK(!dcl_pick(IP(10, 9, 9, 1), IP(10, 9, 9, 2), 0xfffffffcu, used, NULL, &t, &d), "a /30: nothing free");
    /* gateway's /24, like DreamPi's split('.')[:3] */
    CHECK(dcl_pick(IP(172, 16, 5, 254), IP(172, 16, 4, 10), 0, used, NULL, &t, &d) && t == IP(172, 16, 5, 99), "the gateway's /24");

    /* ---- the 1:1 rewrite keeps every checksum right ---- */
    uint8_t pk[1500];
    srand(3);
    int n_ok = 0;
    for (int it = 0; it < 3000; it++) {
        uint8_t proto = (uint8_t)(it % 3 == 0 ? 6 : it % 3 == 1 ? 17 : 1);
        size_t n = 40 + (size_t)(rand() % 1400);           /* at least a whole TCP header */
        uint32_t a = (uint32_t)rand() * 2654435761u, b = (uint32_t)rand() * 40503u + 7;
        fill(pk, n, proto, a, b);
        dcl_pkt_t p = { g, pu, pk, n };
        uint32_t neu = (uint32_t)rand() * 2246822519u;
        int which = it & 1 ? DCL_SRC : DCL_DST;
        CHECK(dcl_rewrite(&p, which, neu), "rewrite accepted");
        uint32_t got = (uint32_t)pk[which] << 24 | (uint32_t)pk[which + 1] << 16 | (uint32_t)pk[which + 2] << 8 | pk[which + 3];
        CHECK(got == neu, "address replaced");
        CHECK(((uint32_t)pk[10] << 8 | pk[11]) == ip_csum(pk), "IP checksum right after the rewrite");
        if (proto == 6) CHECK(((uint32_t)pk[36] << 8 | pk[37]) == l4_csum(pk, n, 16), "TCP checksum right");
        if (proto == 17) { uint16_t c = l4_csum(pk, n, 6); if (!c) c = 0xffff; CHECK(((uint32_t)pk[26] << 8 | pk[27]) == c, "UDP checksum right"); }
        n_ok++;
    }
    /* UDP without checksum stays without */
    fill(pk, 60, 17, IP(10, 64, 0, 2), IP(8, 8, 8, 8)); pk[26] = pk[27] = 0;
    { dcl_pkt_t p = { g, pu, pk, 60 }; dcl_rewrite(&p, DCL_SRC, IP(192, 168, 1, 98)); }
    CHECK(pk[26] == 0 && pk[27] == 0, "UDP checksum 0 stays 0");
    /* a later fragment: only the IP header changes */
    fill(pk, 100, 6, IP(10, 88, 0, 3), IP(1, 2, 3, 4)); pk[6] = 0; pk[7] = 10;
    { uint16_t c = ip_csum(pk); pk[10] = (uint8_t)(c >> 8); pk[11] = (uint8_t)c; }
    uint8_t before[100]; memcpy(before, pk, 100);
    { dcl_pkt_t p = { g, pu, pk, 100 }; dcl_rewrite(&p, DCL_SRC, IP(192, 168, 1, 98)); }
    CHECK(!memcmp(pk + 20, before + 20, 80) && ((uint32_t)pk[10] << 8 | pk[11]) == ip_csum(pk), "later fragment: payload untouched, IP checksum right");
    /* not IPv4 */
    pk[0] = 0x60; { dcl_pkt_t p = { g, pu, pk, 100 }; CHECK(!dcl_rewrite(&p, DCL_SRC, 1), "IPv6 untouched"); }

    /* ---- ARP ---- */
    uint8_t req[28] = { 0, 1, 8, 0, 6, 4, 0, 1, 0xaa, 0xbb, 0xcc, 1, 2, 3, 192, 168, 1, 1, 0, 0, 0, 0, 0, 0, 192, 168, 1, 98 };
    uint8_t mac[6]; uint32_t sip;
    CHECK(dcl_arp_asks_for(req, 28, IP(192, 168, 1, 98), mac, &sip) && sip == IP(192, 168, 1, 1) && mac[0] == 0xaa, "the router asks for .98");
    CHECK(!dcl_arp_asks_for(req, 28, IP(192, 168, 1, 97), mac, &sip), "a question for another address");
    uint8_t probe[28]; memcpy(probe, req, 28); probe[14] = 192; probe[15] = 168; probe[16] = 1; probe[17] = 98;
    CHECK(!dcl_arp_asks_for(probe, 28, IP(192, 168, 1, 98), mac, &sip), "a device announcing .98 for itself is not a question");
    uint8_t ours[6] = { 0x9a, 0xc3, 0x77, 0x61, 0xd9, 0x1a };
    CHECK(dcl_arp_claims(probe, 28, IP(192, 168, 1, 98), ours), "another device claiming .98 is noticed");
    memcpy(probe + 8, ours, 6);
    CHECK(!dcl_arp_claims(probe, 28, IP(192, 168, 1, 98), ours), "our own announcement is not a conflict");
    uint8_t rep[28];
    dcl_arp_reply(rep, ours, IP(192, 168, 1, 98), req + 8, IP(192, 168, 1, 1));
    CHECK(rep[7] == 2 && !memcmp(rep + 8, ours, 6) && rep[17] == 98 && !memcmp(rep + 18, req + 8, 6) && rep[27] == 1, "the answer: .98 is at our MAC, to the asker");

    if (fails) { printf("dclan: %d failures\n", fails); return 1; }
    printf("dclan: all ok (DreamPi's .99/.98 pick, %d rewrites with right checksums, ARP)\n", n_ok);
    return 0;
}
