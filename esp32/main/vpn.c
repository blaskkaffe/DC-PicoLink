/*
 * VPN to Dreamcast Live (E-7): the OpenVPN client of ovpn.c on a UDP socket, a task, and the
 * tunnel as a lwIP interface "DC_VPN" with the address the server pushes.
 *
 * What it does today: connects, keeps the tunnel up, tells the result in the status lines and
 * on the page, and tests the tunnel by pinging the server's gateway through it. Sending the
 * Dreamcast's traffic through it (NAT out of the tunnel, ports in) comes with E-5.
 *
 * The profiles are built into the firmware from esp32/vpn/ (committed, see vpn/README.md).
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
#include "lwip/udp.h"
#include "lwip/priv/tcpip_priv.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "ovpn.h"
#include "vpnpick_model.h"
#include "app.h"

static const char *TAG = "vpn";

extern const char vpn_profiles_start[] asm("_binary_vpn_profiles_txt_start");
extern const char vpn_profiles_end[]   asm("_binary_vpn_profiles_txt_end");

#define MAX_PROF 6
/* only the server and where the profile's text is: the full profile (certificates, key) is parsed
   when connecting and freed afterwards (3 parsed profiles held ~22 KB of RAM for good) */
typedef struct { char host[64]; int port; size_t off, len; } prof_t;
static bool prof_uploaded;                       /* text in the vpnprof partition (else built in) */
static prof_t prof[MAX_PROF];
static int order_[MAX_PROF], best_ = 0;            /* auto: servers by ping (ping_order) */
static int nprof;

static volatile bool enabled;
static SemaphoreHandle_t lk;
static ovpn_t *vo;
static int vsock = -1;
static struct sockaddr_in vdst;
static int cur_prof = -1;
static volatile bool pick_changed;
/* which server: "" = auto (server.list order: shumania first, back to it when no Dreamcast is
   online), or one host chosen on the page */
static char pick_host[64];
static int pick_index(void) {
    if (!pick_host[0]) return -1;
    for (int i = 0; i < nprof; i++) if (!strcmp(prof[i].host, pick_host)) return i;
    return -1;
}
static bool dc_online(void) { return ppp_dc_ip() || dc_eth_dc_ip() || wired_dc_ip(); }
static struct netif vif;
static bool vif_added;
static volatile int64_t up_since_us;
static struct { uint32_t connects, fails, blocked; char err[96]; char ip[16]; char gw[16]; char server[64]; int ping_ms; } st = { .ping_ms = -1 };

static volatile uint32_t tun_ip;                 /* our tunnel address (host order), 0 = down */
static bool vpn_in_allowed(const uint8_t *ip, size_t n);

