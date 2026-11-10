/* OpenVPN client, see ovpn.h. Wire format as in OpenVPN 2.x (ssl.c, crypto.c, reliable.c). */
#include "ovpn.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/error.h"

/* ---- opcodes ---- */
#define P_CONTROL_HARD_RESET_CLIENT_V2 7
#define P_CONTROL_HARD_RESET_SERVER_V2 8
#define P_CONTROL_SOFT_RESET_V1        3
#define P_CONTROL_V1                   4
#define P_ACK_V1                       5
#define P_DATA_V1                      6

#define CTL_MAX      1100      /* control payload per packet (OpenVPN's default is 1250) */
#define CTL_POOL     12
#define CTL_WINDOW   4         /* packets in flight */
#define ACK_MAX      8
#define RXS_SIZE     12288     /* TLS bytes received but not yet read by mbedTLS */
#define APP_SIZE     3072
#define WR_SIZE      1536
#define RESEND_MS    2000
#define RESEND_MAX   10
#define HANDSHAKE_MS 45000
#define SWITCH_MS    1000      /* new key must be this old before we send with it */
#define RETIRE_MS    10000     /* old key is kept this long after the switch */

static const uint8_t PING_MAGIC[16] = {0x2a, 0x18, 0x7b, 0xf3, 0x64, 0x1e, 0xb4, 0xcb,
                                       0x07, 0xed, 0x2d, 0x0a, 0x98, 0x1f, 0xc7, 0x48};

typedef struct {
    uint8_t  used, tries;
    uint16_t len;
    uint32_t id, sent;
    uint8_t  data[CTL_MAX];
} ctl_t;

typedef struct {
    mbedtls_gcm_context gcm;
    uint8_t  iv[8];
    int      ok;
} dkey_t;

/* One key: its own reliable control channel, TLS session and data keys. */
typedef struct ks {
    struct ovpn *o;
    uint8_t  kid;
    int      first;             /* 1: initial key (hard reset), 0: renegotiated (soft reset) */
    ovpn_state_t st;            /* RESET, TLS, KEYS, PUSH (first key only), UP = keys ready */

    ctl_t    pool[CTL_POOL];
    uint32_t tx_next, rx_next;
    uint32_t ackq[ACK_MAX];
    int      nack;

    mbedtls_ssl_context ssl;
    int      ssl_ok;
    uint8_t  rxs[RXS_SIZE]; size_t rxs_len;
    uint8_t  app[APP_SIZE]; size_t app_len;
    uint8_t  wr[WR_SIZE];   size_t wr_len;
    uint8_t  pre_master[48], r1[32], r2[32];
    int      sent_keymethod;

    dkey_t   enc, dec;
    uint32_t pid_tx, pid_rx_max;
    uint64_t pid_rx_bits;

    uint32_t t_born, t_keys, t_push, t_retire;
    uint32_t seq;               /* creation order: only a newer key takes over */
} ks_t;

struct ovpn {
    ovpn_conf_t cf;
    ovpn_ops_t  ops;
    ovpn_state_t state;
    char err[96];

    /* tls-auth */
    uint8_t  ta_send[32], ta_recv[32];
    int      ta;
    uint32_t ta_pid_tx, ta_pid_rx, ta_time_rx;

    /* session */
    uint8_t  sid[8], rsid[8];
    int      have_rsid;

    ks_t    *slot[2];
    int      cur;               /* slot that sends data, -1 before the first key is ready */
    int      keylen;

    mbedtls_ssl_config   conf;
    mbedtls_x509_crt     ca, cert;
    mbedtls_pk_context   key;
    int      tls_ready;

    ovpn_info_t info;
    ovpn_stats_t st;

    uint32_t t_start, t_last_rx, t_last_tx;
    uint32_t now;
    uint32_t nseq;
};

static void logf_(ovpn_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(ovpn_t *o, const char *fmt, ...)
{
    if (!o->ops.log) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    o->ops.log(o->ops.ctx, b);
}

static void fail(ovpn_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(ovpn_t *o, const char *fmt, ...)
{
    if (o->state == OVPN_FAILED) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->err, sizeof o->err, fmt, ap);
    va_end(ap);
    o->state = OVPN_FAILED;
    logf_(o, "VPN failed: %s", o->err);
}

static void cpy(char *d, size_t n, const char *s) { size_t l = strlen(s); if (l >= n) l = n - 1; memcpy(d, s, l); d[l] = 0; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int rnd(ovpn_t *o, uint8_t *b, size_t n) { return o->ops.random(o->ops.ctx, b, n); }
static int rnd_cb(void *ctx, unsigned char *b, size_t n) { return rnd((ovpn_t *)ctx, b, n); }

/* ======================= config file ======================= */

static char *dup_range(const char *s, const char *e)
{
    char *r = malloc((size_t)(e - s) + 1);
    if (!r) return NULL;
    memcpy(r, s, (size_t)(e - s));
    r[e - s] = 0;
    return r;
}

/* <tag> ... </tag> -> text between (leading whitespace trimmed) */
static char *block(const char *text, const char *tag)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(text, open);
    if (!a) return NULL;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b) return NULL;
    while (a < b && (*a == '\n' || *a == '\r' || *a == ' ')) a++;
    return dup_range(a, b);
}

