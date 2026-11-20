/* One short line per Ethernet frame, for the first frames of a Dreamcast session in the log
   (esp32/main/dc_eth.c): enough to see where a game stops (no DNS question, an ARP that gets no
   answer, a SYN without SYN-ACK). Host-testable: firmware/tests/test_ethtrace.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

static inline unsigned ett_be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline int ett_ip(char *o, size_t cap, const uint8_t *p) {
    return snprintf(o, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

static size_t ethtrace_line(const uint8_t *f, size_t n, char *o, size_t cap) {
    if (!cap) return 0;
    o[0] = 0;
    if (n < 14) return (size_t)snprintf(o, cap, "short frame (%u bytes)", (unsigned)n);
    unsigned et = ett_be16(f + 12);
    const uint8_t *p = f + 14; size_t m = n - 14;
    char a[16], b[16];
    if (et == 0x0806 && m >= 28) {
        unsigned op = ett_be16(p + 6);
        ett_ip(a, sizeof a, p + 24); ett_ip(b, sizeof b, p + 14);
        if (op == 1) return (size_t)snprintf(o, cap, "ARP who has %s? (from %s)", a, b);
        if (op == 2) return (size_t)snprintf(o, cap, "ARP %s is at %02x:%02x:%02x:%02x:%02x:%02x", b,
                                             p[8], p[9], p[10], p[11], p[12], p[13]);
        return (size_t)snprintf(o, cap, "ARP op %u", op);
    }
    if (et != 0x0800) return (size_t)snprintf(o, cap, "type %04x, %u bytes", et, (unsigned)n);
    if (m < 20 || (p[0] >> 4) != 4) return (size_t)snprintf(o, cap, "bad IPv4 header");
    size_t hl = (size_t)(p[0] & 15) * 4;
    unsigned tot = ett_be16(p + 2), pr = p[9];
    ett_ip(a, sizeof a, p + 12); ett_ip(b, sizeof b, p + 16);
    if (hl < 20 || m < hl) return (size_t)snprintf(o, cap, "IP %s > %s, bad header", a, b);
    const uint8_t *t = p + hl; size_t tl = m - hl;
    if (pr == 17 && tl >= 8) {
        unsigned sp = ett_be16(t), dp = ett_be16(t + 2);
        const char *what = "";
        char dh[32] = "";
        if (sp == 53 || dp == 53) {
            what = sp == 53 ? " DNS answer" : " DNS question";
            if (tl >= 20) {                                      /* header after UDP: id, flags, counts */
                unsigned rc = t[8 + 3] & 15, an = ett_be16(t + 8 + 6);
                if (sp == 53) snprintf(dh, sizeof dh, " (rcode %u, %u answers)", rc, an);
            }
        } else if ((sp == 67 || sp == 68) && (dp == 67 || dp == 68) && tl >= 8 + 240) {
            static const char *const dn[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM" };
            const uint8_t *opt = t + 8 + 240; size_t ol = tl - 8 - 240; unsigned ty = 0;
            for (size_t i = 0; i + 1 < ol && opt[i] != 255;) {
                if (opt[i] == 0) { i++; continue; }
                if (opt[i] == 53 && opt[i + 1] >= 1 && i + 2 < ol) { ty = opt[i + 2]; break; }
                i += 2u + opt[i + 1];
            }
            return (size_t)snprintf(o, cap, "DHCP %s %s > %s", ty < 9 ? dn[ty] : "?", a, b);
        }
        return (size_t)snprintf(o, cap, "UDP %s:%u > %s:%u%s%s, %u bytes", a, sp, b, dp, what, dh, tot);
    }
    if (pr == 6 && tl >= 14) {
        unsigned sp = ett_be16(t), dp = ett_be16(t + 2), fl = t[13];
        char fs[16]; size_t k = 0;
        if (fl & 0x02) k += (size_t)snprintf(fs + k, sizeof fs - k, "SYN ");
        if (fl & 0x10) k += (size_t)snprintf(fs + k, sizeof fs - k, "ACK ");
        if (fl & 0x01) k += (size_t)snprintf(fs + k, sizeof fs - k, "FIN ");
        if (fl & 0x04) k += (size_t)snprintf(fs + k, sizeof fs - k, "RST ");
        if (k) fs[k - 1] = 0; else snprintf(fs, sizeof fs, "-");
        return (size_t)snprintf(o, cap, "TCP %s:%u > %s:%u %s, %u bytes", a, sp, b, dp, fs, tot);
    }
    if (pr == 1 && tl >= 2) return (size_t)snprintf(o, cap, "ICMP %s > %s type %u code %u", a, b, t[0], t[1]);
    return (size_t)snprintf(o, cap, "IP %s > %s proto %u, %u bytes", a, b, pr, tot);
}