static void fmt_ip(char *d, size_t n, uint32_t a) {
    snprintf(d, n, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------- ovpn ops ---------- */
/* Sending. A socket call made from lwIP's own thread waits for that thread (without core locking)
   or for the core lock it already holds (with it): either way forever. Packets the
   Dreamcast sends into the tunnel are encrypted in lwIP's thread (vif_out), so those go out
   through a raw lwIP UDP handle on the socket's own local port (unconnected, so the socket still
   receives everything); the VPN task keeps using the socket. */
static struct udp_pcb *rawpcb;
static TaskHandle_t lwip_task;
static int op_send(void *c, const uint8_t *d, size_t n) {
    (void)c;
    if (vsock < 0) return -1;
    if (!lwip_task) lwip_task = xTaskGetHandle("tiT");               /* lwIP's thread (TCPIP_THREAD_NAME) */
    if (xTaskGetCurrentTaskHandle() != lwip_task) return (int)send(vsock, d, n, 0);
    if (!rawpcb) return -1;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!p) return -1;
    memcpy(p->payload, d, n);
    ip_addr_t to = IPADDR4_INIT(vdst.sin_addr.s_addr);
    err_t e = udp_sendto(rawpcb, p, &to, lwip_ntohs(vdst.sin_port));
    pbuf_free(p);
    return e == ERR_OK ? (int)n : -1;
}

/* run fn in lwIP's thread and wait for it (the safe way without core locking) */
typedef struct { struct tcpip_api_call_data base; void (*fn)(void *); void *arg; } in_lwip_t;
static err_t in_lwip_cb(struct tcpip_api_call_data *c) {
    in_lwip_t *x = (in_lwip_t *)c;
    x->fn(x->arg);
    return ERR_OK;
}
static void in_lwip(void (*fn)(void *), void *arg) { in_lwip_t x = { .fn = fn, .arg = arg }; tcpip_api_call(in_lwip_cb, &x.base); }

static void raw_open(void *arg) {
    uint16_t port = (uint16_t)(uintptr_t)arg;
    rawpcb = udp_new();
    if (!rawpcb) return;
    ip_set_option(rawpcb, SOF_REUSEADDR);
    if (udp_bind(rawpcb, IP_ADDR_ANY, port) != ERR_OK) { udp_remove(rawpcb); rawpcb = NULL; }
}
static void raw_close(void *arg) { (void)arg; if (rawpcb) { udp_remove(rawpcb); rawpcb = NULL; } }
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
static volatile uint32_t dc_bytes_in, dc_bytes_out;     /* game traffic through the tunnel (the page shows it) */
static char routes_txt[120];
#define PING_ID 0xD1C1

static void op_ip(void *c, const uint8_t *ip, size_t n) {
    (void)c;
    /* our own test ping coming back? */
    if (n >= 28 && ip[9] == 1 && ip[20] == 0 && ((ip[24] << 8) | ip[25]) == PING_ID && ping_sent_us) {
        st.ping_ms = (int)((esp_timer_get_time() - ping_sent_us) / 1000);
        ping_sent_us = 0;
        say_line("VPN: tunnel works, %d ms to the server's gateway", st.ping_ms);
        return;
    }
    if (!vif_added) return;
    if (!vpn_in_allowed(ip, n)) { st.blocked++; return; }
    dc_bytes_in += (uint32_t)n;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, ip, n);
    if (vif.input(p, &vif) != ERR_OK) pbuf_free(p);
}

/* ---------- lwIP interface ---------- */
/* Into the tunnel (lwIP's thread). When the VPN task has the session right now, the packet waits
   in a short queue that the task sends as soon as it lets go, instead of lwIP's thread waiting
   (which stalled all networking up to 50 ms and then dropped the packet). */
