/*
 * VPN to Dreamcast Live (E-7): the OpenVPN client of ovpn.c on a UDP socket, a task, and the
 * tunnel as a lwIP interface "DC_VPN" with the address the server pushes.
 *
 * What it does today: connects, keeps the tunnel up, tells the result in the status lines and
 * on the page, and tests the tunnel by pinging the server's gateway through it. Sending the
 * Dreamcast's traffic through it (NAT out of the tunnel, ports in) comes with E-5.
 *
 * The profiles are built into the firmware from esp32/vpn/ (git-ignored, see vpn/README.md).
 * Off by default: switch it on on the page. Servers are tried in the order of server.list;
 * the one that worked last time goes first.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip4.h"
#include "lwip/pbuf.h"
#include "ovpn.h"
#include "app.h"

static const char *TAG = "vpn";

extern const char vpn_profiles_start[] asm("_binary_vpn_profiles_txt_start");
extern const char vpn_profiles_end[]   asm("_binary_vpn_profiles_txt_end");

#define MAX_PROF 6
typedef struct { ovpn_conf_t cf; char host[64]; } prof_t;
static prof_t prof[MAX_PROF];
static int nprof;

static volatile bool enabled;
static SemaphoreHandle_t lk;
static ovpn_t *vo;
static int vsock = -1;
static struct sockaddr_in vdst;
static int cur_prof = -1;
static struct netif vif;
static bool vif_added;
static volatile int64_t up_since_us;
static struct { uint32_t connects, fails; char err[96]; char ip[16]; char gw[16]; char server[64]; int ping_ms; } st = { .ping_ms = -1 };

static void fmt_ip(char *d, size_t n, uint32_t a) {
    snprintf(d, n, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------- ovpn ops ---------- */
static int op_send(void *c, const uint8_t *d, size_t n) { (void)c; return vsock < 0 ? -1 : (int)send(vsock, d, n, 0); }
static void op_log(void *c, const char *m) { (void)c; ESP_LOGI(TAG, "%s", m); }
static int op_rand(void *c, uint8_t *b, size_t n) { (void)c; esp_fill_random(b, n); return 0; }

static uint16_t csum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += p[i] << 8 | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static volatile int64_t ping_sent_us;
#define PING_ID 0xD1C1

static void op_ip(void *c, const uint8_t *ip, size_t n) {
    (void)c;
    /* our own test ping coming back? */
    if (n >= 28 && ip[9] == 1 && ip[20] == 0 && ((ip[24] << 8) | ip[25]) == PING_ID && ping_sent_us) {
        st.ping_ms = (int)((esp_timer_get_time() - ping_sent_us) / 1000);
        ping_sent_us = 0;
        char t[80];
        int k = snprintf(t, sizeof t, "VPN: tunnel works, %d ms to the server's gateway", st.ping_ms);
        log_add("esp32", t, (size_t)k);
        return;
    }
    if (!vif_added) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, ip, n);
    if (vif.input(p, &vif) != ERR_OK) pbuf_free(p);
}

