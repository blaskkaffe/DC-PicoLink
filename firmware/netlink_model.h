/*
 * Netlink tunnel (eaudunord/Netlink netlink.py, used by DreamPi 2.x): what a dialled number
 * means, and the small formats the tunnel speaks. The ESP32 side is esp32/main/netlink.c.
 * Host-compilable (tests/test_netlink.c).
 *
 * Modes (digit_parser):
 *   "0" / "00"                     direct dial, waiting for a call (no matchmaking)
 *   a code from netlink_config.ini server dial code: TCP relay to a game server
 *   "*70"                          call waiting off: ignore
 *   "*69"                          DCNet prefix (dcnet_model.h)
 *   XBAND server numbers           XBAND (Genesis/SNES) server at retrocomputing.network
 *   "0642542154"                   Capcom (KDDI) matching: PPP with fast LCP echo
 *   "#0xx#"                        waiting, matchmaking for game xx
 *   "#a*b*c*d*1#"                  XBAND call to the tunnel at a.b.c.d
 *   "#a*b*c*d#" / "#aaabbbcccddd#" direct call to a.b.c.d
 *   "#xxx#" (other)                calling through matchmaking (999 = matching off, 888 = on)
 *   anything else                  PPP (or DCNet)
 */
#ifndef NETLINK_MODEL_H
#define NETLINK_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum { NL_PPP = 0, NL_IDLE, NL_DCNET_PREFIX, NL_WAIT, NL_CALL, NL_SERVER, NL_XBAND_SERVER, NL_XBAND_CALL, NL_CAPCOM };

#define NL_TCP_PORT     65432      /* direct-dial handshake */
#define NL_XBAND_PORT   65433      /* XBAND handshake */
#define NL_UDP_WAIT     20001      /* data exchange, waiting side */
#define NL_UDP_CALL     20002      /* data exchange, calling side */
#define NL_MATCH_HOST   "saturn.dreampipe.net"
#define NL_MATCH_PATH   "/match_service.php"
#define NL_MATCH_KEY    "mySuperSecretSaturnKey1234"
#define NL_STUN_HOST    "stun.l.google.com"
#define NL_STUN_PORT    19302
#define NL_XBAND_HOST   "xbserver.retrocomputing.network"
#define NL_XBAND_SRV_PORT 56969
#define NL_CONFIG_URL   "https://raw.githubusercontent.com/eaudunord/Netlink/main/tunnel/netlink_config.ini"

/* ---- game servers (netlink_config.ini [server:CODE]) ---- */
#define NL_SRV_MAX 16
typedef struct {
    char code[16], name[32], host[64];
    uint16_t port;
    bool transparent, fix_ppp, dcnow;
    char secret[48], magic[12];
    float auth_timeout;
} nl_server_t;
typedef struct { nl_server_t s[NL_SRV_MAX]; int n; } nl_servers_t;

static inline void nl__trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* the built-in entry netlink.py always adds (Sega Rally 2) */
static inline void nl_servers_defaults(nl_servers_t *c) {
    memset(c, 0, sizeof *c);
    nl_server_t *s = &c->s[c->n++];
    strcpy(s->code, "0352002700"); strcpy(s->name, "SegaRally2"); strcpy(s->host, "shumania.ddns.net");
    s->port = 21330; s->transparent = true; s->dcnow = true; strcpy(s->magic, "AUTH"); s->auth_timeout = 5;
}

/* One line of the ini; `cur` = index of the section being read (-1 = not a server section). */
static inline void nl_servers_line(nl_servers_t *c, int *cur, const char *line) {
    char l[160];
    strncpy(l, line, sizeof l - 1); l[sizeof l - 1] = 0;
    char *sc = strchr(l, ';'); if (sc && (sc == l || sc[-1] == ' ' || sc[-1] == '\t')) *sc = 0;
    nl__trim(l);
    if (!l[0] || l[0] == '#') return;
    if (l[0] == '[') {
        *cur = -1;
        char *e = strchr(l, ']');
        if (!e || strncmp(l + 1, "server:", 7)) return;
        *e = 0;
        const char *code = l + 8;
        int k = -1;
        for (int i = 0; i < c->n; i++) if (!strcmp(c->s[i].code, code)) k = i;      /* the file replaces a built-in one */
        if (k < 0) {
            if (c->n >= NL_SRV_MAX) return;
            k = c->n++;
        }
        memset(&c->s[k], 0, sizeof c->s[k]);
        strncpy(c->s[k].code, code, sizeof c->s[k].code - 1);
        strcpy(c->s[k].magic, "AUTH"); c->s[k].auth_timeout = 5;
        *cur = k;
        return;
    }
    if (*cur < 0) return;
    char *eq = strchr(l, '=');
    if (!eq) return;
    *eq = 0;
    char *k = l, *v = eq + 1;
    nl__trim(k); nl__trim(v);
    nl_server_t *s = &c->s[*cur];
    bool yes = !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "True") || !strcmp(v, "TRUE");
    if (!strcmp(k, "host")) strncpy(s->host, v, sizeof s->host - 1);
    else if (!strcmp(k, "port")) s->port = (uint16_t)atoi(v);
    else if (!strcmp(k, "name")) strncpy(s->name, v, sizeof s->name - 1);
    else if (!strcmp(k, "handler")) s->transparent = !strcmp(v, "transparent");
    else if (!strcmp(k, "shared_secret")) strncpy(s->secret, v, sizeof s->secret - 1);
    else if (!strcmp(k, "auth_magic")) strncpy(s->magic, v, sizeof s->magic - 1);
    else if (!strcmp(k, "auth_timeout")) s->auth_timeout = (float)atof(v);
    else if (!strcmp(k, "ppp_fix_delimiters")) s->fix_ppp = yes;
    else if (!strcmp(k, "dcnow")) s->dcnow = yes;
}

