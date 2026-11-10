/*
 * What DreamPi's iptables do to packets from the Dreamcast, as a function on one IPv4 packet
 * (host-compilable; used for PPP frames by hdlc_model.h and for Ethernet frames by the ESP32):
 *   - TTL: the packet leaves with TTL 64 (lwIP forwards it at 63; DreamPi's
 *     "-t mangle FORWARD -i ppp0 -j TTL --ttl-set 64" gives 64 on the wire, so we set 65 here)
 *   - PowerSmash: a TCP SYN from source port 3200-3205 is dropped if another SYN to the same
 *     destination address was seen less than a second ago (the game sends every SYN twice).
 *     Same as DreamPi's "recent" rule: a dropped SYN also renews the one-second timer.
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
} ipf_t;

static inline uint16_t ipf__sum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
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
    }
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