static QueueHandle_t outq;                  /* struct pbuf * (own copies) */
static err_t vif_out(struct netif *nif, struct pbuf *p, const ip4_addr_t *dst) {
    (void)nif; (void)dst;
    static uint8_t buf[1500];               /* lwIP's own thread only: not on its 3 KB stack */
    if (p->tot_len > sizeof buf) return ERR_BUF;
    if (xSemaphoreTake(lk, 0)) {
        pbuf_copy_partial(p, buf, p->tot_len, 0);
        err_t r = ERR_RTE;
        if (vo && ovpn_send_ip(vo, buf, p->tot_len, now_ms()) == 0) { r = ERR_OK; dc_bytes_out += p->tot_len; }
        xSemaphoreGive(lk);
        return r;
    }
    struct pbuf *c = outq ? pbuf_clone(PBUF_RAW, PBUF_RAM, p) : NULL;
    if (!c) return ERR_MEM;
    if (xQueueSend(outq, &c, 0) != pdTRUE) { pbuf_free(c); return ERR_MEM; }
    return ERR_OK;
}
/* the VPN task: send what waited (after each time it held the session) */
static void drain_out(void) {
    struct pbuf *c;
    static uint8_t b2[1500];                 /* the VPN task only */
    while (outq && xQueueReceive(outq, &c, 0) == pdTRUE) {
        uint16_t n = c->tot_len;
        pbuf_copy_partial(c, b2, n, 0);
        pbuf_free(c);
        xSemaphoreTake(lk, portMAX_DELAY);
        if (vo && ovpn_send_ip(vo, b2, n, now_ms()) == 0) dc_bytes_out += n;
        xSemaphoreGive(lk);
    }
}
static err_t vif_init(struct netif *nif) {
    nif->name[0] = 'v'; nif->name[1] = 'p';
    nif->output = vif_out;
    nif->mtu = 1400;
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* DreamPi: while the tunnel is up the Dreamcast's PPP address is the tunnel address + 1, so
   other VPN users reach it directly; its Internet traffic still goes out through Wi-Fi. */
uint32_t vpn_dc_addr(void) { return tun_ip ? tun_ip + 1 : 0; }
struct netif *vpn_netif(void) { return tun_ip && vif_added ? &vif : NULL; }

/* DreamPi's INPUT rules for tun0: to our own tunnel address only ICMP, TCP 65432/65433 and
   UDP 20001/20002 (Netlink), and replies to what we sent. Everything else passes on. */
static bool vpn_in_allowed(const uint8_t *ip, size_t n) {
    if (n < 20 || (ip[0] >> 4) != 4) return false;
    uint32_t dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
    if (dst != tun_ip) return true;                     /* for the Dreamcast or others: forwarded */
    size_t hl = (size_t)(ip[0] & 15) * 4;
    if (ip[9] == 1) return true;
    if (n < hl + 4) return false;
    uint16_t dp = (uint16_t)(ip[hl + 2] << 8 | ip[hl + 3]);
    if (ip[9] == 6) {
        if (dp == 65432 || dp == 65433) return true;
        if (n < hl + 14) return false;
        uint8_t fl = ip[hl + 13];
        return !((fl & 0x02) && !(fl & 0x10));          /* no new connections to anything else */
    }
    if (ip[9] == 17) return dp == 20001 || dp == 20002 || dp >= 49152;    /* lwIP's own ports for replies */
    return false;
}

/* routes the server pushed (beyond the tunnel's own subnet), for vpn_route() */
static struct { uint32_t net, mask; } routes[8];
static volatile int nroutes;

/* lwIP's routing hook (hooks/dcl_lwip_hooks.h): a destination in a pushed route goes into the tunnel */
struct netif *vpn_route(const ip4_addr_t *dest) {
    int n = nroutes;
    if (!n || !tun_ip || !dest) return NULL;
    uint32_t d = lwip_ntohl(ip4_addr_get_u32(dest));
    for (int i = 0; i < n; i++) if ((d & routes[i].mask) == routes[i].net) return &vif;
    return NULL;
}

static void vif_up_lwip(void *arg) {
    const ovpn_info_t *in = arg;
    ip4_addr_t ip, mask, gw;
    ip.addr = lwip_htonl(in->ip);
    mask.addr = lwip_htonl(in->mask);
    gw.addr = lwip_htonl(in->gateway ? in->gateway : in->peer);
    if (!vif_added) {
        netif_add(&vif, &ip, &mask, &gw, NULL, vif_init, tcpip_input);
        vif_added = true;
    }
#if IP_NAPT
    vif.napt = 0;                    /* what comes out of the tunnel for the Dreamcast reaches it unchanged (DreamPi: no NAT on tun0;
                                        with napt set, lwIP rewrote every peer's address to this box's) */
#endif
    tun_ip = in->ip;
    int k = 0;
    for (int i = 0; i < in->nroute && k < 8; i++) {
        uint32_t m = in->route[i].mask;
        routes[k].mask = m; routes[k].net = in->route[i].net & m; k++;
        char a[16], b[16]; fmt_ip(a, sizeof a, in->route[i].net & m); fmt_ip(b, sizeof b, m);
        ESP_LOGI(TAG, "pushed route %s/%s into the tunnel", a, b);
    }
    nroutes = k;
    routes_txt[0] = 0;
    for (int i = 0; i < k; i++) {
        uint32_t a = routes[i].net, m = routes[i].mask; int bits = 0; while (m & 0x80000000u) { bits++; m <<= 1; }
        size_t l = strlen(routes_txt);
        snprintf(routes_txt + l, sizeof routes_txt - l, "%s%u.%u.%u.%u/%d", i ? " " : "", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255), (unsigned)(a & 255), bits);
    }
    if (k || in->redirect_gateway) {
        char t[300];
        int n = snprintf(t, sizeof t, "VPN: %d route%s from the server go into the tunnel:", k, k == 1 ? "" : "s");
        for (int i = 0; i < k && n < (int)sizeof t - 24; i++) {      /* which ones: a wide one would take Internet traffic too */
            uint32_t a = routes[i].net, m = routes[i].mask; int bits = 0; while (m & 0x80000000u) { bits++; m <<= 1; }
            n += snprintf(t + n, sizeof t - n, " %u.%u.%u.%u/%d", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255), (unsigned)(a & 255), bits);
        }
        if (in->redirect_gateway && n < (int)sizeof t - 1)
            n += snprintf(t + n, sizeof t - n, "; its \"everything through the VPN\" (redirect-gateway) is not followed: Internet traffic stays outside the tunnel");
        if (n > (int)sizeof t - 1) n = sizeof t - 1;
        log_add("esp32", t, (size_t)n);
    }
    netif_set_up(&vif);
    netif_set_link_up(&vif);
}
static void vif_up(const ovpn_info_t *in) { in_lwip(vif_up_lwip, (void *)in); }

