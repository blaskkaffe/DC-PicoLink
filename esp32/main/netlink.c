/*
 * Netlink tunnel through the USB modem, as DreamPi 2.x does it with eaudunord's netlink.py
 * (../../firmware/netlink_model.h has the dial strings and formats, host-tested):
 *
 *   direct dial    "0", "00", "#0xx#" wait for a call; "#a*b*c*d#", "#aaabbbcccddd#" call an IP;
 *                  "#xxx#" asks the matchmaking server (saturn.dreampipe.net) for an opponent.
 *                  Handshake over TCP 65432 ("readyip" / "g2gip"), then the modem bytes go over
 *                  UDP (20001 waiting, 20002 calling) in netlink.py's packet format.
 *   game servers   dial codes from netlink_config.ini (downloaded from GitHub at start):
 *                  TCP relay, "standard" (with a shared-secret login) or "transparent".
 *   XBAND          the XBAND server numbers relay to xbserver.retrocomputing.network; for 15
 *                  minutes after that the box takes XBAND calls on TCP 65433, and "#a*b*c*d*1#"
 *                  calls another tunnel. (netlink.py also rings a VoIP phone adapter by SIP
 *                  on such a call; that part is not here.)
 *
 * The waiting side needs TCP 65432/65433 and UDP 20001/20002 open to this box from the
 * Internet: port forwarding (portfwd.c) asks the router for them by UPnP, or use the VPN.
 * Everything here runs in the USB modem's task (usbmodem.c), one call at a time.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "netlink_model.h"
#include "app.h"

static nl_servers_t servers;
/* work buffers, only while a call runs (netlink_run) */
typedef struct { nl_tx_t tx; uint8_t dg[NL_KEEP * (NL_CHUNK_MAX + 40)]; uint8_t in[1500]; uint8_t m[NL_CHUNK_MAX]; } xbuf_t;
static xbuf_t *xb;
static bool matching = true;                     /* "999" switches matchmaking off, "888" on */
static int udp = -1;
static int64_t xband_since = -1;                 /* when the XBAND server call ended (listening for 15 min) */
static int xband_ls = -1;
static struct { uint32_t calls, connected, failed, to_net, to_dc, recovered; int ping_ms; char last[48]; char peer[40]; } st = { .ping_ms = -1 };

#define say say_line
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void pause_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

const void *netlink_servers_raw(void) { return &servers; }

const char *netlink_mode_name(int m) {
    switch (m) {
    case NL_WAIT: return "Netlink: waiting for a direct call";
    case NL_CALL: return "Netlink: direct call";
    case NL_SERVER: return "Netlink game server";
    case NL_XBAND_SERVER: return "XBAND server";
    case NL_XBAND_CALL: return "XBAND call to another tunnel";
    case NL_CAPCOM: return "Capcom matching";
    default: return "?";
    }
}

static bool resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints = { .ai_family = AF_INET }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

static int tcp_connect(uint32_t ip, uint16_t port, int timeout_s) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_s };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static int tcp_listen(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 2) != 0) { close(s); return -1; }
    return s;
}

/* readable within ms? */
static bool wait_in(int s, int ms) {
    fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    return select(s + 1, &rs, NULL, NULL, &tv) > 0;
}

static void close_udp(void) { if (udp >= 0) { close(udp); udp = -1; } }
static bool open_udp(uint16_t port) {
    if (udp >= 0) return true;
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) return false;
    int tos = 184, one = 1;
    setsockopt(udp, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (bind(udp, (struct sockaddr *)&a, sizeof a) != 0) { close_udp(); return false; }
    return true;
}

