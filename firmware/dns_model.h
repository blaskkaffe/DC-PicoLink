/*
 * DNS helper for the ESP32's small DNS server (host-compilable).
 *
 * Replaces DreamPi's dnsmasq: forwards to the Dreamcast revival servers, answers the
 * "address=/name/ip" overrides from the downloaded dreampi_dns.conf itself, refuses plain
 * names (domain-needed) and reverse lookups of private ranges (bogus-priv).
 * Also remembers the last A-queries, which DCNow! reports (as DreamPi reads dnsmasq's log).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define DNS_NAME_MAX   128
#define DNS_OVR_MAX    96
#define DNS_SRV_MAX    24
#define DNS_UP_MAX     6

/* revival servers built into DreamPi's dnsmasq.conf */
#define DNS_REVIVAL_1  "46.101.91.123"
#define DNS_REVIVAL_2  "209.50.50.129"

typedef struct {
    uint16_t id, flags;
    char name[DNS_NAME_MAX];       /* lower case, no trailing dot */
    uint16_t qtype, qclass;
    size_t qend;                   /* where the question ends in the packet */
    size_t qstart;                 /* where the question starts (12) */
} dns_q_t;

enum { DNS_A = 1, DNS_PTR = 12, DNS_AAAA = 28 };
enum { DNS_NOERROR = 0, DNS_FORMERR = 1, DNS_SERVFAIL = 2, DNS_NXDOMAIN = 3, DNS_REFUSED = 5 };

