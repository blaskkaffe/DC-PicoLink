/*
 * Our public Internet address, found as DreamPi finds it for its packet helpers (dcvoip,
 * dcdaytona: `dig +short myip.opendns.com @resolver1.opendns.com`): a DNS query for the A record
 * of myip.opendns.com, sent to OpenDNS's resolver (208.67.222.222), which answers with the
 * address the query came from. Host-compilable: builds the query and reads the answer.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PUBIP_SERVER 0xD043DEDEu       /* resolver1.opendns.com = 208.67.222.222 */
#define PUBIP_NAME   "myip.opendns.com"

/* the query; returns its length (out needs 64 bytes) */
static inline size_t pubip_query(uint8_t *out, uint16_t id) {
    size_t o = 0;
    out[o++] = (uint8_t)(id >> 8); out[o++] = (uint8_t)id;
    out[o++] = 0x01; out[o++] = 0x00;            /* standard query, recursion desired */
    out[o++] = 0; out[o++] = 1;                  /* 1 question */
    memset(out + o, 0, 6); o += 6;
    const char *s = PUBIP_NAME;
    while (*s) {
        const char *d = strchr(s, '.');
        size_t l = d ? (size_t)(d - s) : strlen(s);
        out[o++] = (uint8_t)l; memcpy(out + o, s, l); o += l;
        s += l + (d ? 1 : 0);
    }
    out[o++] = 0;
    out[o++] = 0; out[o++] = 1;                  /* type A */
    out[o++] = 0; out[o++] = 1;                  /* class IN */
    return o;
}

/* skip a (possibly compressed) name at p; returns the new offset or 0 */
static inline size_t pubip__skip(const uint8_t *m, size_t n, size_t p) {
    while (p < n) {
        uint8_t l = m[p];
        if (l == 0) return p + 1;
        if ((l & 0xC0) == 0xC0) return p + 2 <= n ? p + 2 : 0;
        if (l & 0xC0) return 0;
        p += 1u + l;
    }
    return 0;
}

/* the first A record of an answer to our query id; 0 = none / not ours / an error */
static inline uint32_t pubip_answer(const uint8_t *m, size_t n, uint16_t id) {
    if (n < 12 || (uint16_t)(m[0] << 8 | m[1]) != id || !(m[2] & 0x80) || (m[3] & 0x0F)) return 0;
    unsigned qd = (unsigned)(m[4] << 8 | m[5]), an = (unsigned)(m[6] << 8 | m[7]);
    size_t p = 12;
    for (unsigned i = 0; i < qd; i++) { p = pubip__skip(m, n, p); if (!p || p + 4 > n) return 0; p += 4; }
    for (unsigned i = 0; i < an; i++) {
        p = pubip__skip(m, n, p);
        if (!p || p + 10 > n) return 0;
        uint16_t type = (uint16_t)(m[p] << 8 | m[p + 1]), cls = (uint16_t)(m[p + 2] << 8 | m[p + 3]);
        uint16_t rdl = (uint16_t)(m[p + 8] << 8 | m[p + 9]);
        p += 10;
        if (p + rdl > n) return 0;
        if (type == 1 && cls == 1 && rdl == 4) {
            uint32_t a = (uint32_t)m[p] << 24 | (uint32_t)m[p + 1] << 16 | (uint32_t)m[p + 2] << 8 | m[p + 3];
            /* a private or zero answer is not a public address */
            if (a == 0 || (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 22) == 0x191) return 0;
            return a;
        }
        p += rdl;
    }
    return 0;
}