int ovpn_conf_parse(const char *text, ovpn_conf_t *c)
{
    memset(c, 0, sizeof *c);
    c->key_direction = -1;
    c->ping_s = 10;
    c->restart_s = 60;
    c->port = 1194;
    c->udp = 1;
    c->tun = 1;
    strcpy(c->cipher, "AES-128-GCM");

    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        char line[160];
        size_t n = (size_t)(e - p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        char host[100]; int v; char s[40];
        if (line[0] == '<') {
            /* skip inline blocks */
            char tag[32];
            if (sscanf(line, "<%31[^>]>", tag) == 1 && tag[0] != '/') {
                char close[40];
                snprintf(close, sizeof close, "</%s>", tag);
                const char *q = strstr(e, close);
                if (q) { p = q + strlen(close); while (*p == '\n' || *p == '\r') p++; continue; }
            }
        } else if (!strncmp(line, "remote ", 7) && sscanf(line + 7, "%99s %d", host, &v) == 2) {
            if (!c->host) { c->host = strdup(host); c->port = v; }
        } else if (!strncmp(line, "remote ", 7) && sscanf(line + 7, "%99s", host) == 1) {
            if (!c->host) c->host = strdup(host);
        } else if (!strncmp(line, "proto ", 6) && sscanf(line + 6, "%39s", s) == 1) {
            c->udp = strncmp(s, "udp", 3) == 0;
        } else if (!strncmp(line, "dev ", 4) && sscanf(line + 4, "%39s", s) == 1) {
            c->tun = strncmp(s, "tun", 3) == 0;
        } else if (!strncmp(line, "key-direction ", 14) && sscanf(line + 14, "%d", &v) == 1) {
            c->key_direction = v;
        } else if (!strncmp(line, "ping-restart ", 13) && sscanf(line + 13, "%d", &v) == 1) {
            c->restart_s = v;
        } else if (!strncmp(line, "ping ", 5) && sscanf(line + 5, "%d", &v) == 1) {
            c->ping_s = v;
        } else if (!strncmp(line, "cipher ", 7) && sscanf(line + 7, "%23s", s) == 1) {
            cpy(c->cipher, sizeof c->cipher, s);
        } else if (strncmp(line, "explicit-exit-notify", 20) == 0) {
            c->explicit_exit = 1;
        }
        p = *e ? e + 1 : e;
    }
    c->ca = block(text, "ca");
    c->cert = block(text, "cert");
    c->key = block(text, "key");
    c->tlsauth = block(text, "tls-auth");
    /* the cert block of Easy-RSA files starts with a text dump: keep from BEGIN CERTIFICATE */
    if (c->cert) {
        char *b = strstr(c->cert, "-----BEGIN CERTIFICATE-----");
        if (b && b != c->cert) memmove(c->cert, b, strlen(b) + 1);
    }
    if (!c->host) return -1;
    if (!c->ca) return -2;
    if (!c->cert) return -3;
    if (!c->key) return -4;
    if (!c->tlsauth) return -5;
    if (!c->udp) return -6;
    if (!c->tun) return -7;
    return 0;
}

void ovpn_conf_free(ovpn_conf_t *c)
{
    free(c->host); free(c->ca); free(c->cert); free(c->key); free(c->tlsauth);
    memset(c, 0, sizeof *c);
}

/* "OpenVPN Static key V1" -> 256 bytes */
static int parse_static_key(const char *t, uint8_t k[256])
{
    const char *b = strstr(t, "-----BEGIN OpenVPN Static key V1-----");
    if (!b) return -1;
    b = strchr(b, '\n');
    if (!b) return -1;
    int n = 0, hi = -1;
    for (b++; *b && strncmp(b, "-----END", 8) != 0; b++) {
        int d;
        if (*b >= '0' && *b <= '9') d = *b - '0';
        else if (*b >= 'a' && *b <= 'f') d = *b - 'a' + 10;
        else if (*b >= 'A' && *b <= 'F') d = *b - 'A' + 10;
        else if (*b == '#') { while (*b && *b != '\n') b++; if (!*b) break; continue; }
        else continue;
        if (hi < 0) hi = d;
        else { if (n < 256) k[n] = (uint8_t)(hi << 4 | d); n++; hi = -1; }
    }
    return n == 256 ? 0 : -1;
}

/* ======================= tls-auth + control packets ======================= */

static int hmac256(const uint8_t *key, const uint8_t *d, size_t n, uint8_t out[32])
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, d, n, out);
}

/* tls-auth: HMAC over  packet_id|time | opcode|sid | rest  */
static int ta_mac(const uint8_t *key, const uint8_t *pkt, size_t n, uint8_t out[32])
{
    uint8_t tmp[1500];
    if (n < 49 || n > sizeof tmp) return -1;
    memcpy(tmp, pkt + 41, 8);           /* packet id + time */
    memcpy(tmp + 8, pkt, 9);            /* opcode + session id */
    memcpy(tmp + 17, pkt + 49, n - 49); /* the rest */
    return hmac256(key, tmp, 17 + n - 49, out);
}

static uint32_t unix_now(ovpn_t *o)
{
    uint32_t t = o->ops.unix_time ? o->ops.unix_time(o->ops.ctx) : 0;
    return t ? t : o->now / 1000 + 1;
}