static inline bool dns_parse(const uint8_t *p, size_t n, dns_q_t *q) {
    if (n < 12 + 5) return false;
    q->id = (uint16_t)(p[0] << 8 | p[1]);
    q->flags = (uint16_t)(p[2] << 8 | p[3]);
    if (q->flags & 0x8000) return false;                       /* a response */
    if (((q->flags >> 11) & 15) != 0) return false;            /* only standard queries */
    if ((p[4] << 8 | p[5]) != 1) return false;
    size_t i = 12, o = 0;
    for (;;) {
        if (i >= n) return false;
        uint8_t l = p[i++];
        if (l == 0) break;
        if (l & 0xC0) return false;                            /* no compression in a question */
        if (i + l > n || o + l + 2 > DNS_NAME_MAX) return false;
        if (o) q->name[o++] = '.';
        for (uint8_t k = 0; k < l; k++) {
            char c = (char)p[i++];
            q->name[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    q->name[o] = 0;
    if (i + 4 > n) return false;
    q->qtype = (uint16_t)(p[i] << 8 | p[i + 1]);
    q->qclass = (uint16_t)(p[i + 2] << 8 | p[i + 3]);
    q->qend = i + 4;
    q->qstart = 12;
    return true;
}

/* answer with an rcode and no records (also "name exists, but no such type") */
static inline size_t dns_reply_rcode(const uint8_t *req, const dns_q_t *q, int rcode, uint8_t *out, size_t cap) {
    if (cap < q->qend) return 0;
    memcpy(out, req, q->qend);
    out[2] = (uint8_t)(0x80 | (req[2] & 0x01));                /* QR, keep RD */
    out[3] = (uint8_t)(0x80 | (rcode & 15));                   /* RA */
    out[4] = 0; out[5] = 1; out[6] = out[7] = out[8] = out[9] = out[10] = out[11] = 0;
    return q->qend;
}

/* answer one A record; ip is in host order */
static inline size_t dns_reply_a(const uint8_t *req, const dns_q_t *q, uint32_t ip, uint32_t ttl, uint8_t *out, size_t cap) {
    if (cap < q->qend + 16) return 0;
    size_t o = dns_reply_rcode(req, q, DNS_NOERROR, out, cap);
    out[7] = 1;
    out[o++] = 0xC0; out[o++] = 12;
    out[o++] = 0; out[o++] = DNS_A; out[o++] = 0; out[o++] = 1;
    out[o++] = (uint8_t)(ttl >> 24); out[o++] = (uint8_t)(ttl >> 16); out[o++] = (uint8_t)(ttl >> 8); out[o++] = (uint8_t)ttl;
    out[o++] = 0; out[o++] = 4;
    out[o++] = (uint8_t)(ip >> 24); out[o++] = (uint8_t)(ip >> 16); out[o++] = (uint8_t)(ip >> 8); out[o++] = (uint8_t)ip;
    return o;
}

/* "a.b.c.d" -> host-order ip; false if not an address */
static inline bool dns_ip(const char *s, size_t n, uint32_t *ip) {
    uint32_t v = 0; int parts = 0; size_t i = 0;
    while (i < n) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint32_t x = 0; int d = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { x = x * 10 + (uint32_t)(s[i++] - '0'); if (++d > 3) return false; }
        if (x > 255) return false;
        v = v << 8 | x; parts++;
        if (parts < 4) { if (i >= n || s[i] != '.') return false; i++; }
    }
    if (parts != 4) return false;
    *ip = v;
    return true;
}

/* ---- configuration: the built-in servers plus lines in dnsmasq syntax ---- */
typedef struct {
    struct { char name[DNS_NAME_MAX]; uint32_t ip; } ovr[DNS_OVR_MAX];       /* address=/name/ip */
    int n_ovr;
    struct { char name[DNS_NAME_MAX]; uint32_t ip; } dom[DNS_SRV_MAX];       /* server=/name/ip */
    int n_dom;
    uint32_t up[DNS_UP_MAX];                                                 /* server=ip */
    int n_up;
} dns_conf_t;

static inline void dns_conf_defaults(dns_conf_t *c) {
    memset(c, 0, sizeof *c);
    dns_ip(DNS_REVIVAL_1, strlen(DNS_REVIVAL_1), &c->up[0]);
    dns_ip(DNS_REVIVAL_2, strlen(DNS_REVIVAL_2), &c->up[1]);
    c->n_up = 2;
}

/* one line of dnsmasq syntax; ignores everything it does not know. true if it took the line. */
static inline bool dns_conf_line(dns_conf_t *c, const char *l, size_t n) {
    while (n && (l[n - 1] == '\r' || l[n - 1] == '\n' || l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
    while (n && (*l == ' ' || *l == '\t')) { l++; n--; }
    if (!n || l[0] == '#') return false;
    bool addr = n > 9 && !memcmp(l, "address=/", 9);
    bool srv = n > 7 && !memcmp(l, "server=", 7);
    if (addr || srv) {
        const char *v = l + (addr ? 8 : 7);
        size_t vn = n - (size_t)(v - l);
        char name[DNS_NAME_MAX]; size_t nl = 0;
        if (vn && v[0] == '/') {                                 /* /name/ip */
            const char *e = memchr(v + 1, '/', vn - 1);
            if (!e) return false;
            nl = (size_t)(e - v - 1);
            if (nl >= DNS_NAME_MAX) return false;
            for (size_t i = 0; i < nl; i++) { char ch = v[1 + i]; name[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
            name[nl] = 0;
            vn -= (size_t)(e + 1 - v); v = e + 1;
        } else if (addr) return false;
        const char *at = memchr(v, '#', vn);                     /* server=1.2.3.4#53 */
        if (at) vn = (size_t)(at - v);
        uint32_t ip;
        if (!dns_ip(v, vn, &ip)) return false;
        if (addr) {
            if (!nl || c->n_ovr >= DNS_OVR_MAX) return false;
            memcpy(c->ovr[c->n_ovr].name, name, nl + 1); c->ovr[c->n_ovr++].ip = ip;
        } else if (nl) {
            if (c->n_dom >= DNS_SRV_MAX) return false;
            memcpy(c->dom[c->n_dom].name, name, nl + 1); c->dom[c->n_dom++].ip = ip;
        } else {
            if (c->n_up >= DNS_UP_MAX) return false;
            c->up[c->n_up++] = ip;
        }
        return true;
    }
    return false;
}

static inline int dns_conf_text(dns_conf_t *c, const char *t, size_t n) {
    int took = 0;
    size_t s = 0;
    for (size_t i = 0; i <= n; i++)
        if (i == n || t[i] == '\n') { if (i > s && dns_conf_line(c, t + s, i - s)) took++; s = i + 1; }
    return took;
}

/* does `name` equal `dom` or end in ".dom"? */
static inline bool dns_in_domain(const char *name, const char *dom) {
    size_t a = strlen(name), b = strlen(dom);
    if (a < b) return false;
    if (memcmp(name + a - b, dom, b)) return false;
    return a == b || name[a - b - 1] == '.';
}

/* the longest matching address=/name/ip, 0 if none */
static inline bool dns_override(const dns_conf_t *c, const char *name, uint32_t *ip) {
    size_t best = 0; bool hit = false;
    for (int i = 0; i < c->n_ovr; i++)
        if (dns_in_domain(name, c->ovr[i].name) && strlen(c->ovr[i].name) >= best) {
            best = strlen(c->ovr[i].name); *ip = c->ovr[i].ip; hit = true;
        }
    return hit;
}

/* which upstream for this name: a server=/domain/ip match, else the default list (index `try_i`).
   0 when there is none. */
static inline uint32_t dns_upstream(const dns_conf_t *c, const char *name, int try_i) {
    size_t best = 0; uint32_t ip = 0;
    for (int i = 0; i < c->n_dom; i++)
        if (dns_in_domain(name, c->dom[i].name) && strlen(c->dom[i].name) >= best) { best = strlen(c->dom[i].name); ip = c->dom[i].ip; }
    if (ip) return ip;
    if (!c->n_up) return 0;
    return c->up[try_i % c->n_up];
}

/* dnsmasq's domain-needed and bogus-priv: true if the name must not go upstream */
static inline bool dns_local_only(const dns_q_t *q) {
    if (q->qtype != DNS_PTR && !strchr(q->name, '.')) return true;               /* plain name */
    if (q->qtype == DNS_PTR) {
        size_t n = strlen(q->name);
        const char *suf = ".in-addr.arpa";
        if (n > 13 && !strcmp(q->name + n - 13, suf)) {
            unsigned a, b, c2, d;
            if (sscanf(q->name, "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) {      /* reversed: d.c.b.a */
                if (d == 10 || d == 127 || d == 0) return true;
                if (d == 192 && c2 == 168) return true;
                if (d == 172 && c2 >= 16 && c2 <= 31) return true;
                if (d == 169 && c2 == 254) return true;
            }
        }
    }
    return false;
}