static inline const nl_server_t *nl_server_find(const nl_servers_t *c, const char *code) {
    for (int i = 0; i < c->n; i++) if (c->s[i].host[0] && c->s[i].port && !strcmp(c->s[i].code, code)) return &c->s[i];
    return NULL;
}

/* ---- what was dialled ---- */
typedef struct { int mode; char target[40]; } nl_dial_t;      /* target: game id / ip / server code */

static inline bool nl__digits(const char *s) { if (!*s) return false; for (; *s; s++) if (*s < '0' || *s > '9') return false; return true; }

static inline nl_dial_t nl_classify(const char *raw, const nl_servers_t *srv) {
    nl_dial_t d; memset(&d, 0, sizeof d);
    size_t n = strlen(raw);
    strncpy(d.target, raw, sizeof d.target - 1);
    if (!n) { d.mode = NL_IDLE; return d; }
    if (!strcmp(raw, "0") || !strcmp(raw, "00")) { d.mode = NL_WAIT; return d; }
    if (srv && nl_server_find(srv, raw)) { d.mode = NL_SERVER; return d; }
    if (!strcmp(raw, "*70")) { d.mode = NL_IDLE; return d; }
    if (!strcmp(raw, "*69")) { d.mode = NL_DCNET_PREFIX; return d; }
    if (!strcmp(raw, "18002071194") || !strcmp(raw, "19209492263") || !strcmp(raw, "0120717360") || !strcmp(raw, "0355703001")) { d.mode = NL_XBAND_SERVER; return d; }
    if (!strcmp(raw, "0642542154")) { d.mode = NL_CAPCOM; return d; }
    if (n >= 2 && raw[0] == '#' && raw[n - 1] == '#') {
        char s[40]; size_t k = 0;
        for (size_t i = 0; i < n && k + 1 < sizeof s; i++) if (raw[i] != '#') s[k++] = raw[i];
        s[k] = 0;
        int stars = 0; for (size_t i = 0; i < k; i++) stars += s[i] == '*';
        if (k == 3 && s[0] == '0') { d.mode = NL_WAIT; strcpy(d.target, s); return d; }
        if (stars == 4 && s[k - 1] == '1' && s[k - 2] == '*') {                  /* a*b*c*d*1 */
            s[k - 2] = 0;
            int a, b, c2, e;
            if (sscanf(s, "%d*%d*%d*%d", &a, &b, &c2, &e) != 4) { d.mode = NL_IDLE; return d; }
            snprintf(d.target, sizeof d.target, "%d.%d.%d.%d", a, b, c2, e);
            d.mode = NL_XBAND_CALL; return d;
        }
        if (stars == 3) {
            int a, b, c2, e;
            if (sscanf(s, "%d*%d*%d*%d", &a, &b, &c2, &e) != 4) { d.mode = NL_IDLE; return d; }
            snprintf(d.target, sizeof d.target, "%d.%d.%d.%d", a, b, c2, e);
            d.mode = NL_CALL; return d;
        }
        if (k == 12) {
            if (!nl__digits(s)) { d.mode = NL_IDLE; return d; }
            int p[4];
            for (int i = 0; i < 4; i++) p[i] = (s[3 * i] - '0') * 100 + (s[3 * i + 1] - '0') * 10 + (s[3 * i + 2] - '0');
            snprintf(d.target, sizeof d.target, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
            d.mode = NL_CALL; return d;
        }
        d.mode = NL_CALL; strcpy(d.target, s);                                   /* matchmaking call */
        return d;
    }
    d.mode = NL_PPP;
    return d;
}

/* matchmaking: "001002003004" <-> "1.2.3.4" */
static inline bool nl_ip12_to_dotted(const char *a, char *out, size_t cap) {
    if (strlen(a) != 12 || !nl__digits(a)) return false;
    int p[4];
    for (int i = 0; i < 4; i++) p[i] = (a[3 * i] - '0') * 100 + (a[3 * i + 1] - '0') * 10 + (a[3 * i + 2] - '0');
    snprintf(out, cap, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
    return true;
}
static inline void nl_ip_to_12(uint32_t ip_host, char *out) {        /* out: 13 bytes */
    snprintf(out, 13, "%03u%03u%03u%03u", (unsigned)(ip_host >> 24), (unsigned)(ip_host >> 16 & 255), (unsigned)(ip_host >> 8 & 255), (unsigned)(ip_host & 255));
}

/* the match service's JSON: "status" and "opponent ip_address": ["001002003004", "20001"] */
static inline bool nl_json_str(const char *j, const char *key, char *out, size_t cap) {
    char k[48]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k);
    if (!p) return false;
    p = strchr(p + strlen(k), ':'); if (!p) return false;
    p++; while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0; while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return *p == '"';
}
static inline bool nl_json_opponent(const char *j, char *ip, size_t cap, uint16_t *port) {
    const char *p = strstr(j, "\"opponent ip_address\"");
    if (!p) return false;
    p = strchr(p, '['); if (!p) return false;
    char a[24] = "", b[12] = ""; size_t i;
    p = strchr(p, '"'); if (!p) return false; p++;
    for (i = 0; *p && *p != '"' && i + 1 < sizeof a; ) a[i++] = *p++;
    a[i] = 0;
    if (*p != '"') return false;
    p++;
    while (*p && *p != '"' && *p != ']' && !(*p >= '0' && *p <= '9')) p++;
    if (*p == '"') p++;
    for (i = 0; *p >= '0' && *p <= '9' && i + 1 < sizeof b; ) b[i++] = *p++;
    b[i] = 0;
    if (!b[0] || !nl_ip12_to_dotted(a, ip, cap)) return false;
    *port = (uint16_t)atoi(b);
    return true;
}

/* ---- the data exchange (listener / sender) ----
   Each UDP datagram: up to the last 5 chunks, newest first, joined by "<packetSplit>"; a chunk
   is payload + "<dataSplit>" + decimal sequence number. The receiver takes the chunk with the
   sequence it expects, or the newest, and drops data older than expected. */
#define NL_PSPLIT "<packetSplit>"
#define NL_DSPLIT "<dataSplit>"
#define NL_KEEP 5
#define NL_CHUNK_MAX 1024

typedef struct { uint8_t d[NL_KEEP][NL_CHUNK_MAX + 24]; size_t n[NL_KEEP]; int have; uint32_t seq; } nl_tx_t;
typedef struct { uint32_t expect; uint32_t recovered; } nl_rx_t;

static inline const uint8_t *nl__find(const uint8_t *h, size_t hn, const char *nd, size_t from) {
    size_t l = strlen(nd);
    for (size_t i = from; i + l <= hn; i++) if (!memcmp(h + i, nd, l)) return h + i;
    return NULL;
}

/* add a chunk; out gets the datagram (cap >= NL_KEEP * (NL_CHUNK_MAX + 37)) */
static inline size_t nl_tx_pack(nl_tx_t *t, const uint8_t *p, size_t n, uint8_t *out, size_t cap) {
    if (n > NL_CHUNK_MAX) n = NL_CHUNK_MAX;
    for (int i = NL_KEEP - 1; i > 0; i--) { memcpy(t->d[i], t->d[i - 1], t->n[i - 1]); t->n[i] = t->n[i - 1]; }
    memcpy(t->d[0], p, n);
    int k = snprintf((char *)t->d[0] + n, 24, NL_DSPLIT "%lu", (unsigned long)t->seq++);
    t->n[0] = n + (size_t)k;
    if (t->have < NL_KEEP) t->have++;
    size_t o = 0;
    for (int i = 0; i < t->have; i++) {
        size_t sl = strlen(NL_PSPLIT);
        if (i) { if (o + sl > cap) break; memcpy(out + o, NL_PSPLIT, sl); o += sl; }
        if (o + t->n[i] > cap) break;
        memcpy(out + o, t->d[i], t->n[i]); o += t->n[i];
    }
    return o;
}

/* a datagram from the other side; *pay, *pn = the payload for the Dreamcast, or false.
   *more: an older chunk was taken to fill a gap, call again for the next one (listener loop). */
static inline bool nl_rx_take(nl_rx_t *r, const uint8_t *d, size_t n, const uint8_t **pay, size_t *pn, bool *more) {
    *more = false;
    const uint8_t *c[NL_KEEP + 3]; size_t cn[NL_KEEP + 3]; int k = 0;
    size_t s = 0, sl = strlen(NL_PSPLIT);
    while (k < NL_KEEP + 3) {
        const uint8_t *e = nl__find(d, n, NL_PSPLIT, s);
        size_t end = e ? (size_t)(e - d) : n;
        c[k] = d + s; cn[k] = end - s; k++;
        if (!e) break;
        s = end + sl;
    }
    int pick = 0; long seq_pick = -1;
    for (int i = 0; i < k; i++) {
        const uint8_t *ds = nl__find(c[i], cn[i], NL_DSPLIT, 0);
        if (!ds) continue;
        long q = strtol((const char *)ds + strlen(NL_DSPLIT), NULL, 10);
        if (i == 0) seq_pick = q;
        if (q == (long)r->expect) { pick = i; seq_pick = q; break; }
    }
    const uint8_t *ds = nl__find(c[pick], cn[pick], NL_DSPLIT, 0);
    if (!ds) return false;
    char num[16]; size_t nl = (size_t)(c[pick] + cn[pick] - (ds + strlen(NL_DSPLIT)));
    if (!nl || nl >= sizeof num) return false;
    memcpy(num, ds + strlen(NL_DSPLIT), nl); num[nl] = 0;
    for (size_t i = 0; i < nl; i++) if (num[i] < '0' || num[i] > '9') return false;
    long q = strtol(num, NULL, 10);
    (void)seq_pick;
    if (q < (long)r->expect) return false;                         /* all old */
    if (pick > 0) { r->recovered++; *more = true; }
    r->expect = (uint32_t)q + 1;
    *pay = c[pick]; *pn = (size_t)(ds - c[pick]);
    return true;
}

/* ---- STUN (RFC 5389 binding request; MAPPED- or XOR-MAPPED-ADDRESS in the answer) ---- */
static inline size_t nl_stun_request(uint8_t *b, const uint8_t tid[12]) {
    b[0] = 0; b[1] = 1; b[2] = 0; b[3] = 0;
    b[4] = 0x21; b[5] = 0x12; b[6] = 0xA4; b[7] = 0x42;
    memcpy(b + 8, tid, 12);
    return 20;
}
static inline bool nl_stun_answer(const uint8_t *b, size_t n, const uint8_t tid[12], uint32_t *ip_host, uint16_t *port) {
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01 || memcmp(b + 8, tid, 12)) return false;
    size_t len = (size_t)(b[2] << 8 | b[3]), p = 20;
    if (20 + len > n) len = n - 20;
    bool got = false;
    while (p + 4 <= 20 + len) {
        uint16_t t = (uint16_t)(b[p] << 8 | b[p + 1]), l = (uint16_t)(b[p + 2] << 8 | b[p + 3]);
        const uint8_t *v = b + p + 4;
        if (p + 4 + l > 20 + len) break;
        if ((t == 0x0020 || t == 0x8020 || t == 0x0001) && l >= 8 && v[1] == 1) {
            uint16_t pt = (uint16_t)(v[2] << 8 | v[3]);
            uint32_t a = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            if (t != 0x0001) { pt ^= 0x2112; a ^= 0x2112A442u; }
            *ip_host = a; *port = pt; got = true;
            if (t != 0x0001) return true;                          /* the XOR one is the reliable one */
        }
        p += 4 + (size_t)((l + 3) & ~3u);
    }
    return got;
}

/* ---- PPP frame delimiters (netlink.py ppp_fix_frame_delimiters) ----
   collapse runs of 0x7e, double every flag, and open a frame that continues a flag-ended chunk.
   out needs 2*n+1. */
static inline size_t nl_ppp_fix(const uint8_t *in, size_t n, bool *prev_flag, uint8_t *out) {
    if (!n) return 0;
    bool ended = in[n - 1] == 0x7e;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0x7e) {
            if (i && in[i - 1] == 0x7e) continue;                 /* part of a run already written */
            out[o++] = 0x7e; out[o++] = 0x7e;
        } else out[o++] = in[i];
    }
    if (*prev_flag && out[0] != 0x7e) { memmove(out + 1, out, o); out[0] = 0x7e; o++; }
    *prev_flag = ended;
    return o;
}

/* the standard server's login: magic, length byte, secret */
static inline size_t nl_auth(const char *magic, const char *secret, uint8_t *out, size_t cap) {
    size_t m = strlen(magic), s = strlen(secret);
    if (m + 1 + s > cap || s > 255) return 0;
    memcpy(out, magic, m); out[m] = (uint8_t)s; memcpy(out + m + 1, secret, s);
    return m + 1 + s;
}
#endif