static void vif_down_lwip(void *arg) {
    (void)arg;
    tun_ip = 0;
    nroutes = 0;
    if (!vif_added) return;
    netif_set_down(&vif);
    netif_remove(&vif);
    vif_added = false;
}
static void vif_down(void) { in_lwip(vif_down_lwip, NULL); }

/* ---------- profiles ----------
   Built in (esp32/vpn/) or uploaded on the page into the "vpnprof" partition:
   "DCLVPN1\0", length (4 bytes), then the same text as the built-in file, NUL-terminated. */
#define PROF_MAGIC "DCLVPN1"
static const char *src_name = "none";

static void parse_profiles(const char *txt) {
    static const char M[] = "@@PROFILE@@", S[] = "@@SERVERS@@";
    const char *servers = strstr(txt, S);
    const char *first = strstr(txt, M);
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
    const char *s = first;
    while (s) {
        const char *body = s + strlen(M);
        const char *nx = strstr(body, M);
        size_t l = nx ? (size_t)(nx - body) : strlen(body);
        s = nx;
        if (l < 100) continue;
        char *copy = strndup(body, l);
        ovpn_conf_t cf;
        if (!copy || ovpn_conf_parse(copy, &cf) != 0) { free(copy); continue; }   /* checked once here */
        free(copy);
        bool listed = no == 0;
        for (int k = 0; k < no; k++) if (!strcmp(order[k], cf.host)) listed = true;
        if (listed && nprof < MAX_PROF) {
            snprintf(prof[nprof].host, sizeof prof[nprof].host, "%s", cf.host);
            prof[nprof].port = cf.port;
            prof[nprof].off = (size_t)(body - txt);
            prof[nprof].len = l;
            nprof++;
        }
        ovpn_conf_free(&cf);
    }
    for (int a = 0; a < nprof; a++)                  /* order of server.list */
        for (int b = a + 1; b < nprof; b++) {
            int ra = 99, rb = 99;
            for (int k = 0; k < no; k++) { if (!strcmp(order[k], prof[a].host)) ra = k; if (!strcmp(order[k], prof[b].host)) rb = k; }
            if (rb < ra) { prof_t t = prof[a]; prof[a] = prof[b]; prof[b] = t; }
        }
}

static const esp_partition_t *prof_part(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "vpnprof");
}