/* Build and send one control packet (op != P_ACK_V1 carries `id` and `payload`). */
static void ctl_emit(ks_t *k, int op, uint32_t id, const uint8_t *pl, size_t n)
{
    ovpn_t *o = k->o;
    uint8_t b[1500];
    size_t i = 0;
    b[i++] = (uint8_t)(op << 3 | k->kid);
    memcpy(b + i, o->sid, 8); i += 8;
    i += 32;                                    /* hmac, filled in below */
    put32(b + i, ++o->ta_pid_tx); i += 4;
    put32(b + i, unix_now(o)); i += 4;
    int na = o->have_rsid ? k->nack : 0;
    if (na > 4) na = 4;
    b[i++] = (uint8_t)na;
    for (int x = 0; x < na; x++) { put32(b + i, k->ackq[x]); i += 4; }
    if (na) {
        memcpy(b + i, o->rsid, 8); i += 8;
        memmove(k->ackq, k->ackq + na, (size_t)(k->nack - na) * sizeof k->ackq[0]);
        k->nack -= na;
    }
    if (op != P_ACK_V1) { put32(b + i, id); i += 4; memcpy(b + i, pl, n); i += n; }
    if (ta_mac(o->ta_send, b, i, b + 9) != 0) return;
    o->ops.send(o->ops.ctx, b, i);
    o->t_last_tx = o->now;
}

static void queue_ack(ks_t *k, uint32_t id)
{
    for (int x = 0; x < k->nack; x++) if (k->ackq[x] == id) return;
    if (k->nack < ACK_MAX) k->ackq[k->nack++] = id;
}

static void flush_acks(ks_t *k)
{
    while (k->nack > 0 && k->o->have_rsid) ctl_emit(k, P_ACK_V1, 0, NULL, 0);
}

static int pool_free(const ks_t *k)
{
    int f = 0;
    for (int x = 0; x < CTL_POOL; x++) if (!k->pool[x].used) f++;
    return f;
}

static int inflight(const ks_t *k)
{
    int f = 0;
    for (int x = 0; x < CTL_POOL; x++) if (k->pool[x].used && k->pool[x].tries) f++;
    return f;
}

static int first_op(const ks_t *k, uint32_t id)
{
    if (id != 0) return P_CONTROL_V1;
    return k->first ? P_CONTROL_HARD_RESET_CLIENT_V2 : P_CONTROL_SOFT_RESET_V1;
}

/* Queue a reliable message; sent as soon as the window allows. */
static int ctl_queue(ks_t *k, const uint8_t *d, size_t n)
{
    for (int x = 0; x < CTL_POOL; x++) {
        ctl_t *c = &k->pool[x];
        if (c->used) continue;
        c->used = 1; c->tries = 0; c->len = (uint16_t)n; c->id = k->tx_next++;
        c->sent = 0;
        memcpy(c->data, d, n);
        return x;
    }
    return -1;
}

static void ctl_pump_tx(ks_t *k)
{
    for (;;) {
        if (inflight(k) >= CTL_WINDOW) return;
        ctl_t *best = NULL;
        for (int x = 0; x < CTL_POOL; x++) {
            ctl_t *c = &k->pool[x];
            if (c->used && !c->tries && (!best || (int32_t)(c->id - best->id) < 0)) best = c;
        }
        if (!best) return;
        best->tries = 1;
        best->sent = k->o->now;
        ctl_emit(k, first_op(k, best->id), best->id, best->data, best->len);
    }
}

static void ctl_retransmit(ks_t *k)
{
    ovpn_t *o = k->o;
    for (int x = 0; x < CTL_POOL; x++) {
        ctl_t *c = &k->pool[x];
        if (!c->used || !c->tries) continue;
        if ((uint32_t)(o->now - c->sent) < RESEND_MS) continue;
        if (c->tries >= RESEND_MAX) { fail(o, "server does not answer"); return; }
        c->tries++;
        c->sent = o->now;
        o->st.ctl_resend++;
        ctl_emit(k, first_op(k, c->id), c->id, c->data, c->len);
    }
}

/* ======================= TLS glue ======================= */

static int bio_send(void *ctx, const unsigned char *d, size_t n)
{
    ks_t *k = ctx;
    size_t chunks = (n + CTL_MAX - 1) / CTL_MAX;
    if ((size_t)pool_free(k) < chunks) return MBEDTLS_ERR_SSL_WANT_WRITE;
    for (size_t off = 0; off < n; off += CTL_MAX) {
        size_t m = n - off < CTL_MAX ? n - off : CTL_MAX;
        ctl_queue(k, d + off, m);
    }
    ctl_pump_tx(k);
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *d, size_t n)
{
    ks_t *k = ctx;
    if (!k->rxs_len) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > k->rxs_len) n = k->rxs_len;
    memcpy(d, k->rxs, n);
    k->rxs_len -= n;
    memmove(k->rxs, k->rxs + n, k->rxs_len);
    return (int)n;
}

static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx; (void)crt; (void)depth;
    /* The ESP32 often has no clock when the tunnel starts: the CA is pinned and tls-auth
     * protects the handshake, so validity dates are not enforced. */
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

/* ======================= key-method 2 ======================= */

/* TLS 1.0/1.1 PRF with MD5 and SHA1, as OpenVPN uses it */
static int p_hash(mbedtls_md_type_t t, const uint8_t *secret, size_t sl, const uint8_t *seed, size_t seedl,
                  uint8_t *out, size_t outl)
{
    const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(t);
    if (!mi) return -1;
    size_t hl = mbedtls_md_get_size(mi);
    uint8_t a[20], buf[20 + 256], h[20];
    if (seedl > 256) return -1;
    if (mbedtls_md_hmac(mi, secret, sl, seed, seedl, a)) return -1;
    for (size_t pos = 0; pos < outl; pos += hl) {
        memcpy(buf, a, hl);
        memcpy(buf + hl, seed, seedl);
        if (mbedtls_md_hmac(mi, secret, sl, buf, hl + seedl, h)) return -1;
        size_t m = outl - pos < hl ? outl - pos : hl;
        memcpy(out + pos, h, m);
        if (mbedtls_md_hmac(mi, secret, sl, a, hl, a)) return -1;
    }
    return 0;
}