/* our address and port as the Internet sees them (STUN from the data socket, like getWanIP) */
static bool wan(uint16_t port, char ip12[13], uint16_t *ext) {
    if (!open_udp(port)) return false;
    uint32_t sip;
    if (!resolve(NL_STUN_HOST, &sip)) return false;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(NL_STUN_PORT), .sin_addr.s_addr = sip };
    uint8_t tid[12], q[20], a[256];
    esp_fill_random(tid, sizeof tid);
    for (int t = 0; t < 2; t++) {
        sendto(udp, q, nl_stun_request(q, tid), 0, (struct sockaddr *)&to, sizeof to);
        int64_t t0 = now_ms();
        while (now_ms() - t0 < 2000) {
            if (!wait_in(udp, 200)) continue;
            int n = recv(udp, a, sizeof a, 0);
            uint32_t ip; uint16_t p;
            if (n > 0 && nl_stun_answer(a, (size_t)n, tid, &ip, &p)) { nl_ip_to_12(ip, ip12); *ext = p; return true; }
        }
    }
    return false;
}

/* ---- the matchmaking server (HTTPS GET, small JSON answer) ---- */
static char hbuf[512]; static size_t hlen;
static esp_err_t hev(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && hlen + (size_t)e->data_len < sizeof hbuf) { memcpy(hbuf + hlen, e->data, (size_t)e->data_len); hlen += (size_t)e->data_len; }
    return ESP_OK;
}
static bool match_call(const char *action, const char *game, const char *ip12, uint16_t port) {
    char url[256];
    if (port) snprintf(url, sizeof url, "https://" NL_MATCH_HOST NL_MATCH_PATH "?action=%s&gameID=%s&client_ip=%s&port=%u&key=" NL_MATCH_KEY, action, game, ip12, (unsigned)port);
    else snprintf(url, sizeof url, "https://" NL_MATCH_HOST NL_MATCH_PATH "?action=%s&gameID=%s&client_ip=%s&key=" NL_MATCH_KEY, action, game, ip12);
    hlen = 0;
    int code = https_req(url, hev, 8000, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36", NULL, NULL);
    bool ok = code > 0 && code < 400;
    hbuf[hlen] = 0;
    return ok;
}

/* ---- the data exchange (netlink_exchange: listener + sender) ---- */
static void exchange(bool waiting, uint32_t peer, uint16_t peer_port) {
    if (!open_udp(waiting ? NL_UDP_WAIT : NL_UDP_CALL)) { say("Netlink: no UDP socket"); return; }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(peer_port), .sin_addr.s_addr = peer };
    nl_tx_t *tx = &xb->tx; uint8_t *dg = xb->dg, *in = xb->in, *m = xb->m;
    const size_t dgcap = sizeof xb->dg, incap = sizeof xb->in, mcap = sizeof xb->m;
    memset(tx, 0, sizeof *tx);
    nl_rx_t rx = { 0 };
    sendto(udp, "OPEN_SHIRO", 10, 0, (struct sockaddr *)&to, sizeof to);
    st.connected++;
    snprintf(st.peer, sizeof st.peer, "%s:%u", inet_ntoa(to.sin_addr), (unsigned)peer_port);
    say("Netlink: exchanging data with %s", st.peer);
    int64_t last_ping = 0, ping_sent = 0, last_report = now_ms();
    bool established = false;
    char tail[16] = ""; size_t tn = 0;
    for (;;) {
        if (now_ms() - last_ping > 1000) { sendto(udp, "PING_SHIRO", 10, 0, (struct sockaddr *)&to, sizeof to); last_ping = ping_sent = now_ms(); }
        while (wait_in(udp, 0)) {
            struct sockaddr_in fr; socklen_t fl = sizeof fr;
            int n = recvfrom(udp, in, incap, 0, (struct sockaddr *)&fr, &fl);
            if (n <= 0) break;
            if (fr.sin_addr.s_addr != peer) continue;      /* only the other player (its port may change behind NAT): anything else sent to this port would go straight into the Dreamcast's modem */
            if (n == 10 && !memcmp(in, "PING_SHIRO", 10)) { sendto(udp, "PONG_SHIRO", 10, 0, (struct sockaddr *)&to, sizeof to); continue; }
            if (n == 10 && !memcmp(in, "PONG_SHIRO", 10)) {
                if (!established) { established = true; say("Netlink: connection established"); }
                int p = (int)(now_ms() - ping_sent);
                if (p <= 500) st.ping_ms = p;
                continue;
            }
            if (n == 10 && !memcmp(in, "OPEN_SHIRO", 10)) continue;
            const uint8_t *pl; size_t pn; bool more;
            do {
                if (!nl_rx_take(&rx, in, (size_t)n, &pl, &pn, &more)) break;
                if (pn) { usbmodem_write(pl, pn); st.to_dc += (uint32_t)pn; }
            } while (more);
        }
        int k = um_read(m, mcap, 2);
        if (k < 0) { say("Netlink: the call ended"); break; }
        if (k > 0) {
            for (int i = 0; i < k; i++) {                      /* NO CARRIER in the stream (sender) */
                if (tn == sizeof tail - 1) { memmove(tail, tail + 1, --tn); }
                tail[tn++] = (char)m[i]; tail[tn] = 0;
            }
            if (strstr(tail, "NO CARRIER")) { say("Netlink: NO CARRIER"); break; }
            size_t dn = nl_tx_pack(tx, m, (size_t)k, dg, dgcap);
            for (int t = 0; t < 2; t++) sendto(udp, dg, dn, 0, (struct sockaddr *)&to, sizeof to);   /* twice, like netlink.py */
            st.to_net += (uint32_t)k;
        }
        if (now_ms() - last_report > 30000) {
            last_report = now_ms();
            say("Netlink: ping %d ms; %lu bytes out, %lu in, %lu recovered", st.ping_ms, (unsigned long)st.to_net, (unsigned long)st.to_dc, (unsigned long)rx.recovered);
        }
    }
    st.recovered += rx.recovered;
    close_udp();
}