/* ---------- lwIP interface ---------- */
static err_t vif_out(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst) {
    (void)nif; (void)dst;
    uint8_t buf[1500];
    if (p->tot_len > sizeof buf) return ERR_BUF;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    err_t r = ERR_RTE;
    if (xSemaphoreTake(lk, pdMS_TO_TICKS(50))) {
        if (vo && ovpn_send_ip(vo, buf, p->tot_len, now_ms()) == 0) r = ERR_OK;
        xSemaphoreGive(lk);
    }
    return r;
}
static err_t vif_init(struct netif *nif) {
    nif->name[0] = 'v'; nif->name[1] = 'p';
    nif->output = vif_out;
    nif->mtu = 1400;
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void vif_up(const ovpn_info_t *in) {
    ip4_addr_t ip, mask, gw;
    ip.addr = lwip_htonl(in->ip);
    mask.addr = lwip_htonl(in->mask);
    gw.addr = lwip_htonl(in->gateway ? in->gateway : in->peer);
    LOCK_TCPIP_CORE();
    if (!vif_added) {
        netif_add(&vif, &ip, &mask, &gw, NULL, vif_init, tcpip_input);
        vif_added = true;
    }
    netif_set_up(&vif);
    netif_set_link_up(&vif);
    UNLOCK_TCPIP_CORE();
}

static void vif_down(void) {
    if (!vif_added) return;
    LOCK_TCPIP_CORE();
    netif_set_down(&vif);
    netif_remove(&vif);
    UNLOCK_TCPIP_CORE();
    vif_added = false;
}

/* ---------- profiles ---------- */
static void load_profiles(void) {
    static const char M[] = "@@PROFILE@@", S[] = "@@SERVERS@@";
    size_t len = (size_t)(vpn_profiles_end - vpn_profiles_start);
    char *txt = malloc(len + 1);
    if (!txt) return;
    memcpy(txt, vpn_profiles_start, len);
    txt[len] = 0;
    char *servers = strstr(txt, S);
    char *first = strstr(txt, M);
    char order[MAX_PROF][64];
    int no = 0;
    if (servers) {                                   /* server.list: the servers DreamPi uses, in order */
        servers += strlen(S);
        size_t l = first ? (size_t)(first - servers) : strlen(servers);
        char *list = strndup(servers, l), *save = NULL;
        for (char *t = list ? strtok_r(list, "\r\n ", &save) : NULL; t && no < MAX_PROF; t = strtok_r(NULL, "\r\n ", &save)) {
            snprintf(order[no++], sizeof order[0], "%s", t);
        }
        free(list);
    }
    char *s = first;
    while (s) {
        char *body = s + strlen(M);
        char *nx = strstr(body, M);
        size_t l = nx ? (size_t)(nx - body) : strlen(body);
        s = nx;
        if (l < 100) continue;
        char *copy = strndup(body, l);
        ovpn_conf_t cf;
        if (!copy || ovpn_conf_parse(copy, &cf) != 0) { free(copy); continue; }
        free(copy);
        bool listed = no == 0;
        for (int k = 0; k < no; k++) if (!strcmp(order[k], cf.host)) listed = true;
        if (!listed || nprof >= MAX_PROF) { ovpn_conf_free(&cf); continue; }
        prof[nprof].cf = cf;
        snprintf(prof[nprof].host, sizeof prof[nprof].host, "%s", cf.host);
        nprof++;
    }
    for (int a = 0; a < nprof; a++)                  /* order of server.list */
        for (int b = a + 1; b < nprof; b++) {
            int ra = 99, rb = 99;
            for (int k = 0; k < no; k++) { if (!strcmp(order[k], prof[a].host)) ra = k; if (!strcmp(order[k], prof[b].host)) rb = k; }
            if (rb < ra) { prof_t t = prof[a]; prof[a] = prof[b]; prof[b] = t; }
        }
    free(txt);
}

/* ---------- connect ---------- */
static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void note(const char *fmt, ...) {
    char t[128];
    va_list ap; va_start(ap, fmt); int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    log_add("esp32", t, (size_t)(k < (int)sizeof t ? k : (int)sizeof t - 1));
}

static void teardown(void) {
    xSemaphoreTake(lk, portMAX_DELAY);
    ovpn_t *o = vo;
    vo = NULL;
    xSemaphoreGive(lk);
    vif_down();
    if (o) { ovpn_stop(o, now_ms()); ovpn_free(o); }
    if (vsock >= 0) { close(vsock); vsock = -1; }
    up_since_us = 0;
    st.ip[0] = st.gw[0] = 0;
    st.ping_ms = -1;
}

static bool open_socket(const prof_t *pr) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", pr->cf.port);
    if (getaddrinfo(pr->host, port, &hints, &res) != 0 || !res) { snprintf(st.err, sizeof st.err, "cannot find %s", pr->host); return false; }
    memcpy(&vdst, res->ai_addr, sizeof vdst);
    freeaddrinfo(res);
    vsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (vsock < 0) { snprintf(st.err, sizeof st.err, "no socket"); return false; }
    if (connect(vsock, (struct sockaddr *)&vdst, sizeof vdst) != 0) { snprintf(st.err, sizeof st.err, "connect failed"); close(vsock); vsock = -1; return false; }
    return true;
}

static int load_last(void) {
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "vpn_last", &v); nvs_close(h); }
    return v;
}
static void save_last(int v) {
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "vpn_last", (uint8_t)v); nvs_commit(h); nvs_close(h); }
}