static int tls1_prf(const char *label, const uint8_t *secret, size_t sl, const uint8_t *seed, size_t seedl,
                    uint8_t *out, size_t outl)
{
    uint8_t ls[256], md5[256], sha[256];
    size_t ll = strlen(label);
    if (ll + seedl > sizeof ls || outl > sizeof md5) return -1;
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seedl);
    size_t half = (sl + 1) / 2;
    if (p_hash(MBEDTLS_MD_MD5, secret, half, ls, ll + seedl, md5, outl)) return -1;
    if (p_hash(MBEDTLS_MD_SHA1, secret + sl - half, half, ls, ll + seedl, sha, outl)) return -1;
    for (size_t i = 0; i < outl; i++) out[i] = md5[i] ^ sha[i];
    return 0;
}

static int dkey_init(ovpn_t *o, dkey_t *d, const uint8_t *cipher, const uint8_t *hmac)
{
    if (d->ok) mbedtls_gcm_free(&d->gcm);
    mbedtls_gcm_init(&d->gcm);
    if (mbedtls_gcm_setkey(&d->gcm, MBEDTLS_CIPHER_ID_AES, cipher, (unsigned)o->keylen * 8)) return -1;
    memcpy(d->iv, hmac, 8);
    d->ok = 1;
    return 0;
}

static int derive_keys(ks_t *k, const uint8_t sr1[32], const uint8_t sr2[32])
{
    ovpn_t *o = k->o;
    uint8_t master[48], seed[80], key2[256], s1[64];
    memcpy(s1, k->r1, 32); memcpy(s1 + 32, sr1, 32);
    if (tls1_prf("OpenVPN master secret", k->pre_master, 48, s1, 64, master, 48)) return -1;
    memcpy(seed, k->r2, 32); memcpy(seed + 32, sr2, 32);
    memcpy(seed + 64, o->sid, 8); memcpy(seed + 72, o->rsid, 8);
    if (tls1_prf("OpenVPN key expansion", master, 48, seed, 80, key2, 256)) return -1;
    /* the client sends with key[0] and receives with key[1] */
    if (dkey_init(o, &k->enc, key2 + 0, key2 + 64)) return -1;
    if (dkey_init(o, &k->dec, key2 + 128, key2 + 192)) return -1;
    memset(master, 0, sizeof master);
    memset(key2, 0, sizeof key2);
    return 0;
}

static void tls_put(ks_t *k, const void *d, size_t n)
{
    if (k->wr_len + n > WR_SIZE) { fail(k->o, "internal: write buffer full"); return; }
    memcpy(k->wr + k->wr_len, d, n);
    k->wr_len += n;
}

static void send_keymethod(ks_t *k)
{
    static const char opts[] = "V4,dev-type tun,link-mtu 1549,tun-mtu 1500,proto UDPv4,cipher AES-128-GCM,"
                               "auth [null-digest],keysize 128,key-method 2,tls-client";
    char peer[160];
    int pl = snprintf(peer, sizeof peer, "IV_VER=2.6.0\nIV_PLAT=freertos\nIV_NCP=2\nIV_CIPHERS=%s\n"
                      "IV_GUI_VER=DC-PicoLink\n", k->o->cf.cipher);
    uint8_t z[4] = {0, 0, 0, 0}, m = 2, l[2];
    tls_put(k, z, 4);
    tls_put(k, &m, 1);
    tls_put(k, k->pre_master, 48);
    tls_put(k, k->r1, 32);
    tls_put(k, k->r2, 32);
    l[0] = (sizeof opts) >> 8; l[1] = (sizeof opts) & 255;
    tls_put(k, l, 2); tls_put(k, opts, sizeof opts);
    l[0] = 0; l[1] = 0;
    tls_put(k, l, 2);                 /* user */
    tls_put(k, l, 2);                 /* password */
    l[0] = (uint8_t)((pl + 1) >> 8); l[1] = (uint8_t)((pl + 1) & 255);
    tls_put(k, l, 2); tls_put(k, peer, (size_t)pl + 1);
    k->sent_keymethod = 1;
}

static uint32_t ip_a(const char *s)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return a << 24 | b << 16 | c << 8 | d;
}