static void load_profiles(void) {
    const esp_partition_t *p = prof_part();
    if (p) {
        char hdr[12];
        if (esp_partition_read(p, 0, hdr, sizeof hdr) == ESP_OK && !memcmp(hdr, PROF_MAGIC, 8)) {
            uint32_t len; memcpy(&len, hdr + 8, 4);
            const void *map; esp_partition_mmap_handle_t mh;
            if (len && len < p->size - 13 && esp_partition_mmap(p, 0, len + 13, ESP_PARTITION_MMAP_DATA, &map, &mh) == ESP_OK) {
                const char *t = (const char *)map + 12;
                if (t[len] == 0) { parse_profiles(t); if (nprof) { src_name = "uploaded"; prof_uploaded = true; } }
                esp_partition_munmap(mh);
            }
        }
    }
    if (!nprof) {
        size_t len = (size_t)(vpn_profiles_end - vpn_profiles_start);   /* EMBED_TXTFILES ends it with a NUL */
        if (len > 1 && vpn_profiles_start[len - 1] == 0) { parse_profiles((const char *)vpn_profiles_start); if (nprof) src_name = "built in"; }
    }
}

/* the page's upload: the text (as above, without the header) in pieces; n == 0 at the end.
   Returns false on a flash error. An empty upload removes the profiles. */
static size_t up_off;
bool vpn_profiles_write(const void *d, size_t n, bool first, bool last) {
    const esp_partition_t *p = prof_part();
    if (!p) return false;
    if (first) {
        if (esp_partition_erase_range(p, 0, p->size) != ESP_OK) return false;
        up_off = 12;
    }
    if (n) {
        if (up_off + n + 1 > p->size) return false;
        if (esp_partition_write(p, up_off, d, n) != ESP_OK) return false;
        up_off += n;
    }
    if (last) {
        uint32_t len = (uint32_t)(up_off - 12);
        if (!len) return true;                              /* erased: no profiles */
        char z = 0, hdr[12];
        if (esp_partition_write(p, up_off, &z, 1) != ESP_OK) return false;
        memcpy(hdr, PROF_MAGIC, 8); memcpy(hdr + 8, &len, 4);
        if (esp_partition_write(p, 0, hdr, sizeof hdr) != ESP_OK) return false;
    }
    return true;
}

/* ---------- connect ---------- */
#define note say_line

static void teardown(void) {
    xSemaphoreTake(lk, portMAX_DELAY);
    ovpn_t *o = vo;
    vo = NULL;
    xSemaphoreGive(lk);
    vif_down();
    { struct pbuf *c; while (outq && xQueueReceive(outq, &c, 0) == pdTRUE) pbuf_free(c); }
    if (o) { ovpn_stop(o, now_ms()); ovpn_free(o); }
    in_lwip(raw_close, NULL);
    if (vsock >= 0) { close(vsock); vsock = -1; }
    up_since_us = 0;
    st.ip[0] = st.gw[0] = 0;
    st.ping_ms = -1;
}

static bool open_socket(const prof_t *pr) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", pr->port);
    if (getaddrinfo(pr->host, port, &hints, &res) != 0 || !res) { snprintf(st.err, sizeof st.err, "cannot find %s", pr->host); return false; }
    memcpy(&vdst, res->ai_addr, sizeof vdst);
    freeaddrinfo(res);
    vsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (vsock < 0) { snprintf(st.err, sizeof st.err, "no socket"); return false; }
    int one = 1;
    setsockopt(vsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   /* the raw handle shares its port (op_send) */
    if (connect(vsock, (struct sockaddr *)&vdst, sizeof vdst) != 0) { snprintf(st.err, sizeof st.err, "connect failed"); close(vsock); vsock = -1; return false; }
    struct sockaddr_in me; socklen_t ml = sizeof me;
    if (getsockname(vsock, (struct sockaddr *)&me, &ml) == 0) in_lwip(raw_open, (void *)(uintptr_t)ntohs(me.sin_port));
    if (!rawpcb) ESP_LOGW(TAG, "no raw UDP handle: the Dreamcast's packets cannot go into the tunnel");
    return true;
}