/* ---- direct dial (do_netlink + initConnection) ---- */
static void send_ring(void) { usbmodem_write("RING\r\nCONNECT\r\n", 15); }

static void direct(const nl_dial_t *d) {
    close_udp();
    um_stop_tone();
    um_cmd("AT%E0\\V1", 3000);
    um_cmd("AT%C0\\N3", 3000);
    um_cmd("AT&C1&D2", 3000);
    if (!um_connect(false, 60000)) { say("Netlink: the modems did not connect"); st.failed++; return; }
    bool waiting = d->mode == NL_WAIT;
    uint32_t peer = 0; uint16_t pport = 0;
    char ip12[13] = ""; uint16_t ext = 0;
    if (waiting) {
        bool mm = strlen(d->target) == 3 && matching;         /* "#0xx#": matchmaking for game xx */
        const char *game = d->target + 1;
        int ls = tcp_listen(NL_TCP_PORT);
        if (ls < 0) { say("Netlink: cannot listen on TCP %d", NL_TCP_PORT); st.failed++; return; }
        say("Netlink: waiting for a call on TCP %d%s", NL_TCP_PORT, mm ? " and at the matchmaking server" : "");
        bool registered = false;
        int64_t t0 = now_ms(), last_stun = -10000, last_poll = 0;
        while (now_ms() - t0 < 120000 && !peer) {
            if (!um_carrier()) { say("Netlink: the Dreamcast hung up"); break; }
            if (wait_in(ls, 50)) {
                struct sockaddr_in a; socklen_t al = sizeof a;
                int c = accept(ls, (struct sockaddr *)&a, &al);
                if (c >= 0) {
                    struct timeval tv = { .tv_sec = 20 };
                    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                    char b[32]; int n = recv(c, b, sizeof b - 1, 0);
                    if (n == 7 && !memcmp(b, "readyip", 7)) {
                        send(c, "g2gip", 5, 0);
                        say("Netlink: call from %s", inet_ntoa(a.sin_addr));
                        send_ring();
                        peer = a.sin_addr.s_addr; pport = NL_UDP_CALL;
                    }
                    close(c);
                }
            }
            if (mm && !peer) {
                if (now_ms() - last_stun > 5000) {                  /* keeps the router's mapping open */
                    last_stun = now_ms();
                    char i2[13]; uint16_t e2;
                    if (wan(NL_UDP_WAIT, i2, &e2)) { memcpy(ip12, i2, 13); ext = e2; }
                }
                if (ip12[0] && now_ms() - last_poll > 1000) {
                    last_poll = now_ms();
                    if (!registered) {
                        if (match_call("wait", game, ip12, ext)) { registered = true; say("Netlink: registered at the matchmaking server for game %s", game); }
                    } else if (match_call("status", game, ip12, 0)) {
                        char s[32], ip[20]; uint16_t p;
                        if (nl_json_str(hbuf, "status", s, sizeof s) && !strcmp(s, "matched") && nl_json_opponent(hbuf, ip, sizeof ip, &p)) {
                            say("Netlink: matched with %s:%u", ip, (unsigned)p);
                            send_ring();
                            inet_aton(ip, (struct in_addr *)&peer); pport = p;
                        }
                    }
                }
            }
        }
        close(ls);
        if (registered && !peer) match_call("timeout", game, ip12, 0);
        if (!peer && now_ms() - t0 >= 120000) say("Netlink: nobody called within 2 minutes");
    } else if (strchr(d->target, '.')) {                       /* call an address */
        uint32_t ip;
        if (!inet_aton(d->target, (struct in_addr *)&ip)) { st.failed++; return; }
        say("Netlink: calling %s", d->target);
        int s = tcp_connect(ip, NL_TCP_PORT, 20);
        if (s < 0) say("Netlink: could not reach %s (port %d not open, or no answer)", d->target, NL_TCP_PORT);
        else {
            send(s, "readyip", 7, 0);
            char b[16]; int n = recv(s, b, sizeof b, 0);
            if (n == 5 && !memcmp(b, "g2gip", 5)) { peer = ip; pport = NL_UDP_WAIT; }
            close(s);
        }
    } else if (!strcmp(d->target, "999")) { matching = false; say("Netlink: matchmaking off"); }
    else if (!strcmp(d->target, "888")) { matching = true; say("Netlink: matchmaking on"); }
    else {                                                     /* ask the matchmaking server */
        const char *game = d->target + (strlen(d->target) > 2 ? strlen(d->target) - 2 : 0);
        if (wan(NL_UDP_CALL, ip12, &ext) && match_call("match", game, ip12, ext)) {
            char s[32], ip[20]; uint16_t p;
            if (nl_json_str(hbuf, "status", s, sizeof s)) say("Netlink: matchmaking: %s", s);
            if (nl_json_opponent(hbuf, ip, sizeof ip, &p)) { inet_aton(ip, (struct in_addr *)&peer); pport = p; }
        } else say("Netlink: the matchmaking server could not be reached");
    }
    if (!peer) { st.failed++; return; }
    exchange(waiting, peer, pport);
}

