/*
 * The Dreamcast's own address on the home network, as on a DreamPi (dreampi.py).
 *
 * DreamPi: autoconfigure_ppp() takes the gateway's /24 and counts down from .100 for the first
 * free address (find_next_unused_ip): that is the Pi's end of the PPP link (usually .99), the
 * next free one below is the Dreamcast's (usually .98). pppd "proxyarp" answers ARP for the
 * Dreamcast's address, so the router sees it as a device of its own; no NAT on the Pi. With the
 * VPN up the Dreamcast gets tun+1 over PPP and iptables maps it 1:1 to that address (SNAT for
 * what does not go into the tunnel, DNAT for what comes in for it). UPnP points at it.
 *
 * Here: the same pick, the same 1:1 rewrite (stateless, so it is what SNAT/DNAT of one address
 * does), and the ARP answer. Host-compilable; esp32/main/dclan.c is the glue to lwIP.
 * Addresses are host order.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---- the pick (find_next_unused_ip, twice) ---- */
typedef bool (*dcl_used_fn)(void *ctx, uint32_t ip);

/* gw: the uplink's gateway, own: the box's uplink address. false when nothing is free. */
static inline bool dcl_pick(uint32_t gw, uint32_t own, dcl_used_fn used, void *ctx, uint32_t *this_ip, uint32_t *dc_ip) {
    uint32_t net = gw & 0xffffff00u;
    uint32_t found[2]; int k = 0;
    for (uint32_t h = 99; h >= 1 && k < 2; h--) {
        uint32_t a = net | h;
        if (a == gw || a == own) continue;
        if (used && used(ctx, a)) continue;
        found[k++] = a;
    }
    if (k < 2) return false;
    *this_ip = found[0];
    *dc_ip = found[1];
    return true;
}

/* ---- checksums (RFC 1624: HC' = ~(~HC + ~m + m')) ---- */
static inline uint16_t dcl_csum_fix(uint16_t c, uint32_t old, uint32_t neu) {
    uint32_t s = (uint16_t)~c;
    s += (uint16_t)~(old >> 16); s += (uint16_t)~(old & 0xffff);
    s += neu >> 16; s += neu & 0xffff;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* a packet seen through accessors (a flat buffer in the tests, an lwIP pbuf chain on the box) */
typedef struct {
    uint8_t (*get)(void *ctx, size_t off);
    void (*put)(void *ctx, size_t off, uint8_t v);
    void *ctx;
    size_t len;
} dcl_pkt_t;

static inline uint32_t dcl_rd16(const dcl_pkt_t *p, size_t o) { return (uint32_t)p->get(p->ctx, o) << 8 | p->get(p->ctx, o + 1); }
static inline uint32_t dcl_rd32(const dcl_pkt_t *p, size_t o) { return dcl_rd16(p, o) << 16 | dcl_rd16(p, o + 2); }
static inline void dcl_wr16(const dcl_pkt_t *p, size_t o, uint32_t v) { p->put(p->ctx, o, (uint8_t)(v >> 8)); p->put(p->ctx, o + 1, (uint8_t)v); }

enum { DCL_SRC = 12, DCL_DST = 16 };

/* Replace the source (DCL_SRC) or destination (DCL_DST) address, fixing the IP checksum and the
   TCP/UDP checksum (pseudo header). Returns false (untouched) for anything that is not IPv4. */
static inline bool dcl_rewrite(const dcl_pkt_t *p, int which, uint32_t neu) {
    if (p->len < 20 || (p->get(p->ctx, 0) >> 4) != 4) return false;
    size_t hl = (size_t)(p->get(p->ctx, 0) & 15) * 4;
    if (hl < 20 || p->len < hl) return false;
    uint32_t old = dcl_rd32(p, (size_t)which);
    if (old == neu) return true;
    dcl_wr16(p, (size_t)which, neu >> 16);
    dcl_wr16(p, (size_t)which + 2, neu & 0xffff);
    dcl_wr16(p, 10, dcl_csum_fix((uint16_t)dcl_rd16(p, 10), old, neu));
    bool first_frag = ((dcl_rd16(p, 6) & 0x1fff) == 0);
    if (!first_frag) return true;
    uint8_t proto = p->get(p->ctx, 9);
    if (proto == 6 && p->len >= hl + 18) {
        dcl_wr16(p, hl + 16, dcl_csum_fix((uint16_t)dcl_rd16(p, hl + 16), old, neu));
    } else if (proto == 17 && p->len >= hl + 8) {
        uint16_t c = (uint16_t)dcl_rd16(p, hl + 6);
        if (c) {                                   /* 0 = no checksum: stays 0 */
            c = dcl_csum_fix(c, old, neu);
            dcl_wr16(p, hl + 6, c ? c : 0xffff);
        }
    }
    return true;
}

/* ---- ARP (the 28-byte ARP payload after the Ethernet header) ---- */
/* an ARP request asking for `target`? fills the asker's MAC and address */
static inline bool dcl_arp_asks_for(const uint8_t *a, size_t n, uint32_t target, uint8_t asker_mac[6], uint32_t *asker_ip) {
    if (n < 28) return false;
    if (a[0] != 0 || a[1] != 1 || a[2] != 8 || a[3] != 0 || a[4] != 6 || a[5] != 4) return false;   /* Ethernet/IPv4 */
    if (a[6] != 0 || a[7] != 1) return false;                                                           /* request */
    uint32_t tip = (uint32_t)a[24] << 24 | (uint32_t)a[25] << 16 | (uint32_t)a[26] << 8 | a[27];
    if (tip != target) return false;
    uint32_t sip = (uint32_t)a[14] << 24 | (uint32_t)a[15] << 16 | (uint32_t)a[16] << 8 | a[17];
    if (sip == target) return false;              /* someone probing for itself: not a question for us */
    memcpy(asker_mac, a + 8, 6);
    *asker_ip = sip;
    return true;
}

/* another device says it has `ip` (any ARP packet with that sender address and another MAC) */
static inline bool dcl_arp_claims(const uint8_t *a, size_t n, uint32_t ip, const uint8_t our_mac[6]) {
    if (n < 28 || a[2] != 8 || a[3] != 0) return false;
    uint32_t sip = (uint32_t)a[14] << 24 | (uint32_t)a[15] << 16 | (uint32_t)a[16] << 8 | a[17];
    return sip == ip && memcmp(a + 8, our_mac, 6) != 0;
}

/* the answer: `ip` is at our_mac */
static inline void dcl_arp_reply(uint8_t out[28], const uint8_t our_mac[6], uint32_t ip, const uint8_t to_mac[6], uint32_t to_ip) {
    static const uint8_t hdr[6] = { 0, 1, 8, 0, 6, 4 };
    memcpy(out, hdr, 6);
    out[6] = 0; out[7] = 2;
    memcpy(out + 8, our_mac, 6);
    out[14] = (uint8_t)(ip >> 24); out[15] = (uint8_t)(ip >> 16); out[16] = (uint8_t)(ip >> 8); out[17] = (uint8_t)ip;
    memcpy(out + 18, to_mac, 6);
    out[24] = (uint8_t)(to_ip >> 24); out[25] = (uint8_t)(to_ip >> 16); out[26] = (uint8_t)(to_ip >> 8); out[27] = (uint8_t)to_ip;
}