static void handle_push_reply(ks_t *k, char *msg)
{
    ovpn_t *o = k->o;
    ovpn_info_t *in = &o->info;
    memset(in, 0, sizeof *in);
    in->mtu = 1500;
    cpy(in->cipher, sizeof in->cipher, o->cf.cipher);
    char *save = NULL;
    for (char *t = strtok_r(msg, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char a[40], b[40], c[40];
        int n = sscanf(t, "%39s %39s %39s", a, b, c);
        if (n < 1) continue;
        if (!strcmp(a, "ifconfig") && n >= 3) {
            in->ip = ip_a(b);
            uint32_t x = ip_a(c);
            if (x && (x >> 24) == 255) in->mask = x;       /* topology subnet */
            else in->peer = x;
        } else if (!strcmp(a, "route-gateway") && n >= 2) {
            in->gateway = ip_a(b);
        } else if (!strcmp(a, "route") && n >= 2 && in->nroute < 8) {
            in->route[in->nroute].net = ip_a(b);
            in->route[in->nroute].mask = n >= 3 ? ip_a(c) : 0xffffff00u;
            in->route[in->nroute].gw = 0;
            in->nroute++;
        } else if (!strcmp(a, "redirect-gateway")) {
            in->redirect_gateway = 1;
        } else if (!strcmp(a, "dhcp-option") && n >= 3 && !strcmp(b, "DNS") && in->ndns < 2) {
            in->dns[in->ndns++] = ip_a(c);
        } else if (!strcmp(a, "ping") && n >= 2) {
            in->ping_s = atoi(b);
        } else if (!strcmp(a, "ping-restart") && n >= 2) {
            in->restart_s = atoi(b);
        } else if (!strcmp(a, "tun-mtu") && n >= 2) {
            in->mtu = atoi(b);
        } else if (!strcmp(a, "cipher") && n >= 2) {
            cpy(in->cipher, sizeof in->cipher, b);
        }
    }
    if (!in->ip) { fail(o, "server pushed no address"); return; }
    if (!in->mask) in->mask = 0xfffffffcu;     /* net30 / p2p */
    if (in->ping_s) o->cf.ping_s = in->ping_s;
    if (in->restart_s) o->cf.restart_s = in->restart_s;

    int want = !strcmp(in->cipher, "AES-256-GCM") ? 32 : !strcmp(in->cipher, "AES-128-GCM") ? 16 : 0;
    if (!want) { fail(o, "server wants cipher %s", in->cipher); return; }
    if (want != o->keylen) { fail(o, "server picked %s, only the profile's cipher is built in", in->cipher); return; }
    k->st = OVPN_UP;
    o->state = OVPN_UP;
    o->cur = k == o->slot[0] ? 0 : 1;
    o->t_last_rx = o->t_last_tx = o->now;
    logf_(o, "VPN is up: %u.%u.%u.%u/%u.%u.%u.%u", (unsigned)(in->ip >> 24), (unsigned)((in->ip >> 16) & 255),
          (unsigned)((in->ip >> 8) & 255), (unsigned)(in->ip & 255), (unsigned)(in->mask >> 24),
          (unsigned)((in->mask >> 16) & 255), (unsigned)((in->mask >> 8) & 255), (unsigned)(in->mask & 255));
}

/* stream of plaintext from the TLS session */
static void app_consume(ks_t *k)
{
    ovpn_t *o = k->o;
    for (;;) {
        if (k->st == OVPN_KEYS) {
            /* 4 zero, method, random1, random2, options (u16 len) */
            if (k->app_len < 4 + 1 + 64 + 2) return;
            uint32_t ol = (uint32_t)k->app[69] << 8 | k->app[70];
            if (k->app_len < 71 + ol) return;
            if (get32(k->app) != 0 || k->app[4] != 2) { fail(o, "server uses another key method"); return; }
            if (derive_keys(k, k->app + 5, k->app + 37)) { fail(o, "key derivation failed"); return; }
            size_t used = 71 + ol;
            k->app_len -= used;
            memmove(k->app, k->app + used, k->app_len);
            k->t_keys = o->now;
            if (k->first) {
                k->st = OVPN_PUSH;
                o->state = OVPN_PUSH;
                k->t_push = o->now - 5000;      /* ask now */
                logf_(o, "VPN keys exchanged");
            } else {
                k->st = OVPN_UP;
                logf_(o, "VPN new key %u ready", k->kid);
            }
            continue;
        }
        if (k->st == OVPN_PUSH || k->st == OVPN_UP) {
            uint8_t *z = memchr(k->app, 0, k->app_len);
            if (!z) return;
            size_t l = (size_t)(z - k->app);
            char msg[APP_SIZE];
            memcpy(msg, k->app, l + 1);
            size_t used = l + 1;
            k->app_len -= used;
            memmove(k->app, k->app + used, k->app_len);
            if (!strncmp(msg, "PUSH_REPLY", 10) && k->st == OVPN_PUSH) handle_push_reply(k, msg);
            else if (!strncmp(msg, "AUTH_FAILED", 11)) { fail(o, "server refused: %s", msg); return; }
            else if (!strncmp(msg, "RESTART", 7) || !strncmp(msg, "HALT", 4)) { fail(o, "server says %s", msg); return; }
            else if (msg[0]) logf_(o, "VPN server message: %.60s", msg);
            continue;
        }
        return;
    }
}

static void tls_pump(ks_t *k)
{
    ovpn_t *o = k->o;
    for (int guard = 0; guard < 8 && k->st >= OVPN_TLS && k->st <= OVPN_UP && o->state != OVPN_FAILED; guard++) {
        int progress = 0;
        if (k->st == OVPN_TLS) {
            int r = mbedtls_ssl_handshake(&k->ssl);
            if (r == 0) {
                const mbedtls_x509_crt *pc = mbedtls_ssl_get_peer_cert(&k->ssl);
                if (!pc || mbedtls_x509_crt_check_extended_key_usage(pc, MBEDTLS_OID_SERVER_AUTH,
                                                                      MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0) {
                    fail(o, "server certificate is not for a server");
                    return;
                }
                logf_(o, "VPN TLS up (%s)", mbedtls_ssl_get_ciphersuite(&k->ssl));
                k->st = OVPN_KEYS;
                if (k->first) o->state = OVPN_KEYS;
                if (!k->sent_keymethod) send_keymethod(k);
                progress = 1;
            } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char eb[80];
                mbedtls_strerror(r, eb, sizeof eb);
                uint32_t vf = mbedtls_ssl_get_verify_result(&k->ssl);
                fail(o, "TLS handshake: %s%s", eb, vf && vf != (uint32_t)-1 ? " (certificate not accepted)" : "");
                return;
            }
        }
        if (k->wr_len && k->st >= OVPN_KEYS) {
            int r = mbedtls_ssl_write(&k->ssl, k->wr, k->wr_len);
            if (r > 0) {
                k->wr_len -= (size_t)r;
                memmove(k->wr, k->wr + r, k->wr_len);
                progress = 1;
            } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                fail(o, "TLS write failed (%d)", r);
                return;
            }
        }
        if (k->st >= OVPN_KEYS) {
            int r = mbedtls_ssl_read(&k->ssl, k->app + k->app_len, APP_SIZE - k->app_len);
            if (r > 0) { k->app_len += (size_t)r; progress = 1; app_consume(k); if (o->state == OVPN_FAILED) return; }
            else if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) {
                if (k->first || k->st != OVPN_UP) { fail(o, "server closed the TLS session"); return; }
            }
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) { fail(o, "TLS read failed (%d)", r); return; }
        }
        if (!progress) break;
    }
}