/* one connection attempt to profile i; returns when the tunnel is gone or switched off */
/* the full profile i, parsed now (freed with ovpn_conf_free after the session) */
static bool load_conf(int i, ovpn_conf_t *cf) {
    prof_t *pr = &prof[i];
    char *txt = malloc(pr->len + 1);
    if (!txt) return false;
    bool ok = true;
    if (prof_uploaded) {
        const esp_partition_t *p = prof_part();
        ok = p && esp_partition_read(p, 12 + pr->off, txt, pr->len) == ESP_OK;
    } else memcpy(txt, (const char *)vpn_profiles_start + pr->off, pr->len);
    txt[pr->len] = 0;
    ok = ok && ovpn_conf_parse(txt, cf) == 0;
    free(txt);
    if (!ok) return false;
    uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);          /* IV_HWADDR, as a DreamPi sends its MAC */
    snprintf(cf->hwaddr, sizeof cf->hwaddr, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return true;
}

static void session_conf(int i, ovpn_conf_t *cf);
static void session(int i) {
    ovpn_conf_t cf;
    if (!load_conf(i, &cf)) { st.fails++; snprintf(st.err, sizeof st.err, "profile %s could not be loaded (memory?)", prof[i].host); note("VPN: %s", st.err); return; }
    session_conf(i, &cf);
    ovpn_conf_free(&cf);                              /* after teardown: the session used its strings */
}

