/*
 * What DreamPi's iptables do to packets from the Dreamcast, as a function on one IPv4 packet
 * (host-compilable; used for PPP frames by hdlc_model.h and for Ethernet frames by the ESP32):
 *   - TTL: the packet leaves with TTL 64 (lwIP forwards it at 63; DreamPi's
 *     "-t mangle FORWARD -i ppp0 -j TTL --ttl-set 64" gives 64 on the wire, so we set 65 here)
 *   - PowerSmash: a TCP SYN from source port 3200-3205 is dropped if another SYN to the same
 *     destination address was seen less than a second ago (the game sends every SYN twice).
 *     Same as DreamPi's "recent" rule: a dropped SYN also renews the one-second timer.
 * and what DreamPi's packet helpers (Jonas Karlsson, MIT, NFQUEUE programs; read from their ARM
 * binaries 2026-11-13, see the project's 12-dreampi-spec.md) do:
 *   - dcgamespy (from the DC): a TCP segment whose flags are exactly SYN+PSH loses the PSH flag.
 *   - dcvoip (UDP 1285 -> 1285, Planet Ring voice): in a negotiation packet (UDP length 64 or 32)
 *     the IPv4 address at IP offset 70 or 42 becomes: from the DC, our public address (if known);
 *     to the DC, the packet's source address.
 *   - dc2k2 (TCP to the DC, destination port 5011, PSH+ACK): a message starting with byte 1 gets
 *     the packet's source address in its bytes 1-4.
 *   - dcdaytona (UDP 12079 -> 12079, Daytona USA; to the DC only): see ipf__daytona().
 * ipf_packet() handles packets from the Dreamcast, ipf_to_dc() packets going to it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define IPF_SYN_SLOTS  8
#define IPF_TTL_OUT    65

typedef struct {
    struct { uint32_t dst; uint32_t ms; bool used; } syn[IPF_SYN_SLOTS];
    uint32_t ip_packets, ttl_fixed, syn_dropped;
    uint32_t public_ip;                       /* our address on the Internet (dcvoip -p); 0 = unknown */
    uint32_t gamespy_fixed, voip_fixed, k2_fixed, daytona_fixed;
} ipf_t;