/* ======================= slots ======================= */

static ks_t *slot_new(ovpn_t *o, int idx, uint8_t kid, int first)
{
    ks_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->o = o;
    k->kid = kid;
    k->first = first;
    k->st = first ? OVPN_RESET : OVPN_TLS;
    k->t_born = o->now;
    k->seq = ++o->nseq;
    mbedtls_ssl_init(&k->ssl);
    k->ssl_ok = 1;
    int r = mbedtls_ssl_setup(&k->ssl, &o->conf);
    if (r != 0) { fail(o, "TLS set-up failed (%d)", r); mbedtls_ssl_free(&k->ssl); free(k); return NULL; }
    mbedtls_ssl_set_bio(&k->ssl, k, bio_send, bio_recv, NULL);
    if (rnd(o, k->pre_master, 48) || rnd(o, k->r1, 32) || rnd(o, k->r2, 32)) {
        fail(o, "no random numbers");
        mbedtls_ssl_free(&k->ssl);
        free(k);
        return NULL;
    }
    o->slot[idx] = k;
    return k;
}

static void slot_free(ovpn_t *o, int idx)
{
    ks_t *k = o->slot[idx];
    if (!k) return;
    if (k->ssl_ok) mbedtls_ssl_free(&k->ssl);
    if (k->enc.ok) mbedtls_gcm_free(&k->enc.gcm);
    if (k->dec.ok) mbedtls_gcm_free(&k->dec.gcm);
    memset(k, 0, sizeof *k);
    free(k);
    o->slot[idx] = NULL;
}

/* ======================= data channel ======================= */

static int data_crypt(dkey_t *k, int enc, const uint8_t *in, size_t n, const uint8_t pid[4], uint8_t *out, uint8_t *tag)
{
    uint8_t iv[12];
    memcpy(iv, pid, 4);
    memcpy(iv + 4, k->iv, 8);
    if (enc) return mbedtls_gcm_crypt_and_tag(&k->gcm, MBEDTLS_GCM_ENCRYPT, n, iv, 12, pid, 4, in, out, 16, tag);
    return mbedtls_gcm_auth_decrypt(&k->gcm, n, iv, 12, pid, 4, tag, 16, in, out);
}

static int send_data(ovpn_t *o, const uint8_t *pt, size_t n)
{
    uint8_t b[1 + 4 + 16 + 1700];
    ks_t *k = o->cur >= 0 ? o->slot[o->cur] : NULL;
    if (n > 1700 || !k || !k->enc.ok) return -1;
    b[0] = (uint8_t)(P_DATA_V1 << 3 | k->kid);
    put32(b + 1, ++k->pid_tx);
    if (data_crypt(&k->enc, 1, pt, n, b + 1, b + 21, b + 5)) return -1;
    o->st.tx_pkts++;
    o->st.tx_bytes += (uint32_t)n;
    o->t_last_tx = o->now;
    return o->ops.send(o->ops.ctx, b, 21 + n) < 0 ? -1 : 0;
}

static void recv_data(ovpn_t *o, const uint8_t *d, size_t n)
{
    uint8_t pt[1700];
    ks_t *k = NULL;
    for (int x = 0; x < 2; x++) if (o->slot[x] && o->slot[x]->kid == (d[0] & 7) && o->slot[x]->dec.ok) k = o->slot[x];
    if (!k || n < 21 || n > 21 + sizeof pt) { o->st.bad_data++; return; }
    uint32_t pid = get32(d + 1);
    if (pid == 0) { o->st.bad_data++; return; }
    if (pid <= k->pid_rx_max) {                   /* replay window of 64 */
        uint32_t diff = k->pid_rx_max - pid;
        if (diff >= 64 || (k->pid_rx_bits >> diff & 1)) { o->st.bad_data++; return; }
    }
    if (data_crypt(&k->dec, 0, d + 21, n - 21, d + 1, pt, (uint8_t *)d + 5)) { o->st.bad_data++; return; }
    if (pid > k->pid_rx_max) {
        uint32_t sh = pid - k->pid_rx_max;
        k->pid_rx_bits = sh >= 64 ? 1 : (k->pid_rx_bits << sh) | 1;
        k->pid_rx_max = pid;
    } else {
        k->pid_rx_bits |= 1ull << (k->pid_rx_max - pid);
    }
    o->t_last_rx = o->now;
    size_t m = n - 21;
    if (m == 16 && !memcmp(pt, PING_MAGIC, 16)) return;
    if (m >= 20 && (pt[0] >> 4) == 4) {
        o->st.rx_pkts++;
        o->st.rx_bytes += (uint32_t)m;
        o->ops.ip_in(o->ops.ctx, pt, m);
    }
}

/* ======================= control input ======================= */