/* ---- game servers (netlink_server) ---- */
static void game_server(const nl_dial_t *d) {
    const nl_server_t *sv = nl_server_find(&servers, d->target);
    if (!sv) { say("Netlink: no server for %s", d->target); return; }
    const char *label = sv->name[0] ? sv->name : sv->host;
    um_stop_tone();
    if (!um_connect(false, 60000)) { say("%s: the modems did not connect", label); st.failed++; return; }
    if (!sv->transparent) {                                    /* standard: drop what the modem said after CONNECT */
        uint8_t junk[64]; pause_ms(200);
        while (um_read(junk, sizeof junk, 50) > 0) { }
    }
    uint32_t ip;
    if (!resolve(sv->host, &ip)) { say("%s: cannot find %s", label, sv->host); st.failed++; return; }
    int s = tcp_connect(ip, sv->port, sv->transparent ? 5 : 10);
    if (s < 0) { say("%s: cannot connect to %s:%u", label, sv->host, (unsigned)sv->port); st.failed++; return; }
    if (!sv->transparent && sv->secret[0]) {
        uint8_t au[96]; size_t an = nl_auth(sv->magic, sv->secret, au, sizeof au);
        struct timeval tv = { .tv_sec = (int)sv->auth_timeout, .tv_usec = (int)((sv->auth_timeout - (int)sv->auth_timeout) * 1e6f) };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t r = 0;
        if (send(s, au, an, 0) != (int)an || recv(s, &r, 1, 0) != 1 || r != 1) { say("%s: the server refused the login", label); close(s); st.failed++; return; }
    }
    st.connected++;
    snprintf(st.peer, sizeof st.peer, "%.39s", label);
    say("%s: connected to %s:%u", label, sv->host, (unsigned)sv->port);
    if (sv->dcnow) dcnow_set_online(3, true);
    uint8_t *a = xb->m, *b = xb->dg;                           /* 1024, and >= 2 * 1024 + 1 */
    const size_t acap = sizeof xb->m;
    bool pf_dc = false, pf_srv = false;
    char tail[16] = ""; size_t tn = 0;
    for (;;) {
        int k = um_read(a, acap, 5);
        if (k < 0) { say("%s: the call ended", label); break; }
        if (k > 0) {
            for (int i = 0; i < k; i++) { if (tn == sizeof tail - 1) memmove(tail, tail + 1, --tn); tail[tn++] = (char)a[i]; tail[tn] = 0; }
            if (strstr(tail, "NO CARRIER")) { say("%s: NO CARRIER", label); break; }
            const uint8_t *o = a; size_t on = (size_t)k;
            if (sv->fix_ppp) { on = nl_ppp_fix(a, (size_t)k, &pf_dc, b); o = b; }
            if (send(s, o, on, 0) < 0) { say("%s: connection lost", label); break; }
            st.to_net += (uint32_t)on;
        }
        if (wait_in(s, 0)) {
            int n = recv(s, a, acap, 0);
            if (n <= 0) { say("%s: the server ended the call", label); break; }
            const uint8_t *o = a; size_t on = (size_t)n;
            if (sv->fix_ppp) { on = nl_ppp_fix(a, (size_t)n, &pf_srv, b); o = b; }
            usbmodem_write(o, on);
            st.to_dc += (uint32_t)on;
        }
    }
    close(s);
    if (sv->dcnow) dcnow_set_online(3, false);
}

