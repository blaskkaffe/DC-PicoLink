/*
 * DCNet (flyinghead's network for Flycast and real Dreamcasts), as DreamPi does it:
 *   - netlink.py: "*69" dialled just before the number asks for DCNet (or a setting: every call)
 *   - dcnet.rpi: find the closest access point (UDP 7655 at dcnet.flyca.st), then pass the
 *     modem bytes unchanged over TCP port 7654 (7656 for the Power Smash / VOOT numbers);
 *     the access point runs PPP itself. On the way to the Dreamcast a lone PPP flag '~' is
 *     doubled.
 * The ESP32 side is esp32/main/dcnet.c. Host-compilable (tests/test_dcnet.c).
 */
#ifndef DCNET_MODEL_H
#define DCNET_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define DCNET_HOST      "dcnet.flyca.st"
#define DCNET_PORT      7654
#define DCNET_PORT_ALT  7656
#define DCNET_DISCO     7655
#define DCNET_MAGIC     0xDC15C001u
#define DCNET_MAX_AP    8

enum { DCNET_OFF = 0, DCNET_PREFIX = 1, DCNET_ALWAYS = 2 };

/* keep only what a modem dials: digits, '*', '#' (drops T, P, W, ',', spaces, '-') */
static inline size_t dcnet_digits(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 1 < cap; in++)
        if ((*in >= '0' && *in <= '9') || *in == '*' || *in == '#') out[o++] = *in;
    if (cap) out[o] = 0;
    return o;
}

/* Does this call go to DCNet? `dial` = the digits; `prefix_recent` = "*69" was heard on its own
   in the last 10 s (DreamPi's dial modifier). `number` gets the dial string without "*69". */
static inline bool dcnet_wanted(int mode, const char *dial, bool prefix_recent, char *number, size_t cap) {
    bool pre = !strncmp(dial, "*69", 3);
    const char *n = pre ? dial + 3 : dial;
    if (cap) { strncpy(number, n, cap - 1); number[cap - 1] = 0; }
    if (mode == DCNET_OFF) return false;
    if (mode == DCNET_ALWAYS) return true;
    return pre || prefix_recent;
}

/* netlink.py: ^0053600100(0[1-9]|10|13|14|15|16)$ -> 7656 */
static inline uint16_t dcnet_port_for(const char *number) {
    if (strlen(number) != 12 || strncmp(number, "0053600100", 10)) return DCNET_PORT;
    int v = (number[10] - '0') * 10 + (number[11] - '0');
    if (number[10] < '0' || number[10] > '9' || number[11] < '0' || number[11] > '9') return DCNET_PORT;
    if ((v >= 1 && v <= 10) || (v >= 13 && v <= 16)) return DCNET_PORT_ALT;
    return DCNET_PORT;
}

/* the discovery question: magic (little-endian) + 3 */
static inline size_t dcnet_disco_query(uint8_t *b) {
    uint32_t m = DCNET_MAGIC;
    b[0] = (uint8_t)m; b[1] = (uint8_t)(m >> 8); b[2] = (uint8_t)(m >> 16); b[3] = (uint8_t)(m >> 24);
    b[4] = 3;
    return 5;
}

/* a ping: magic + 1 + 8 bytes the access point sends back */
static inline size_t dcnet_ping(uint8_t *b, uint64_t t) {
    dcnet_disco_query(b);
    b[4] = 1;
    memcpy(b + 5, &t, 8);
    return 13;
}

static inline bool dcnet_magic_ok(const uint8_t *b, size_t n) {
    return n >= 5 && b[0] == 0x01 && b[1] == 0xC0 && b[2] == 0x15 && b[3] == 0xDC;
}

/* the discovery answer: [ip 4][name length 1][name] ...; returns how many (ip in network order) */
static inline int dcnet_disco_answer(const uint8_t *b, size_t n, uint32_t *ip, char (*name)[32], int max) {
    if (!dcnet_magic_ok(b, n) || b[4] != 3) return 0;
    size_t p = 5; int k = 0;
    while (p + 5 <= n && k < max) {
        memcpy(&ip[k], b + p, 4); p += 4;
        size_t l = b[p++];
        if (p + l > n) break;
        size_t c = l < 31 ? l : 31;
        memcpy(name[k], b + p, c); name[k][c] = 0;
        p += l; k++;
    }
    return k;
}

/* To the Dreamcast: a '~' followed by anything but '~' becomes "~~" (dcnet.c's inbuffixed).
   `seen` carries a trailing '~' over to the next call. out needs 2*n+1. */
static inline size_t dcnet_fix_to_dc(bool *seen, const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (*seen) { *seen = false; if (c != '~') out[o++] = '~'; out[o++] = c; continue; }
        if (c == '~') *seen = true;
        out[o++] = c;
    }
    return o;
}
/* flush: a '~' at the very end of the stream stays single (dcnet.c also waits for the next byte) */
#endif
