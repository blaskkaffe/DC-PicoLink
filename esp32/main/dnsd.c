/*
 * Small DNS server for the Dreamcast (what dnsmasq does on a DreamPi). Listens on the PPP
 * address while a call is up and on the Ethernet side's address (dc_eth.c) while that is up. Rules and parsing: ../../firmware/dns_model.h.
 *   - names with an "address=/name/ip" line are answered here
 *   - plain names and private reverse lookups are refused (domain-needed, bogus-priv)
 *   - everything else goes to the revival servers (and any server= lines of the downloaded file)
 * Every A-query is noted for DCNow!. The downloaded file (dreamcast.online/dreampi/dreampi_dns.conf)
 * is fetched when Wi-Fi is up and renewed every six hours.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "dns_model.h"
#include "app.h"

static const char *TAG = "dnsd";
#define DNS_FILE_URL "https://dreamcast.online/dreampi/dreampi_dns.conf"
#define PENDING 8
#define PKT_MAX 512
#define Q_MAX   288                              /* a waiting question (the Dreamcast's are < 100 bytes) */

/* the downloaded dreampi_dns.conf (8 KB table), only while there is one: DreamPi's emergency
   backup, which has been absent (404) for years. Without it: the two DreamPi DNS servers. */
static dns_conf_t *conf;
static uint32_t def_up[2];
static SemaphoreHandle_t conf_lk;
#define LISTEN_N 3                              /* 0: the PPP call, 1: the Ethernet side (E-4), 2: the wired port (BBA) */
static volatile bool online[LISTEN_N];
static volatile uint32_t bind_ip[LISTEN_N];
static struct { uint32_t q, fwd, ovr, nx, tcp; } cnt;
static volatile bool use_tcp;                    /* the router answers UDP DNS itself: ask over TCP */
static int *g_ls;                                /* the listening sockets (dns_task) */

/* One DNS question over TCP (RFC 1035 4.2.2): 2-byte length, then the message. Returns the
   answer's length, or -1. Blocks up to ms. Also used by pubip.c. */
int dns_tcp_ask(uint32_t server_host_order, const uint8_t *q, size_t qn, uint8_t *a, size_t cap, int ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(server_host_order) };
    int r = -1;
    uint8_t hdr[2] = { (uint8_t)(qn >> 8), (uint8_t)qn };
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && send(s, hdr, 2, 0) == 2 && send(s, q, qn, 0) == (int)qn) {
        uint8_t l[2]; int got = 0;
        while (got < 2) { int k = recv(s, l + got, (size_t)(2 - got), 0); if (k <= 0) break; got += k; }
        if (got == 2) {
            size_t want = (size_t)(l[0] << 8 | l[1]), have = 0;
            uint8_t sink[64];
            while (have < want) {                                        /* keep what fits, read the rest away */
                size_t room = have < cap ? cap - have : 0;
                int k = room ? recv(s, a + have, room < want - have ? room : want - have, 0)
                             : recv(s, sink, want - have < sizeof sink ? want - have : sizeof sink, 0);
                if (k <= 0) break;
                have += (size_t)k;
            }
            if (have == want) r = (int)(want < cap ? want : cap);
        }
    }
    close(s);
    return r;
}

void dnsd_use_tcp(bool on) { use_tcp = on; }
bool dnsd_tcp(void) { return use_tcp; }
static char last_name[DNS_NAME_MAX];

static struct {
    bool used;
    struct sockaddr_in client;
    int via;                                     /* which listening socket the Dreamcast asked on */
    uint16_t cid, uid;
    uint32_t ms;
    uint8_t tries;
    uint8_t pkt[Q_MAX];
    size_t plen;
    char name[DNS_NAME_MAX];
} pend[PENDING];
static uint16_t next_uid = 1;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void dnsd_set_listen(int which, bool on, uint32_t ip) { if (which < 0 || which >= LISTEN_N) return; bind_ip[which] = ip; online[which] = on; }

void dnsd_info(uint32_t *q, uint32_t *fwd, uint32_t *ovr, uint32_t *nx, int *n_ovr, int *n_up, char *last, size_t cap) {
    xSemaphoreTake(conf_lk, portMAX_DELAY);
    *q = cnt.q; *fwd = cnt.fwd; *ovr = cnt.ovr; *nx = cnt.nx;
    *n_ovr = conf ? conf->n_ovr : 0; *n_up = conf ? conf->n_up : 2;
    strlcpy(last, last_name, cap);
    xSemaphoreGive(conf_lk);
}

