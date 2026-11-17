/*
 * The Dreamcast's own address on the home network, the way a DreamPi does it (dreampi.py):
 *
 *  - the pick: the gateway's /24, counting down from .100 for the first two free addresses:
 *    this box's end of the PPP link (usually .99, also the Dreamcast's DNS) and the Dreamcast's
 *    (usually .98). DreamPi looks in its ARP table; here each candidate is also asked for with
 *    an ARP request, so an address that is in use but not yet in the table is not taken.
 *  - proxy ARP (pppd "proxyarp"): while the Dreamcast is connected this box answers ARP for its
 *    address on the uplink (Wi-Fi or the wired port), so the router sees the Dreamcast as a
 *    device of its own. Packets for it are routed to it, unchanged: no NAT here.
 *  - VPN up: the Dreamcast has tun+1 on PPP (as on DreamPi); what it sends outside the tunnel
 *    leaves as its home address and what comes in for that address goes to tun+1 (DreamPi's
 *    SNAT/DNAT rules, add_pseudo_interface_rules).
 *  - the cable's network and the wired BBA port (no DreamPi equivalent): the Dreamcast keeps its
 *    192.168.99.x / BBA lease there and is mapped 1:1 to the same home address, so UPnP and the
 *    router see the same thing whichever cable it uses.
 * UPnP (portfwd.c) points the game ports at this address, as DreamPi's port_forwarding.py does.
 * When no address can be picked (no uplink, nothing free) the Dreamcast stays behind this box's
 * NAT as before, and the log says so.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdarg.h>
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/prot/ethernet.h"
#include "netif/ethernet.h"
#include "dclan_model.h"
#include "app.h"

static volatile uint32_t lan_this, lan_dc;        /* picked addresses (host order), 0 = none */
static struct netif *volatile up_nif;             /* the uplink they belong to */
static uint32_t up_ip, up_gw;                     /* the uplink's address/gateway at the pick (host order) */
static struct netif *volatile in_nif;             /* where the Dreamcast is connected now, NULL = nowhere */
static volatile uint32_t inside;                  /* its address there (host order) */
static volatile bool conflict;                    /* another device answered for the Dreamcast's address */
static struct { uint32_t arp_answered, rewritten_out, rewritten_in, picks, conflicts; } cnt;
static char why_none[96] = "waiting for the home network";

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char t[180];
    va_list ap; va_start(ap, fmt); int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    log_add("esp32", t, (size_t)(k < (int)sizeof t ? k : (int)sizeof t - 1));
}
static const char *ips(uint32_t h, char *b) { snprintf(b, 16, "%u.%u.%u.%u", (unsigned)(h >> 24), (unsigned)(h >> 16 & 255), (unsigned)(h >> 8 & 255), (unsigned)(h & 255)); return b; }

bool dclan_addrs(uint32_t *this_ip, uint32_t *dc_ip) {
    uint32_t t = lan_this, d = lan_dc;
    if (!t || !d) return false;
    if (this_ip) *this_ip = t;
    if (dc_ip) *dc_ip = d;
    return true;
}
uint32_t dclan_dc_ip(void) { return lan_dc; }

/* ---------- lwIP hooks (tcpip thread, core locked) ---------- */
struct netif *dclan_route(const ip4_addr_t *dest) {
    struct netif *n = in_nif;
    if (!n || !dest || !inside) return NULL;
    return lwip_ntohl(ip4_addr_get_u32(dest)) == inside ? n : NULL;
}

static uint8_t pg(void *c, size_t o) { return pbuf_get_at((struct pbuf *)c, (u16_t)o); }
static void pp(void *c, size_t o, uint8_t v) { pbuf_put_at((struct pbuf *)c, (u16_t)o, v); }

static bool is_own_addr(uint32_t h) {
    struct netif *n;
    NETIF_FOREACH(n) if (lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n))) == h) return true;
    return false;
}