/* one connection attempt to profile i; returns when the tunnel is gone or switched off */
static void session(int i) {
    prof_t *pr = &prof[i];
    snprintf(st.server, sizeof st.server, "%s", pr->host);
    st.err[0] = 0;
    cur_prof = i;
    if (!open_socket(pr)) { st.fails++; note("VPN: %s", st.err); return; }
    ovpn_ops_t ops = { .send = op_send, .ip_in = op_ip, .log = op_log, .random = op_rand };
    xSemaphoreTake(lk, portMAX_DELAY);
    vo = ovpn_new(&pr->cf, &ops);
    if (vo) ovpn_start(vo, now_ms());
    xSemaphoreGive(lk);
    if (!vo) { st.fails++; snprintf(st.err, sizeof st.err, "out of memory"); return; }
    note("VPN: connecting to %s", pr->host);
    bool announced = false, pinged = false;
    uint32_t last_ping = 0;
    for (;;) {
        if (!enabled) break;
        fd_set rs; FD_ZERO(&rs); FD_SET(vsock, &rs);
        struct timeval tv = { 0, 50000 };
        int r = select(vsock + 1, &rs, NULL, NULL, &tv);
        xSemaphoreTake(lk, portMAX_DELAY);
        if (r > 0 && FD_ISSET(vsock, &rs)) {
            static uint8_t b[1700];
            for (int k = 0; k < 8; k++) {
                int n = recv(vsock, b, sizeof b, MSG_DONTWAIT);
                if (n <= 0) break;
                ovpn_input(vo, b, (size_t)n, now_ms());
            }
        }
        ovpn_tick(vo, now_ms());
        ovpn_state_t s = ovpn_state(vo);
        xSemaphoreGive(lk);
        if (s == OVPN_FAILED) { snprintf(st.err, sizeof st.err, "%s", ovpn_error(vo)); st.fails++; note("VPN: %s", st.err); break; }
        if (s == OVPN_UP && !announced) {
            const ovpn_info_t *in = ovpn_info(vo);
            fmt_ip(st.ip, sizeof st.ip, in->ip);
            fmt_ip(st.gw, sizeof st.gw, in->gateway ? in->gateway : in->peer);
            vif_up(in);
            st.connects++;
            up_since_us = esp_timer_get_time();
            save_last(i);
            announced = true;
            note("VPN: connected to %s, tunnel address %s", pr->host, st.ip);
        }
        if (announced && !pinged && (now_ms() - last_ping) > 1000) {
            /* test ping to the server's gateway through the tunnel */
            const ovpn_info_t *in = ovpn_info(vo);
            uint32_t dst = in->gateway ? in->gateway : in->peer;
            uint8_t pk[36] = {0x45, 0, 0, 36, 0, 0, 0, 0, 64, 1};
            for (int k = 0; k < 4; k++) { pk[12 + k] = in->ip >> (24 - 8 * k); pk[16 + k] = dst >> (24 - 8 * k); }
            uint16_t c = csum(pk, 20); pk[10] = c >> 8; pk[11] = c;
            pk[20] = 8; pk[24] = PING_ID >> 8; pk[25] = PING_ID & 255; pk[27] = 1;
            c = csum(pk + 20, 16); pk[22] = c >> 8; pk[23] = c;
            ping_sent_us = esp_timer_get_time();
            xSemaphoreTake(lk, portMAX_DELAY);
            ovpn_send_ip(vo, pk, sizeof pk, now_ms());
            xSemaphoreGive(lk);
            pinged = true;
            last_ping = now_ms();
        }
    }
    teardown();
}

static void vpn_task(void *arg) {
    (void)arg;
    int fail_round = 0;
    for (;;) {
        if (!enabled || !nprof || !wifi_sta_up()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        int start = load_last();
        if (start >= nprof) start = 0;
        for (int k = 0; k < nprof && enabled; k++) {
            session((start + k) % nprof);
            if (!enabled) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (enabled) {                         /* every server failed: wait longer, then start over */
            fail_round++;
            vTaskDelay(pdMS_TO_TICKS(fail_round < 3 ? 10000 : 60000));
        }
    }
}

/* ---------- control and status ---------- */
void vpn_set_enabled(bool on) {
    enabled = on;
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "vpn", on); nvs_commit(h); nvs_close(h); }
}
bool vpn_enabled(void) { return enabled; }

size_t vpn_info_json(char *out, size_t cap) {
    bool up = up_since_us != 0;
    return (size_t)snprintf(out, cap,
        "{\"profiles\":%d,\"on\":%s,\"up\":%s,\"server\":\"%s\",\"ip\":\"%s\",\"gateway\":\"%s\",\"ping_ms\":%d,"
        "\"connects\":%lu,\"fails\":%lu,\"error\":\"%s\",\"up_s\":%lu}",
        nprof, enabled ? "true" : "false", up ? "true" : "false", st.server, st.ip, st.gw, st.ping_ms,
        (unsigned long)st.connects, (unsigned long)st.fails, st.err,
        up ? (unsigned long)((esp_timer_get_time() - up_since_us) / 1000000) : 0UL);
}

void vpn_start(void) {
    lk = xSemaphoreCreateMutex();
    load_profiles();
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "vpn", &v); nvs_close(h); }
    enabled = v && nprof;
    ESP_LOGI(TAG, "%d VPN profile(s) in this firmware, VPN %s", nprof, enabled ? "on" : "off");
    xTaskCreate(vpn_task, "vpn", 12288, NULL, 5, NULL);
}