static void recv_control(ovpn_t *o, int op, const uint8_t *d, size_t n)
{
    uint8_t mac[32];
    if (n < 50 || !o->ta) return;
    if (ta_mac(o->ta_recv, d, n, mac) || memcmp(mac, d + 9, 32)) { o->st.bad_hmac++; return; }
    uint32_t pid = get32(d + 41), tm = get32(d + 45);
    if (tm < o->ta_time_rx || (tm == o->ta_time_rx && pid <= o->ta_pid_rx)) return;      /* replay */
    o->ta_time_rx = tm; o->ta_pid_rx = pid;

    if (o->have_rsid && memcmp(o->rsid, d + 1, 8)) return;                              /* another session */
    size_t i = 49;
    int na = d[i++];
    if (n < i + (size_t)na * 4 + (na ? 8 : 0)) return;
    const uint8_t *acks = d + i;
    i += (size_t)na * 4;
    if (na) {
        if (memcmp(d + i, o->sid, 8)) return;                                           /* not for our session */
        i += 8;
    }
    if (!o->have_rsid) {
        if (op != P_CONTROL_HARD_RESET_SERVER_V2) return;
        memcpy(o->rsid, d + 1, 8);
        o->have_rsid = 1;
    }

    /* which key? */
    uint8_t kid = d[0] & 7;
    ks_t *k = NULL;
    for (int x = 0; x < 2; x++) if (o->slot[x] && o->slot[x]->kid == kid) k = o->slot[x];
    if (!k) {
        if (op != P_CONTROL_SOFT_RESET_V1 || o->state != OVPN_UP) return;
        int idx = o->slot[0] ? (o->slot[1] ? -1 : 1) : 0;
        if (idx < 0) return;                       /* both slots busy: the server will ask again */
        k = slot_new(o, idx, kid, 0);
        if (!k) return;
        logf_(o, "VPN server renegotiates (key %u)", kid);
        ctl_queue(k, (const uint8_t *)"", 0);      /* our soft reset, message id 0 */
    }
    o->t_last_rx = o->now;
    for (int x = 0; x < na; x++) {
        uint32_t id = get32(acks + 4 * x);
        for (int s = 0; s < CTL_POOL; s++) if (k->pool[s].used && k->pool[s].tries && k->pool[s].id == id) k->pool[s].used = 0;
    }
    if (op == P_ACK_V1) { ctl_pump_tx(k); return; }
    if (n < i + 4) return;
    uint32_t id = get32(d + i);
    i += 4;
    size_t pl = n - i;
    if (id == k->rx_next) {
        if (op == P_CONTROL_V1) {
            if (k->rxs_len + pl > RXS_SIZE) return;                                     /* no room: peer resends */
            memcpy(k->rxs + k->rxs_len, d + i, pl);
            k->rxs_len += pl;
        } else if (op == P_CONTROL_HARD_RESET_SERVER_V2) {
            if (k->st == OVPN_RESET) { k->st = OVPN_TLS; o->state = OVPN_TLS; }
        }
        k->rx_next++;
        queue_ack(k, id);
    } else if ((int32_t)(id - k->rx_next) < 0) {
        queue_ack(k, id);                                                               /* ours was lost, ack again */
    }
    ctl_pump_tx(k);
    tls_pump(k);
    flush_acks(k);
}

/* the new key takes over once it is a second old and everything we said on it is acknowledged */
static void keys_housekeeping(ovpn_t *o)
{
    for (int x = 0; x < 2; x++) {
        ks_t *k = o->slot[x];
        if (!k) continue;
        if (x != o->cur && o->cur >= 0 && o->slot[o->cur] && k->seq > o->slot[o->cur]->seq && k->st == OVPN_UP && !k->first && inflight(k) == 0 &&
            (uint32_t)(o->now - k->t_keys) >= SWITCH_MS) {
            ks_t *old = o->slot[o->cur];
            if (old) old->t_retire = o->now + RETIRE_MS;
            o->cur = x;
            logf_(o, "VPN now sends with key %u", k->kid);
        } else if (x != o->cur && k->t_retire && (int32_t)(o->now - k->t_retire) >= 0) {
            slot_free(o, x);
        }
    }
}

/* ======================= public API ======================= */