/* the 1:1 rewrite (DreamPi's SNAT/DNAT); never eats a packet */
int dclan_ip4_input(struct pbuf *p, struct netif *inp) {
    struct netif *dcn = in_nif;
    uint32_t in = inside, lan = lan_dc;
    if (!dcn || !in || !lan || in == lan || !p || p->len < 20) return 0;
    const uint8_t *h = (const uint8_t *)p->payload;
    if ((h[0] >> 4) != 4) return 0;
    uint32_t src = (uint32_t)h[12] << 24 | (uint32_t)h[13] << 16 | (uint32_t)h[14] << 8 | h[15];
    uint32_t dst = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
    dcl_pkt_t pk = { pg, pp, p, p->tot_len };
    if (inp == dcn && src == in) {
        if (is_own_addr(dst) || ip4_addr_ismulticast(&(ip4_addr_t){ .addr = lwip_htonl(dst) }) || dst == 0xffffffffu) return 0;
        ip4_addr_t s = { .addr = lwip_htonl(src) }, d = { .addr = lwip_htonl(dst) };
        struct netif *out = ip4_route_src(&s, &d);
        if (out && out != dcn && out != vpn_netif()) {                 /* not into the tunnel: leaves as the home address */
            dcl_rewrite(&pk, DCL_SRC, lan);
            cnt.rewritten_out++;
        }
    } else if (inp == up_nif && dst == lan) {                           /* for the home address: to where the Dreamcast is */
        dcl_rewrite(&pk, DCL_DST, in);
        cnt.rewritten_in++;
    }
    return 0;
}

/* proxy ARP: the uplink's frames pass here first (tcpip thread) */
static err_t eth_in(struct pbuf *p, struct netif *inp) {
    uint32_t lan = lan_dc;
    if (lan && inp == up_nif && p->len >= SIZEOF_ETH_HDR + 28) {
        const uint8_t *f = (const uint8_t *)p->payload;
        if (f[12] == 0x08 && f[13] == 0x06) {
            const uint8_t *a = f + SIZEOF_ETH_HDR;
            uint8_t mac[6]; uint32_t sip;
            if (dcl_arp_claims(a, 28, lan, inp->hwaddr)) {
                if (!conflict) cnt.conflicts++;
                conflict = true;
            } else if (in_nif && dcl_arp_asks_for(a, 28, lan, mac, &sip)) {
                struct pbuf *q = pbuf_alloc(PBUF_LINK, 28, PBUF_RAM);
                if (q) {
                    dcl_arp_reply((uint8_t *)q->payload, inp->hwaddr, lan, mac, sip);
                    ethernet_output(inp, q, (const struct eth_addr *)inp->hwaddr, (const struct eth_addr *)mac, ETHTYPE_ARP);
                    pbuf_free(q);
                    cnt.arp_answered++;
                }
            }
        }
    }
    return ethernet_input(p, inp);
}
static err_t wrapped_input(struct pbuf *p, struct netif *inp) { return tcpip_inpkt(p, inp, eth_in); }

static struct netif *wrapped[3];
static void wrap(struct netif *n) {                /* core locked */
    for (int i = 0; i < 3; i++) if (wrapped[i] == n) return;
    if (!(n->flags & NETIF_FLAG_ETHARP)) return;
    if (n->input != tcpip_input) return;           /* not the usual esp_netif set-up: leave it */
    for (int i = 0; i < 3; i++) if (!wrapped[i]) { wrapped[i] = n; n->input = wrapped_input; return; }
}