static void send_up(int us, int i) {
    static uint8_t pkt[PKT_MAX];                 /* static: the DNS task only (its 4 KB stack had 108 bytes to spare) */
    memcpy(pkt, pend[i].pkt, pend[i].plen);
    pkt[0] = (uint8_t)(pend[i].uid >> 8); pkt[1] = (uint8_t)pend[i].uid;
    xSemaphoreTake(conf_lk, portMAX_DELAY);
    uint32_t up = conf ? dns_upstream(conf, pend[i].name, pend[i].tries) : def_up[pend[i].tries & 1];
    xSemaphoreGive(conf_lk);
    if (!up) return;
    if (use_tcp && g_ls && g_ls[pend[i].via] >= 0) {
        static uint8_t a[PKT_MAX];
        int k = dns_tcp_ask(up, pkt, pend[i].plen, a, sizeof a, 2500);
        if (k >= 12) {
            if (k == (int)sizeof a) a[2] |= 2;                          /* longer than fits in UDP: truncated */
            a[0] = (uint8_t)(pend[i].cid >> 8); a[1] = (uint8_t)pend[i].cid;
            sendto(g_ls[pend[i].via], a, (size_t)k, 0, (struct sockaddr *)&pend[i].client, sizeof pend[i].client);
            pend[i].used = false;
            cnt.tcp++;
            return;
        }
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(up) };
    sendto(us, pkt, pend[i].plen, 0, (struct sockaddr *)&to, sizeof to);
    pend[i].tries++;
    pend[i].ms = now_ms();
}