/* ---- XBAND ---- */
static void hwid(char *out) {                     /* netlink.py getserial(): 16 characters, ours from the MAC */
    uint8_t mac[6], h[32];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mbedtls_sha256(mac, 6, h, 0);
    for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static void xband_server(void) {
    um_stop_tone();
    if (!um_connect(false, 60000)) { say("XBAND: the modems did not connect"); st.failed++; return; }
    uint32_t ip;
    if (!resolve(NL_XBAND_HOST, &ip)) { say("XBAND: cannot find %s", NL_XBAND_HOST); st.failed++; return; }
    int s = tcp_connect(ip, NL_XBAND_SRV_PORT, 15);
    if (s < 0) { say("XBAND: cannot connect to the server"); st.failed++; return; }
    char id[17]; hwid(id);
    char hello[40]; int hn = snprintf(hello, sizeof hello, "///////PI-%s\n", id);
    send(s, hello, (size_t)hn, 0);
    st.connected++;
    snprintf(st.peer, sizeof st.peer, "XBAND server");
    say("XBAND: connected to the server");
    uint8_t *a = xb->m, *line = xb->in;
    const size_t acap = sizeof xb->m, lcap = sizeof xb->in;
    size_t ln = 0;
    int64_t no_cd = 0;
    for (;;) {
        if (wait_in(s, 0)) {
            int n = recv(s, a, acap, 0);
            if (n <= 0) { say("XBAND: the server connection dropped"); break; }
            usbmodem_write(a, (size_t)n); st.to_dc += (uint32_t)n;
        }
        int k = um_read(a, acap, 20);
        if (k < 0) {                                           /* carrier gone: netlink.py allows 2 s */
            if (um_gone()) break;
            if (!no_cd) no_cd = now_ms();
            if (now_ms() - no_cd > 2000) { say("XBAND: the console hung up"); break; }
            continue;
        }
        no_cd = 0;
        for (int i = 0; i < k; i++) {                          /* sent a line at a time: ends with DLE ETX */
            if (ln < lcap) line[ln++] = a[i];
            if (ln >= 2 && line[ln - 2] == 0x10 && line[ln - 1] == 0x03) { send(s, line, ln, 0); st.to_net += (uint32_t)ln; ln = 0; }
            else if (ln == lcap) { send(s, line, ln, 0); ln = 0; }
        }
    }
    close(s);
    xband_since = now_ms();                                    /* now listen for XBAND calls for a while */
    say("XBAND: back to the dial tone; taking XBAND calls on TCP %d for 15 minutes", NL_XBAND_PORT);
}

static void init_xband(void) {
    um_stop_tone();
    um_cmd("AT%E0", 3000);
    um_cmd("AT\\V1%C0", 3000);
    um_cmd("AT+MS=V22b", 3000);
}

/* write 0xff, then wait for the console's 0x01 (skipping 0xff echoes) */
static bool xband_ping(int ms) {
    usbmodem_write("\xff", 1);
    uint8_t b[64]; int64_t t0 = now_ms();
    while (now_ms() - t0 < ms) {
        int k = um_read(b, sizeof b, 50);
        if (k < 0) return false;
        for (int i = 0; i < k; i++) if (b[i] == 0x01) return true;
    }
    return false;
}

/* an XBAND call from another tunnel (xband_listen) */
static bool xband_incoming(int c, uint32_t from) {
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int64_t t0 = now_ms();
    char b[32];
    bool answered = false;
    while (now_ms() - t0 < 120000) {
        int n = recv(c, b, sizeof b - 1, 0);
        if (n == 0) break;
        if (n < 0) continue;
        b[n] = 0;
        if (!strcmp(b, "RESET")) { init_xband(); send(c, "ACK RESET", 9, 0); }
        else if (!strcmp(b, "RING")) {
            say("XBAND: ring from %s", inet_ntoa(*(struct in_addr *)&from));
            send(c, "ANSWERING", 9, 0);
            pause_ms(6000);
            if (!um_connect(true, 30000)) { say("XBAND: could not answer"); return false; }
            answered = true;
        } else if (!strcmp(b, "PING")) {
            send(c, "ACK PING", 8, 0);
            if (!xband_ping(10000) || !um_carrier()) { say("XBAND: the call dropped"); return false; }
            send(c, "RESPONSE", 8, 0);
        } else if (!strcmp(b, "RESPONSE")) {
            usbmodem_write("\x01", 1);
            if (answered && um_carrier()) { exchange(true, from, NL_UDP_CALL); return true; }
        }
    }
    return answered;
}

/* during the dial tone: an XBAND call from the network? (xband_match) */
bool netlink_idle_poll(void) {
    if (xband_since < 0) return false;
    int64_t t = now_ms() - xband_since;
    if (t > 900000) { if (xband_ls >= 0) { close(xband_ls); xband_ls = -1; } xband_since = -1; return false; }
    if (t < 15000) return false;
    if (xband_ls < 0) xband_ls = tcp_listen(NL_XBAND_PORT);
    if (xband_ls < 0 || !wait_in(xband_ls, 0)) return false;
    struct sockaddr_in a; socklen_t al = sizeof a;
    int c = accept(xband_ls, (struct sockaddr *)&a, &al);
    if (c < 0) return false;
    st.calls++;
    say("XBAND: incoming call from %s", inet_ntoa(a.sin_addr));
    xb = malloc(sizeof *xb);
    if (xb) xband_incoming(c, a.sin_addr.s_addr);
    free(xb); xb = NULL;
    close_udp();
    close(c);
    close(xband_ls); xband_ls = -1; xband_since = -1;
    um_hangup();
    return true;
}

/* call another tunnel's console (ring_phone; no SIP ring) */
static void xband_call(const nl_dial_t *d) {
    uint32_t ip;
    if (!inet_aton(d->target, (struct in_addr *)&ip)) return;
    init_xband();
    int s = tcp_connect(ip, NL_XBAND_PORT, 15);
    if (s < 0) { say("XBAND: could not reach %s", d->target); st.failed++; return; }
    send(s, "RESET", 5, 0);
    int64_t t0 = now_ms();
    bool ok = false;
    char b[32];
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (now_ms() - t0 < 90000) {
        int n = recv(s, b, sizeof b - 1, 0);
        if (n == 0) break;
        if (n < 0) continue;
        b[n] = 0;
        if (!strcmp(b, "ACK RESET")) send(s, "RING", 4, 0);
        else if (!strcmp(b, "ANSWERING")) {
            if (!um_connect(false, 60000)) break;
            send(s, "PING", 4, 0);
        } else if (!strcmp(b, "ACK PING")) {
            if (!xband_ping(10000) || !um_carrier()) break;
            send(s, "RESPONSE", 8, 0);
        } else if (!strcmp(b, "RESPONSE")) { usbmodem_write("\x01", 1); ok = true; break; }
    }
    close(s);
    if (!ok) { say("XBAND: the call to %s did not go through", d->target); st.failed++; return; }
    exchange(false, ip, NL_UDP_WAIT);
}

void netlink_run(const void *dv) {
    const nl_dial_t *d = dv;
    xb = malloc(sizeof *xb);
    if (!xb) { say("Netlink: out of memory"); return; }
    st.calls++;
    snprintf(st.last, sizeof st.last, "%s", d->target);
    st.peer[0] = 0;
    switch (d->mode) {
    case NL_WAIT: case NL_CALL: direct(d); break;
    case NL_SERVER: game_server(d); break;
    case NL_XBAND_SERVER: xband_server(); break;
    case NL_XBAND_CALL: xband_call(d); break;
    default: break;
    }
    close_udp();
    free(xb); xb = NULL;
}

size_t netlink_info_json(char *o, size_t cap) {
    return (size_t)snprintf(o, cap, "{\"servers\":%d,\"calls\":%lu,\"connected\":%lu,\"failed\":%lu,\"to_net\":%lu,\"to_dc\":%lu,\"recovered\":%lu,"
        "\"ping_ms\":%d,\"last\":\"%s\",\"peer\":\"%s\",\"matching\":%s,\"xband_listening\":%s}",
        servers.n, (unsigned long)st.calls, (unsigned long)st.connected, (unsigned long)st.failed, (unsigned long)st.to_net,
        (unsigned long)st.to_dc, (unsigned long)st.recovered, st.ping_ms, st.last, st.peer, matching ? "true" : "false",
        xband_since >= 0 ? "true" : "false");
}

/* ---- netlink_config.ini: fetched once when the Internet is up (read line by line) ---- */
static nl_servers_t *nsv; static int ncur; static char ll[200]; static size_t lln;
static esp_err_t cfg_ev(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA || !nsv) return ESP_OK;
    const char *d = e->data;
    for (int i = 0; i < e->data_len; i++) {
        if (d[i] == '\n') { ll[lln < sizeof ll ? lln : sizeof ll - 1] = 0; nl_servers_line(nsv, &ncur, ll); lln = 0; }
        else if (lln < sizeof ll - 1) ll[lln++] = d[i];
    }
    return ESP_OK;
}

static void cfg_task(void *arg) {
    (void)arg;
    for (int tries = 0; tries < 20; tries++) {
        while (!inet_up()) pause_ms(2000);
        pause_ms(tries ? 30000 : 12000);
        nsv = malloc(sizeof *nsv);
        if (!nsv) continue;
        nl_servers_defaults(nsv); ncur = -1; lln = 0;
        bool ok = https_req(NL_CONFIG_URL, cfg_ev, 10000, NULL, NULL, NULL) == 200;
        if (ok) {
            if (lln) { ll[lln] = 0; nl_servers_line(nsv, &ncur, ll); }
            servers = *nsv;                                     /* (calls only read it from the modem task) */
            say("Netlink: %d game server dial codes from netlink_config.ini", servers.n);
            free(nsv); nsv = NULL;
            break;
        }
        free(nsv); nsv = NULL;
    }
    vTaskDelete(NULL);
}

void netlink_start(void) {
    nl_servers_defaults(&servers);
    xTaskCreate(cfg_task, "nl_cfg", 6144, NULL, 3, NULL);
}
