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
#define PENDING 16
#define PKT_MAX 512

static dns_conf_t conf;
static SemaphoreHandle_t conf_lk;
#define LISTEN_N 2                              /* 0: the PPP call, 1: the Ethernet side (E-4) */
static volatile bool online[LISTEN_N];
static volatile uint32_t bind_ip[LISTEN_N];
static struct { uint32_t q, fwd, ovr, nx; } cnt;
static char last_name[DNS_NAME_MAX];

static struct {
    bool used;
    struct sockaddr_in client;
    int via;                                     /* which listening socket the Dreamcast asked on */
    uint16_t cid, uid;
    uint32_t ms;
    uint8_t tries;
    uint8_t pkt[PKT_MAX];
    size_t plen;
    char name[DNS_NAME_MAX];
} pend[PENDING];
static uint16_t next_uid = 1;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void dnsd_set_listen(int which, bool on, uint32_t ip) { if (which < 0 || which >= LISTEN_N) return; bind_ip[which] = ip; online[which] = on; }

void dnsd_info(uint32_t *q, uint32_t *fwd, uint32_t *ovr, uint32_t *nx, int *n_ovr, int *n_up, char *last, size_t cap) {
    xSemaphoreTake(conf_lk, portMAX_DELAY);
    *q = cnt.q; *fwd = cnt.fwd; *ovr = cnt.ovr; *nx = cnt.nx;
    *n_ovr = conf.n_ovr; *n_up = conf.n_up;
    strlcpy(last, last_name, cap);
    xSemaphoreGive(conf_lk);
}

static void send_up(int us, int i) {
    uint8_t pkt[PKT_MAX];
    memcpy(pkt, pend[i].pkt, pend[i].plen);
    pkt[0] = (uint8_t)(pend[i].uid >> 8); pkt[1] = (uint8_t)pend[i].uid;
    xSemaphoreTake(conf_lk, portMAX_DELAY);
    uint32_t up = dns_upstream(&conf, pend[i].name, pend[i].tries);
    xSemaphoreGive(conf_lk);
    if (!up) return;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(up) };
    sendto(us, pkt, pend[i].plen, 0, (struct sockaddr *)&to, sizeof to);
    pend[i].tries++;
    pend[i].ms = now_ms();
}

static void on_client(int via, int ls, int us) {
    uint8_t buf[PKT_MAX], out[PKT_MAX];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int n = recvfrom(ls, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    dns_q_t q;
    if (n <= 0 || !dns_parse(buf, (size_t)n, &q)) return;

    xSemaphoreTake(conf_lk, portMAX_DELAY);
    cnt.q++;
    if (q.qtype == DNS_A) { strlcpy(last_name, q.name, sizeof last_name); }
    uint32_t ip = 0;
    bool ovr = (q.qtype == DNS_A || q.qtype == DNS_AAAA) && dns_override(&conf, q.name, &ip);
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
    uint8_t buf[PKT_MAX];
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
    int ls[LISTEN_N] = { -1, -1 }, us = -1;
    for (;;) {
        bool any = false;
        for (int i = 0; i < LISTEN_N; i++) {
            if (!online[i]) {
                if (ls[i] >= 0) { close(ls[i]); ls[i] = -1; for (int k = 0; k < PENDING; k++) if (pend[k].via == i) pend[k].used = false; }
                continue;
            }
            any = true;
            if (ls[i] < 0) {
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
        if (us < 0 || (ls[0] < 0 && ls[1] < 0)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
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

/* ---- the downloaded file ---- */
static char *fbuf; static size_t flen;
static esp_err_t http_ev(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && fbuf && flen + (size_t)e->data_len < 16 * 1024) {
        memcpy(fbuf + flen, e->data, (size_t)e->data_len);
        flen += (size_t)e->data_len;
    }
    return ESP_OK;
}

static bool fetch_file(void) {
    fbuf = malloc(16 * 1024 + 1); flen = 0;
    if (!fbuf) return false;
    esp_http_client_config_t c = {
        .url = DNS_FILE_URL, .event_handler = http_ev, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    bool ok = false;
    if (h && esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) == 200 && flen) {
        fbuf[flen] = 0;
        dns_conf_t *nc = malloc(sizeof *nc);
        if (nc) {
            dns_conf_defaults(nc);
            int took = dns_conf_text(nc, fbuf, flen);
            xSemaphoreTake(conf_lk, portMAX_DELAY);
            memcpy(&conf, nc, sizeof conf);
            xSemaphoreGive(conf_lk);
            char t[96];
            int k = snprintf(t, sizeof t, "DNS: %d overrides and %d extra servers from dreamcast.online (%d lines)", nc->n_ovr, nc->n_up - 2, took);
            log_add("esp32", t, (size_t)k);
            free(nc);
            ok = true;
        }
    }
    if (h) esp_http_client_cleanup(h);
    free(fbuf); fbuf = NULL;
    return ok;
}

static void fetch_task(void *arg) {
    (void)arg;
    bool warned = false;
    vTaskDelay(pdMS_TO_TICKS(8000));                       /* Wi-Fi first */
    for (;;) {
        if (fetch_file()) { warned = false; vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000)); }
        else {
            if (!warned) { const char *m = "DNS: could not fetch dreampi_dns.conf yet (no internet?); using the built-in servers, will retry"; log_add("esp32", m, strlen(m)); warned = true; }
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
}

void dnsd_start(void) {
    conf_lk = xSemaphoreCreateMutex();
    dns_conf_defaults(&conf);
    xTaskCreate(dns_task, "dnsd", 4096, NULL, 5, NULL);
    xTaskCreate(fetch_task, "dns_fetch", 8192, NULL, 3, NULL);
}