static void on_client(int via, int ls, int us) {
    static uint8_t buf[PKT_MAX], out[PKT_MAX];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int n = recvfrom(ls, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    dns_q_t q;
    if (n <= 0 || n > Q_MAX || !dns_parse(buf, (size_t)n, &q)) return;

    xSemaphoreTake(conf_lk, portMAX_DELAY);
    cnt.q++;
    if (q.qtype == DNS_A) { strlcpy(last_name, q.name, sizeof last_name); }
    uint32_t ip = 0;
    bool ovr = conf && (q.qtype == DNS_A || q.qtype == DNS_AAAA) && dns_override(conf, q.name, &ip);
    bool local = !ovr && dns_local_only(&q);
    if (ovr) cnt.ovr++; else if (local) cnt.nx++; else cnt.fwd++;
    xSemaphoreGive(conf_lk);
    if (q.qtype == DNS_A) dcnow_note_domain(q.name);

    size_t k = 0;
    if (ovr) k = q.qtype == DNS_A ? dns_reply_a(buf, &q, ip, 0, out, sizeof out) : dns_reply_rcode(buf, &q, DNS_NOERROR, out, sizeof out);
    else if (local) k = dns_reply_rcode(buf, &q, DNS_NXDOMAIN, out, sizeof out);
    if (k) { sendto(ls, out, k, 0, (struct sockaddr *)&from, fl); return; }

    int slot = -1;
    for (int i = 0; i < PENDING && slot < 0; i++) if (!pend[i].used) slot = i;
    if (slot < 0) {                                         /* full: reuse the oldest */
        slot = 0;
        for (int i = 1; i < PENDING; i++) if (now_ms() - pend[i].ms > now_ms() - pend[slot].ms) slot = i;
    }
    pend[slot].used = true; pend[slot].client = from; pend[slot].via = via; pend[slot].cid = q.id; pend[slot].uid = next_uid++;
    pend[slot].tries = 0; pend[slot].plen = (size_t)n; memcpy(pend[slot].pkt, buf, (size_t)n);
    strlcpy(pend[slot].name, q.name, sizeof pend[slot].name);
    send_up(us, slot);
}

static void on_upstream(const int *ls, int us) {
    static uint8_t buf[PKT_MAX];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int n = recvfrom(us, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    if (n < 12 || ntohs(from.sin_port) != 53) return;
    uint16_t uid = (uint16_t)(buf[0] << 8 | buf[1]);
    for (int i = 0; i < PENDING; i++)
        if (pend[i].used && pend[i].uid == uid) {
            buf[0] = (uint8_t)(pend[i].cid >> 8); buf[1] = (uint8_t)pend[i].cid;
            if (ls[pend[i].via] >= 0)
                sendto(ls[pend[i].via], buf, (size_t)n, 0, (struct sockaddr *)&pend[i].client, sizeof pend[i].client);
            pend[i].used = false;
            return;
        }
}

static void dns_task(void *arg) {
    (void)arg;
    static int ls[LISTEN_N] = { -1, -1, -1 };
    int us = -1;
    g_ls = ls;
    for (;;) {
        bool any = false;
        for (int i = 0; i < LISTEN_N; i++) {
            if (!online[i]) {
                if (ls[i] >= 0) { close(ls[i]); ls[i] = -1; for (int k = 0; k < PENDING; k++) if (pend[k].via == i) pend[k].used = false; }
                continue;
            }
            any = true;
            bool dup = false;                         /* the Dreamcast network's .1 serves several interfaces: one socket answers them all */
            for (int j = 0; j < LISTEN_N; j++) if (j != i && online[j] && ls[j] >= 0 && bind_ip[j] == bind_ip[i]) dup = true;
            if (ls[i] < 0 && !dup) {
                ls[i] = socket(AF_INET, SOCK_DGRAM, 0);
                struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(bind_ip[i]) };
                if (ls[i] < 0 || bind(ls[i], (struct sockaddr *)&a, sizeof a) != 0) {
                    ESP_LOGE(TAG, "cannot listen on port 53 (%d)", i);
                    if (ls[i] >= 0) close(ls[i]);
                    ls[i] = -1;
                }
            }
        }
        if (!any) {
            if (us >= 0) { close(us); us = -1; }
            memset(pend, 0, sizeof pend);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (us < 0) {
            us = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in u = { .sin_family = AF_INET };
            if (us >= 0) bind(us, (struct sockaddr *)&u, sizeof u);
        }
        if (us < 0 || (ls[0] < 0 && ls[1] < 0 && ls[2] < 0)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        fd_set rs; FD_ZERO(&rs); FD_SET(us, &rs);
        int mx = us;
        for (int i = 0; i < LISTEN_N; i++) if (ls[i] >= 0) { FD_SET(ls[i], &rs); if (ls[i] > mx) mx = ls[i]; }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
        int r = select(mx + 1, &rs, NULL, NULL, &tv);
        if (r > 0) {
            for (int i = 0; i < LISTEN_N; i++) if (ls[i] >= 0 && FD_ISSET(ls[i], &rs)) on_client(i, ls[i], us);
            if (FD_ISSET(us, &rs)) on_upstream(ls, us);
        }
        uint32_t t = now_ms();
        for (int i = 0; i < PENDING; i++) {
            if (!pend[i].used) continue;
            uint32_t age = t - pend[i].ms;
            if (pend[i].tries == 1 && age > 1500) send_up(us, i);       /* the second server */
            else if (age > 3500) pend[i].used = false;                   /* the Dreamcast asks again */
        }
    }
}

/* ---- the downloaded file: read line by line straight into a new table (no copy of the file) ---- */
static dns_conf_t *nc; static char fl[256]; static size_t fln; static int took; static size_t got;
static void take_line(void) { if (fln < sizeof fl) { fl[fln] = 0; if (dns_conf_line(nc, fl, fln)) took++; } fln = 0; }
static esp_err_t http_ev(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA || !nc) return ESP_OK;
    const char *d = e->data;
    got += (size_t)e->data_len;
    for (int i = 0; i < e->data_len; i++) {
        if (d[i] == '\n') take_line();
        else if (fln < sizeof fl) fl[fln++] = d[i];
        else fln = sizeof fl;                                       /* too long: the line is skipped */
    }
    return ESP_OK;
}

static char fetch_err[64];
static bool fetch_file(void) {
    nc = malloc(sizeof *nc);
    if (!nc) { snprintf(fetch_err, sizeof fetch_err, "out of memory"); return false; }
    dns_conf_defaults(nc); fln = 0; took = 0; got = 0;
    bool ok = false;
    esp_err_t er;
    int code = https_req(DNS_FILE_URL, http_ev, 10000, NULL, NULL, &er);
    if (er == ESP_OK && code == 200 && got) {
        if (fln) take_line();
        xSemaphoreTake(conf_lk, portMAX_DELAY);
        dns_conf_t *old = conf; conf = nc;                     /* the new table stays (no second 8 KB copy) */
        xSemaphoreGive(conf_lk);
        free(old);
        say_line("DNS: %d overrides and %d extra servers from dreamcast.online (%d lines)", conf->n_ovr, conf->n_up - 2, took);
        ok = true;
    } else if (er != ESP_OK) snprintf(fetch_err, sizeof fetch_err, "%s", esp_err_to_name(er));
    else snprintf(fetch_err, sizeof fetch_err, "HTTP %d", code);
    if (!ok) free(nc);
    nc = NULL;
    return ok;
}

/* the DNS list download: called often from DCNow!'s task (dcnow.c), the one task for secure
   connections (they take turns anyway: tls_lock), so no 8 KB stack of its own */
void dnsd_fetch_poll(void) {
    static bool warned;
    static uint32_t due_ms = 8000;                         /* Wi-Fi first */
    if (now_ms() - due_ms > 0x80000000u) return;           /* not yet (wraps fine) */
    if (!inet_up()) { due_ms = now_ms() + 2000; return; }
    if (fetch_file()) { warned = false; due_ms = now_ms() + 6u * 3600 * 1000; return; }
    /* the file is only DreamPi's backup (answers locally if the DNS servers are down); without it
       DreamPi says "Did not find remote DNS config; will use upstream" and asks its two servers,
       as this box does. Not there (404): ask again in 6 hours; other errors: in 30 s. */
    bool gone = !strcmp(fetch_err, "HTTP 404");
    if (!warned) {
        char m[160];
        int k = gone ? snprintf(m, sizeof m, "DNS: no dreampi_dns.conf on the server (DreamPi's backup list); asking the DNS servers directly, as DreamPi then does")
                     : snprintf(m, sizeof m, "DNS: could not fetch dreampi_dns.conf (%s); asking the DNS servers directly, will retry", fetch_err);
        log_add("esp32", m, (size_t)k); warned = true;
    }
    due_ms = now_ms() + (gone ? 6u * 3600 * 1000 : 30000);
}

void dnsd_start(void) {
    conf_lk = xSemaphoreCreateMutex();
    dns_ip(DNS_REVIVAL_1, strlen(DNS_REVIVAL_1), &def_up[0]);
    dns_ip(DNS_REVIVAL_2, strlen(DNS_REVIVAL_2), &def_up[1]);
    xTaskCreate(dns_task, "dnsd", 4096, NULL, 5, NULL);
}