static inline uint16_t ipf__sum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static inline uint32_t ipf__get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void ipf__put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* IP header checksum, then the TCP/UDP checksum (pseudo header) over len bytes at l4 */
static inline void ipf__fix_sums(uint8_t *ip, size_t ihl, uint8_t *l4, size_t len, int csum_off) {
    ip[10] = ip[11] = 0;
    uint16_t c = ipf__sum(ip, ihl); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    l4[csum_off] = l4[csum_off + 1] = 0;
    uint32_t s = 0;
    for (int i = 12; i < 20; i += 2) s += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    s += ip[9]; s += (uint32_t)len;
    for (size_t i = 0; i + 1 < len; i += 2) s += (uint32_t)(l4[i] << 8 | l4[i + 1]);
    if (len & 1) s += (uint32_t)l4[len - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    uint16_t r = (uint16_t)~s;
    if (ip[9] == 17 && r == 0) r = 0xFFFF;
    l4[csum_off] = (uint8_t)(r >> 8); l4[csum_off + 1] = (uint8_t)r;
}

/* dcvoip: true if changed. from_dc: write public_ip, else the source address */
static inline bool ipf__voip(ipf_t *h, uint8_t *ip, size_t ihl, size_t n, bool from_dc) {
    if (ip[9] != 17 || n < ihl + 8) return false;
    uint8_t *u = ip + ihl;
    if ((u[0] << 8 | u[1]) != 1285 || (u[2] << 8 | u[3]) != 1285) return false;
    size_t ulen = (size_t)(u[4] << 8 | u[5]);
    size_t off = ulen - 4 == 60 ? 70 : ulen - 4 == 28 ? 42 : 0;   /* from the IP header start, as dcvoip */
    if (!off || off + 4 > n || ihl + ulen > n) return false;
    uint32_t a = from_dc ? h->public_ip : ipf__get32(ip + 12);
    if (!a) return false;
    ipf__put32(ip + off, a);
    ipf__fix_sums(ip, ihl, u, ulen, 6);
    h->voip_fixed++;
    return true;
}

/* the one-second "recent" list; true = drop this SYN */
static inline bool ipf__syn_drop(ipf_t *h, uint32_t dst, uint32_t now_ms) {
    int free_i = -1, old_i = 0;
    for (int i = 0; i < IPF_SYN_SLOTS; i++) {
        if (h->syn[i].used && h->syn[i].dst == dst) {
            bool recent = (uint32_t)(now_ms - h->syn[i].ms) < 1000;
            h->syn[i].ms = now_ms;               /* --update and --set both refresh the time */
            return recent;
        }
        if (!h->syn[i].used) free_i = i;
        else if ((uint32_t)(now_ms - h->syn[i].ms) > (uint32_t)(now_ms - h->syn[old_i].ms)) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    h->syn[i].used = true; h->syn[i].dst = dst; h->syn[i].ms = now_ms;
    return false;
}

/* ip points at an IPv4 header, n bytes available. Changes TTL and checksum in place.
   Returns false if the packet must be dropped. Anything that is not IPv4 passes. */
static inline bool ipf_packet(ipf_t *h, uint8_t *ip, size_t n, uint32_t now_ms) {
    if (n < 20 || (ip[0] >> 4) != 4) return true;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if (ihl < 20 || ihl > n) return true;
    h->ip_packets++;
    if (ip[9] == 6 && ((ip[6] & 0x1F) | ip[7]) == 0 && n >= ihl + 14) {
        const uint8_t *t = ip + ihl;
        uint16_t sport = (uint16_t)(t[0] << 8 | t[1]);
        uint8_t fl = t[13];
        if (sport >= 3200 && sport <= 3205 && (fl & 0x17) == 0x02) {          /* SYN, no ACK/RST/FIN */
            uint32_t dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
            if (ipf__syn_drop(h, dst, now_ms)) { h->syn_dropped++; return false; }
        }
        size_t tot = (size_t)(ip[2] << 8 | ip[3]);
        if ((fl & 0x3F) == 0x0A && tot >= ihl + 20 && tot <= n) {            /* dcgamespy: SYN+PSH exactly */
            ip[ihl + 13] = (uint8_t)(fl & ~0x08);
            ipf__fix_sums(ip, ihl, ip + ihl, tot - ihl, 16);
            h->gamespy_fixed++;
        }
    }
    if (((ip[6] & 0x1F) | ip[7]) == 0) ipf__voip(h, ip, ihl, n, true);
    if (ip[8] != IPF_TTL_OUT) {
        ip[8] = IPF_TTL_OUT;
        ip[10] = ip[11] = 0;
        uint16_t c = ipf__sum(ip, ihl);
        ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
        h->ttl_fixed++;
    }
    return true;
}

/* An Ethernet frame (no FCS). EtherType 0x0800, also behind one 802.1Q tag. false = drop. */
static inline bool ipf_ether(ipf_t *h, uint8_t *f, size_t n, uint32_t now_ms) {
    if (n < 14 + 20) return true;
    size_t o = 12;
    uint16_t type = (uint16_t)(f[o] << 8 | f[o + 1]);
    if (type == 0x8100 && n >= 18 + 20) { o += 4; type = (uint16_t)(f[o] << 8 | f[o + 1]); }
    if (type != 0x0800) return true;
    return ipf_packet(h, f + o + 2, n - (o + 2), now_ms);
}

/* dcdaytona, packets to the Dreamcast (after NAT: destination = the Dreamcast's own address).
   Offsets from the start of a 20-byte IP header, L = UDP length - 4, as the helper does:
   A) L in {27,29,36,38,45,47}, byte 28 = 0x20, byte 31 = 0x25, n = byte 32 in 2..4:
      bytes 33-36 = the sender's address; then the first of these that holds our public address
      gets the Dreamcast's address: offset 42 (n == 2: L > 26; n > 2: L > 35), else 51 (n > 2),
      else 60 (n == 4, L > 44).
   B) 48 < L <= 53, bytes 28-31 = 20 2B 42 21: bytes 65-68 = the sender's address. */
static inline bool ipf__daytona(ipf_t *h, uint8_t *ip, size_t ihl, size_t n) {
    if (ip[9] != 17 || ihl != 20 || n < 28) return false;
    uint8_t *u = ip + 20;
    if ((u[0] << 8 | u[1]) != 12079 || (u[2] << 8 | u[3]) != 12079) return false;
    size_t ulen = (size_t)(u[4] << 8 | u[5]);
    if (ulen < 8 || 20 + ulen > n) return false;
    int L = (int)ulen - 4;
    uint32_t src = ipf__get32(ip + 12), dst = ipf__get32(ip + 16), pub = h->public_ip;
    bool changed = false;
#define IPF_FITS(off) ((size_t)(off) + 4 <= 20 + ulen)
    if ((L == 27 || L == 29 || L == 36 || L == 38 || L == 45 || L == 47) && IPF_FITS(33)
        && ip[28] == 0x20 && ip[31] == 0x25 && ip[32] >= 2 && ip[32] <= 4) {
        uint8_t k = ip[32];
        ipf__put32(ip + 33, src); changed = true;
        if (k == 2) {
            if (L > 26 && IPF_FITS(42) && ipf__get32(ip + 42) == pub) ipf__put32(ip + 42, dst);
        } else if (L > 35) {
            if (IPF_FITS(42) && ipf__get32(ip + 42) == pub) ipf__put32(ip + 42, dst);
            else if (IPF_FITS(51) && ipf__get32(ip + 51) == pub) ipf__put32(ip + 51, dst);
            else if (k == 4 && L > 44 && IPF_FITS(60) && ipf__get32(ip + 60) == pub) ipf__put32(ip + 60, dst);
        }
    }
    if (L > 48 && L <= 53 && IPF_FITS(65) && ip[28] == 0x20 && ip[29] == 0x2B && ip[30] == 0x42 && ip[31] == 0x21) {
        ipf__put32(ip + 65, src); changed = true;
    }
#undef IPF_FITS
    if (!changed) return false;
    ipf__fix_sums(ip, 20, u, ulen, 6);
    h->daytona_fixed++;
    return true;
}

/* A packet going to the Dreamcast (after NAT: the source is still the remote's address). */
static inline void ipf_to_dc(ipf_t *h, uint8_t *ip, size_t n) {
    if (n < 20 || (ip[0] >> 4) != 4) return;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if (ihl < 20 || ihl > n || ((ip[6] & 0x1F) | ip[7]) != 0) return;
    size_t tot = (size_t)(ip[2] << 8 | ip[3]);
    if (tot > n || tot < ihl) return;
    if (ipf__voip(h, ip, ihl, tot, false)) return;
    if (ipf__daytona(h, ip, ihl, tot)) return;
    if (ip[9] != 6 || tot < ihl + 20) return;
    uint8_t *t = ip + ihl;
    if ((t[2] << 8 | t[3]) != 5011) return;                                 /* dc2k2: dport 5011 */
    uint8_t fl = t[13];
    if (!(fl & 0x08) || !(fl & 0x10)) return;                               /* PSH and ACK */
    size_t thl = (size_t)(t[12] >> 4) * 4, seg = tot - ihl, pay = ihl + thl;
    if (thl < 20 || pay + 5 > tot) return;
    if (!(seg > pay + 5)) return;                                           /* the helper's own length test */
    if (ip[pay] != 1) return;
    ipf__put32(ip + pay + 1, ipf__get32(ip + 12));
    ipf__fix_sums(ip, ihl, t, seg, 16);
    h->k2_fixed++;
}

/* to-DC Ethernet frame */
static inline void ipf_ether_to_dc(ipf_t *h, uint8_t *f, size_t n) {
    if (n < 14 + 20) return;
    size_t o = 12;
    uint16_t type = (uint16_t)(f[o] << 8 | f[o + 1]);
    if (type == 0x8100 && n >= 18 + 20) { o += 4; type = (uint16_t)(f[o] << 8 | f[o + 1]); }
    if (type == 0x0800) ipf_to_dc(h, f + o + 2, n - (o + 2));
}