/* ---------- the pick ---------- */
static struct netif *probe_nif;
static bool used(void *ctx, uint32_t a) {
    (void)ctx;
    ip4_addr_t ip = { .addr = lwip_htonl(a) };
    struct eth_addr *ea; const ip4_addr_t *ia;
    for (int t = 0; t < 3; t++) {
        LOCK_TCPIP_CORE();
        bool known = etharp_find_addr(probe_nif, &ip, &ea, &ia) >= 0;
        if (!known && t < 2) etharp_request(probe_nif, &ip);
        UNLOCK_TCPIP_CORE();
        if (known) return true;
        if (t < 2) vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

static void pick(struct netif *n, uint32_t ip, uint32_t gw) {
    char a[16], b[16], c[16];
    probe_nif = n;
    uint32_t t, d;
    if (!gw) { snprintf(why_none, sizeof why_none, "the home network has no gateway"); lan_this = lan_dc = 0; return; }
    if (!dcl_pick(gw, ip, used, NULL, &t, &d)) {
        char w[96]; snprintf(w, sizeof w, "no free address in %s/24", ips(gw & 0xffffff00u, a));
        if (strcmp(w, why_none)) say("home network: no free address for the Dreamcast in %s/24; it stays behind this box's NAT", a);
        snprintf(why_none, sizeof why_none, "%s", w);
        lan_this = lan_dc = 0;
        return;
    }
    LOCK_TCPIP_CORE();
    wrap(n);
    up_nif = n;
    lan_this = t; lan_dc = d;
    UNLOCK_TCPIP_CORE();
    up_ip = ip; up_gw = gw; conflict = false; cnt.picks++;
    why_none[0] = 0;
    say("home network: the Dreamcast gets %s, this box is %s on its PPP link (as a DreamPi picks them, gateway %s)", ips(d, a), ips(t, b), ips(gw, c));
}

/* where is the Dreamcast connected now? (one at a time: PPP, then the cable's network, then the BBA port) */
static void track(void) {
    struct netif *n = NULL; uint32_t a = 0;
    uint32_t p = ppp_dc_ip();
    if (p && ppp_dc_lanmode()) { n = ppp_dc_netif(); a = lwip_ntohl(p); }
    else if (!p && dc_eth_dc_ip()) { n = dc_eth_netif(); a = lwip_ntohl(dc_eth_dc_ip()); }
    else if (!p && wired_dc_ip()) { n = wired_dc_netif(); a = lwip_ntohl(wired_dc_ip()); }
    if (!lan_dc) n = NULL;
    if (!n) a = 0;
    if (n == in_nif && a == inside) return;
    char x[16], y[16];
    LOCK_TCPIP_CORE();
    struct netif *old = in_nif;
    /* the Ethernet sides normally NAT; mapped 1:1 instead while the Dreamcast is there (the flag
       only: ip_napt_enable_no(.., 0) would free the NAT tables of every interface) */
    if (old && old != n && old != ppp_dc_netif()) old->napt = 1;
    if (n && n != ppp_dc_netif()) n->napt = 0;
    in_nif = n; inside = a;
    UNLOCK_TCPIP_CORE();
    if (n && a == lan_dc) say("home network: the Dreamcast is %s there; this box answers for it (proxy ARP), no NAT", ips(a, x));
    else if (n) say("home network: the Dreamcast (%s here) is %s at home; this box answers for it (proxy ARP)", ips(a, x), ips(lan_dc, y));
}

static void task(void *arg) {
    (void)arg;
    struct netif *for_n = NULL; uint32_t for_ip = 0, for_gw = 0;   /* what the last pick was for */
    int64_t failed_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_netif_t *en = esp_netif_get_default_netif();
        esp_netif_ip_info_t ii;
        bool have = inet_up() && en && esp_netif_get_ip_info(en, &ii) == ESP_OK && ii.ip.addr;
        struct netif *n = have ? (struct netif *)esp_netif_get_netif_impl(en) : NULL;
        uint32_t ip = have ? lwip_ntohl(ii.ip.addr) : 0, gw = have ? lwip_ntohl(ii.gw.addr) : 0;
        if (in_nif) {                                 /* never move the address under a connected Dreamcast */
            if (conflict && cnt.conflicts == 1) {
                char a[16]; say("home network: another device answers for %s too; a new address is picked after this call", ips(lan_dc, a));
                cnt.conflicts++;
            }
        } else if (!have) {
            if (lan_dc || for_n) { LOCK_TCPIP_CORE(); lan_this = lan_dc = 0; up_nif = NULL; UNLOCK_TCPIP_CORE(); for_n = NULL; }
            snprintf(why_none, sizeof why_none, "waiting for the home network");
        } else {
            bool changed = n != for_n || ip != for_ip || gw != for_gw;
            bool retry = !lan_dc && esp_timer_get_time() - failed_at > 60LL * 1000000;
            if (changed || conflict || retry) {
                if (conflict) { char a[16]; say("home network: another device answered for %s; picking again", ips(lan_dc, a)); }
                LOCK_TCPIP_CORE(); lan_this = lan_dc = 0; UNLOCK_TCPIP_CORE();
                conflict = false; cnt.conflicts = 0;
                for_n = n; for_ip = ip; for_gw = gw;
                pick(n, ip, gw);
                if (!lan_dc) failed_at = esp_timer_get_time();
            }
        }
        track();
    }
}

size_t dclan_info_json(char *o, size_t cap) {
    char a[16], b[16], c[16];
    return (size_t)snprintf(o, cap, "{\"dc\":\"%s\",\"this\":\"%s\",\"connected_as\":\"%s\",\"none\":\"%s\",\"arp_answered\":%lu,\"out\":%lu,\"in\":%lu,\"conflict\":%s}",
        lan_dc ? ips(lan_dc, a) : "", lan_this ? ips(lan_this, b) : "", in_nif ? ips(inside, c) : "", why_none,
        (unsigned long)cnt.arp_answered, (unsigned long)cnt.rewritten_out, (unsigned long)cnt.rewritten_in, conflict ? "true" : "false");
}

void dclan_start(void) { xTaskCreate(task, "dclan", 3072, NULL, 4, NULL); }
