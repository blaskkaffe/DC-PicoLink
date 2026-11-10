/* OpenVPN client (UDP, tun, TLS 1.2, tls-auth, key-method 2, AES-GCM data channel).
 *
 * Only what the DreamPi 2.1 profile uses: cipher AES-128-GCM, auth SHA256, tls-auth,
 * client certificate, no compression, no user/password. Needs mbedTLS and nothing else:
 * no sockets, no tasks, no clock of its own, so the same file runs on the ESP32 and
 * in the PC test (esp32/tests/ovpn). The caller serializes all calls.
 */
#ifndef OVPN_H
#define OVPN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OVPN_IDLE = 0,
    OVPN_RESET,     /* hard reset sent, waiting for the server */
    OVPN_TLS,       /* TLS handshake */
    OVPN_KEYS,      /* TLS done, key exchange */
    OVPN_PUSH,      /* waiting for PUSH_REPLY */
    OVPN_UP,        /* tunnel up */
    OVPN_FAILED,
} ovpn_state_t;

/* What the caller provides. */
typedef struct {
    void *ctx;
    int  (*send)(void *ctx, const uint8_t *d, size_t n);      /* one UDP datagram to the server */
    void (*ip_in)(void *ctx, const uint8_t *ip, size_t n);    /* decrypted IPv4 packet */
    void (*log)(void *ctx, const char *msg);                  /* optional */
    int  (*random)(void *ctx, uint8_t *buf, size_t n);        /* 0 = ok */
    uint32_t (*unix_time)(void *ctx);                         /* optional: seconds; 0 if unknown */
} ovpn_ops_t;

/* Parsed `.conf` (the inline-block style of DreamPi's client configs). Free with ovpn_conf_free. */
typedef struct {
    char *host;                 /* first `remote` */
    int   port;
    char *ca, *cert, *key;      /* PEM, NUL-terminated */
    char *tlsauth;              /* OpenVPN Static key V1 text */
    int   key_direction;        /* 0, 1, -1 = not given */
    int   ping_s;               /* `ping`, default 10 */
    int   restart_s;            /* `ping-restart`, default 60 */
    int   explicit_exit;        /* explicit-exit-notify */
    char  cipher[24];           /* "AES-128-GCM" unless the file says otherwise */
    int   udp;                  /* proto udp */
    int   tun;                  /* dev tun* */
} ovpn_conf_t;

int  ovpn_conf_parse(const char *text, ovpn_conf_t *out);   /* 0 = ok, <0 = what is missing */
void ovpn_conf_free(ovpn_conf_t *c);

typedef struct ovpn ovpn_t;

/* The strings in `c` must stay valid until ovpn_free. */
ovpn_t *ovpn_new(const ovpn_conf_t *c, const ovpn_ops_t *ops);
void    ovpn_free(ovpn_t *o);

void    ovpn_start(ovpn_t *o, uint32_t now_ms);                   /* sends the hard reset */
void    ovpn_input(ovpn_t *o, const uint8_t *d, size_t n, uint32_t now_ms);
void    ovpn_tick(ovpn_t *o, uint32_t now_ms);                    /* call every ~100 ms */
int     ovpn_send_ip(ovpn_t *o, const uint8_t *ip, size_t n, uint32_t now_ms);   /* 0 = sent */
void    ovpn_stop(ovpn_t *o, uint32_t now_ms);                    /* tells the server, then FAILED */

ovpn_state_t ovpn_state(const ovpn_t *o);
const char  *ovpn_state_name(ovpn_state_t s);
const char  *ovpn_error(const ovpn_t *o);        /* why FAILED, or "" */

/* After PUSH_REPLY (addresses in host order, a.b.c.d = 0xAABBCCDD). */
typedef struct {
    uint32_t ip, mask, peer, gateway;
    uint32_t dns[2];
    int      ndns;
    struct { uint32_t net, mask, gw; } route[8];
    int      nroute;
    int      redirect_gateway;
    int      ping_s, restart_s;      /* pushed, 0 = none */
    int      mtu;                    /* tun-mtu used for the tunnel, 1500 default */
    char     cipher[24];
} ovpn_info_t;

const ovpn_info_t *ovpn_info(const ovpn_t *o);

/* counters */
typedef struct { uint32_t tx_pkts, rx_pkts, tx_bytes, rx_bytes, bad_hmac, bad_data, ctl_resend; } ovpn_stats_t;
const ovpn_stats_t *ovpn_stats(const ovpn_t *o);

#ifdef __cplusplus
}
#endif
#endif