ovpn_t *ovpn_new(const ovpn_conf_t *c, const ovpn_ops_t *ops)
{
    ovpn_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->cf = *c;
    o->ops = *ops;
    o->cur = -1;
    o->keylen = !strcmp(c->cipher, "AES-256-GCM") ? 32 : 16;

    uint8_t sk[256];
    if (parse_static_key(c->tlsauth, sk) || c->key_direction < 0) { snprintf(o->err, sizeof o->err, "bad tls-auth key"); o->state = OVPN_FAILED; return o; }
    int d = c->key_direction;
    memcpy(o->ta_send, sk + (d ? 192 : 64), 32);
    memcpy(o->ta_recv, sk + (d ? 64 : 192), 32);
    o->ta = 1;
    memset(sk, 0, sizeof sk);

    mbedtls_ssl_config_init(&o->conf);
    mbedtls_x509_crt_init(&o->ca);
    mbedtls_x509_crt_init(&o->cert);
    mbedtls_pk_init(&o->key);
    int r;
    if ((r = mbedtls_x509_crt_parse(&o->ca, (const unsigned char *)c->ca, strlen(c->ca) + 1)) != 0 ||
        (r = mbedtls_x509_crt_parse(&o->cert, (const unsigned char *)c->cert, strlen(c->cert) + 1)) != 0) {
        snprintf(o->err, sizeof o->err, "certificate in the profile cannot be read (%d)", r);
        o->state = OVPN_FAILED;
        return o;
    }
    if ((r = mbedtls_pk_parse_key(&o->key, (const unsigned char *)c->key, strlen(c->key) + 1, NULL, 0, rnd_cb, o)) != 0) {
        snprintf(o->err, sizeof o->err, "private key in the profile cannot be read (%d)", r);
        o->state = OVPN_FAILED;
        return o;
    }
    mbedtls_ssl_config_defaults(&o->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_min_tls_version(&o->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&o->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&o->conf, rnd_cb, o);
    mbedtls_ssl_conf_authmode(&o->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&o->conf, &o->ca, NULL);
    mbedtls_ssl_conf_verify(&o->conf, verify_cb, o);
    if ((r = mbedtls_ssl_conf_own_cert(&o->conf, &o->cert, &o->key)) != 0) {
        snprintf(o->err, sizeof o->err, "TLS set-up failed (%d)", r);
        o->state = OVPN_FAILED;
        return o;
    }
    o->tls_ready = 1;
    return o;
}

void ovpn_free(ovpn_t *o)
{
    if (!o) return;
    slot_free(o, 0);
    slot_free(o, 1);
    mbedtls_ssl_config_free(&o->conf);
    mbedtls_x509_crt_free(&o->ca);
    mbedtls_x509_crt_free(&o->cert);
    mbedtls_pk_free(&o->key);
    memset(o, 0, sizeof *o);
    free(o);
}

void ovpn_start(ovpn_t *o, uint32_t now)
{
    if (o->state != OVPN_IDLE || !o->tls_ready) return;
    o->now = now;
    o->t_start = now;
    if (rnd(o, o->sid, 8)) { fail(o, "no random numbers"); return; }
    ks_t *k = slot_new(o, 0, 0, 1);
    if (!k) { fail(o, "out of memory"); return; }
    o->state = OVPN_RESET;
    ctl_queue(k, (const uint8_t *)"", 0);         /* the hard reset */
    ctl_pump_tx(k);
}

void ovpn_input(ovpn_t *o, const uint8_t *d, size_t n, uint32_t now)
{
    o->now = now;
    if (o->state < OVPN_RESET || o->state == OVPN_FAILED || n < 1) return;
    int op = d[0] >> 3;
    if (op == P_DATA_V1) { if (o->state == OVPN_UP) recv_data(o, d, n); return; }
    if (op == P_CONTROL_V1 || op == P_ACK_V1 || op == P_CONTROL_HARD_RESET_SERVER_V2 || op == P_CONTROL_SOFT_RESET_V1)
        recv_control(o, op, d, n);
}

void ovpn_tick(ovpn_t *o, uint32_t now)
{
    o->now = now;
    if (o->state < OVPN_RESET || o->state == OVPN_FAILED) return;
    if (o->state < OVPN_UP && (uint32_t)(now - o->t_start) > HANDSHAKE_MS) { fail(o, "no tunnel after %d s", HANDSHAKE_MS / 1000); return; }
    for (int x = 0; x < 2; x++) {
        ks_t *k = o->slot[x];
        if (!k) continue;
        ctl_retransmit(k);
        if (o->state == OVPN_FAILED) return;
        ctl_pump_tx(k);
        tls_pump(k);
        if (o->state == OVPN_FAILED) return;
        if (k->st == OVPN_PUSH && (uint32_t)(now - k->t_push) >= 5000) {
            static const char pr[] = "PUSH_REQUEST";
            k->t_push = now;
            tls_put(k, pr, sizeof pr);
            tls_pump(k);
        }
        flush_acks(k);
    }
    keys_housekeeping(o);
    if (o->state == OVPN_UP) {
        if ((uint32_t)(now - o->t_last_tx) >= (uint32_t)o->cf.ping_s * 1000) send_data(o, PING_MAGIC, 16);
        if (o->cf.restart_s && (uint32_t)(now - o->t_last_rx) >= (uint32_t)o->cf.restart_s * 1000) fail(o, "server silent for %d s", o->cf.restart_s);
    }
}

int ovpn_send_ip(ovpn_t *o, const uint8_t *ip, size_t n, uint32_t now)
{
    o->now = now;
    if (o->state != OVPN_UP) return -1;
    return send_data(o, ip, n);
}

void ovpn_stop(ovpn_t *o, uint32_t now)
{
    o->now = now;
    if (o->state == OVPN_UP && o->cf.explicit_exit && o->cur >= 0 && o->slot[o->cur]) {
        static const char ex[] = "EXIT";
        ks_t *k = o->slot[o->cur];
        tls_put(k, ex, sizeof ex);
        tls_pump(k);
    }
    if (o->state != OVPN_FAILED) { o->state = OVPN_FAILED; snprintf(o->err, sizeof o->err, "stopped"); }
}

ovpn_state_t ovpn_state(const ovpn_t *o) { return o->state; }
const char *ovpn_error(const ovpn_t *o) { return o->err; }
const ovpn_info_t *ovpn_info(const ovpn_t *o) { return &o->info; }
const ovpn_stats_t *ovpn_stats(const ovpn_t *o) { return &o->st; }

const char *ovpn_state_name(ovpn_state_t s)
{
    switch (s) {
    case OVPN_IDLE: return "idle";
    case OVPN_RESET: return "waiting for the server";
    case OVPN_TLS: return "TLS handshake";
    case OVPN_KEYS: return "key exchange";
    case OVPN_PUSH: return "asking for settings";
    case OVPN_UP: return "up";
    default: return "failed";
    }
}