static void session_conf(int i, ovpn_conf_t *cf) {
    prof_t *pr = &prof[i];
    snprintf(st.server, sizeof st.server, "%s", pr->host);
    st.err[0] = 0;
    cur_prof = i;
    if (!open_socket(pr)) { st.fails++; note("VPN: %s", st.err); return; }
    ovpn_ops_t ops = { .send = op_send, .ip_in = op_ip, .log = op_log, .random = op_rand };
    /* the handshake is the RAM peak (certificates, RSA, TLS buffers): one secure connection at a
       time, as DCNow! and the DNS list do. Held until TLS is done, at most 15 s. */
    tls_lock();
    bool locked = true;
    uint32_t lock_t = now_ms();
    xSemaphoreTake(lk, portMAX_DELAY);
    vo = ovpn_new(cf, &ops);
    ovpn_conf_free(cf);                               /* parsed into vo: the profile's text is not needed any more */
    if (vo) ovpn_start(vo, now_ms());
    xSemaphoreGive(lk);
    if (!vo || ovpn_state(vo) == OVPN_FAILED) {
        st.fails++;
        if (!vo || !strcmp(ovpn_error(vo), "out of memory"))
            snprintf(st.err, sizeof st.err, "not enough memory to connect (free %u bytes, largest block %u)",
                     (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        else snprintf(st.err, sizeof st.err, "%s", ovpn_error(vo));
        note("VPN: %s", st.err);
        tls_unlock();
        teardown();                                   /* closes the socket too (the old path leaked one per try) */
        return;
    }
    note("VPN: connecting to %s", pr->host);
    bool announced = false, pinged = false;
    uint32_t last_ping = 0, up_at = 0;
    for (;;) {
        if (!enabled || pick_changed) break;
        drain_out();                              /* anything that queued while the session was held (the ping below) */
        /* auto: on a fallback server, ping again and go to the fastest every 15 min while no Dreamcast is online */
        if (announced && i != best_ && !pick_host[0] && now_ms() - up_at > 15u * 60 * 1000 && !dc_online()) {
            note("VPN: trying %s again (the fastest when last pinged)", prof[best_].host);
            break;
        }
        fd_set rs; FD_ZERO(&rs); FD_SET(vsock, &rs);
        struct timeval tv = { 0, 250000 };   /* data wakes select at once; this is only for the timers */
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
        drain_out();
        if (locked && (s >= OVPN_KEYS || s == OVPN_FAILED || now_ms() - lock_t > 15000)) { tls_unlock(); locked = false; }
        if (s == OVPN_FAILED) { snprintf(st.err, sizeof st.err, "%s", ovpn_error(vo)); st.fails++; note("VPN: %s", st.err); break; }
        if (s == OVPN_UP && !announced) {
            const ovpn_info_t *in = ovpn_info(vo);
            fmt_ip(st.ip, sizeof st.ip, in->ip);
            fmt_ip(st.gw, sizeof st.gw, in->gateway ? in->gateway : in->peer);
            vif_up(in);
            st.connects++;
            up_since_us = esp_timer_get_time();
            announced = true; up_at = now_ms();
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
    if (locked) tls_unlock();
    teardown();
}

/* auto: as DreamPi's openvpn-client, one ping to every server (1 s to answer), the fastest
   first; no answer = after those, in the list's order (shumania first) */
static void ping_order(void) {
    int32_t rtt[MAX_PROF];
    uint32_t addr[MAX_PROF];
    int64_t t0[MAX_PROF];
    for (int i = 0; i < nprof; i++) { rtt[i] = -1; addr[i] = 0; t0[i] = 0; }
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s >= 0) {
        for (int i = 0; i < nprof; i++) {
            struct addrinfo hints = { .ai_family = AF_INET }, *res = NULL;
            if (getaddrinfo(prof[i].host, NULL, &hints, &res) != 0 || !res) continue;
            addr[i] = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
            uint8_t e[16] = { 8, 0, 0, 0, PING_ID >> 8, PING_ID & 255, 0, (uint8_t)(0x40 + i), 'D', 'C', 'P', 'L' };
            uint16_t c = csum(e, sizeof e); e[2] = c >> 8; e[3] = c;
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = addr[i] };
            t0[i] = esp_timer_get_time();
            sendto(s, e, sizeof e, 0, (struct sockaddr *)&to, sizeof to);
        }
        int64_t end = esp_timer_get_time() + 1000000;
        for (int64_t now; (now = esp_timer_get_time()) < end;) {
            struct timeval tv = { 0, (long)(end - now) };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            uint8_t b[80];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            int n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            int h = (b[0] & 15) * 4;                          /* the raw socket gives the IP header too */
            if (n < h + 8 || b[h] != 0 || ((b[h + 4] << 8) | b[h + 5]) != PING_ID) continue;
            int i = b[h + 7] - 0x40;
            if (i < 0 || i >= nprof || addr[i] != from.sin_addr.s_addr || rtt[i] >= 0) continue;
            rtt[i] = (int32_t)((esp_timer_get_time() - t0[i]) / 1000);
        }
        close(s);
    }
    vpnpick_order(rtt, nprof, order_);
    best_ = order_[0];
    char m[200]; size_t o = 0;
    for (int i = 0; i < nprof && o < sizeof m - 40; i++) {
        int k = order_[i];
        o += (size_t)snprintf(m + o, sizeof m - o, "%s%s ", i ? ", " : "", prof[k].host);
        o += (size_t)(rtt[k] >= 0 ? snprintf(m + o, sizeof m - o, "%ld ms", (long)rtt[k]) : snprintf(m + o, sizeof m - o, "no answer"));
    }
    say_line("VPN: ping %s", m);
}

static void vpn_task(void *arg) {
    (void)arg;
    int fail_round = 0;
    for (;;) {
        if (!enabled || !nprof || !inet_up()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        pick_changed = false;
        int only = pick_index();
        /* a chosen server is the only one tried; auto: fastest ping first (ping_order) */
        if (only < 0) ping_order();
        for (int k = 0; k < (only >= 0 ? 1 : nprof) && enabled && !pick_changed; k++) {
            uint32_t c0 = st.connects;
            session(only >= 0 ? only : order_[k]);
            if (st.connects != c0) { fail_round = 0; break; }   /* was up, then lost: start again from the first */
            if (!enabled || pick_changed) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (enabled && !pick_changed) {
            fail_round++;
            vTaskDelay(pdMS_TO_TICKS(fail_round < 3 ? 10000 : 60000));
        }
    }
}

/* ---------- control and status ---------- */
static bool chosen_off;                               /* switched off on the page: never switched on by itself */
/* the box found a reason (IPv4 shared by the provider): on, unless switched off on the page */
void vpn_auto_on(const char *why) {
    if (!nprof) { say_line("VPN: would switch on (%s), but there are no VPN profiles yet", why); return; }
    if (enabled || chosen_off) return;
    enabled = true;
    say_line("VPN: switched on by itself (%s)", why);
}

void vpn_set_enabled(bool on) {
    enabled = on && nprof;
    chosen_off = !on;
    nvs_handle_t h;
    if (nvs_open("dcl", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "vpn", on); nvs_commit(h); nvs_close(h); }
}
bool vpn_enabled(void) { return enabled; }
void vpn_set_pick(const char *host) {
    char h[64]; snprintf(h, sizeof h, "%s", host ? host : "");
    if (!strcmp(h, "auto")) h[0] = 0;
    if (!strcmp(h, pick_host)) return;
    snprintf(pick_host, sizeof pick_host, "%s", h);
    nvs_handle_t n;
    if (nvs_open("dcl", NVS_READWRITE, &n) == ESP_OK) { nvs_set_str(n, "vpn_host", pick_host); nvs_commit(n); nvs_close(n); }
    say_line("VPN: server %s", pick_host[0] ? pick_host : "auto (the fastest ping, as DreamPi)");
    pick_changed = true;
}
int vpn_profiles_n(void) { return nprof; }

static char hosts_csv[MAX_PROF * 32];
size_t vpn_info_json(char *out, size_t cap) {
    bool up = up_since_us != 0;
    char dcip[16] = "";
    if (vpn_dc_addr()) fmt_ip(dcip, sizeof dcip, vpn_dc_addr());
    return (size_t)snprintf(out, cap,
        "{\"profiles\":%d,\"source\":\"%s\",\"dc_ip\":\"%s\",\"blocked\":%lu,\"on\":%s,\"up\":%s,\"server\":\"%s\",\"ip\":\"%s\",\"gateway\":\"%s\",\"ping_ms\":%d,"
        "\"connects\":%lu,\"fails\":%lu,\"error\":\"%s\",\"up_s\":%lu,\"pick\":\"%s\",\"hosts\":\"%s\",\"dc_in\":%lu,\"dc_out\":%lu,\"routes\":\"%s\"}",
        nprof, src_name, dcip, (unsigned long)st.blocked, enabled ? "true" : "false", up ? "true" : "false", st.server, st.ip, st.gw, st.ping_ms,
        (unsigned long)st.connects, (unsigned long)st.fails, st.err,
        up ? (unsigned long)((esp_timer_get_time() - up_since_us) / 1000000) : 0UL, pick_host[0] ? pick_host : "auto", hosts_csv, (unsigned long)dc_bytes_in, (unsigned long)dc_bytes_out, routes_txt);
}

void vpn_start(void) {
    lk = xSemaphoreCreateMutex();
    outq = xQueueCreate(16, sizeof(struct pbuf *));
    load_profiles();
    for (int i = 0; i < nprof; i++) {
        size_t l = strlen(hosts_csv), hn = strlen(prof[i].host);
        if (l + hn + 2 > sizeof hosts_csv) break;
        if (i) hosts_csv[l++] = ',';
        memcpy(hosts_csv + l, prof[i].host, hn + 1);
    }
    { nvs_handle_t n; size_t hl = sizeof pick_host;
      if (nvs_open("dcl", NVS_READONLY, &n) == ESP_OK) { if (nvs_get_str(n, "vpn_host", pick_host, &hl) != ESP_OK) pick_host[0] = 0; nvs_close(n); } }
    nvs_handle_t h; uint8_t v = 2;                       /* 2 = never chosen on the page */
    if (nvs_open("dcl", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "vpn", &v); nvs_close(h); }
    chosen_off = v == 0;
    enabled = nprof && v != 0;                           /* on by default when there are profiles, as on DreamPi 2.1 */
    if (nprof && v == 2) { const char *m = "VPN: on (the default with profiles; switch it off on the page if you do not want it)"; log_add("esp32", m, strlen(m)); }
    ESP_LOGI(TAG, "%d VPN profile(s) in this firmware, VPN %s", nprof, enabled ? "on" : "off");
    if (nprof) xTaskCreate(vpn_task, "vpn", 9216, NULL, 5, NULL);   /* 9 KB of stack (5.5 KB of 12 were never used) only when there is a VPN */
}
