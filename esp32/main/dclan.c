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
#include "lwip/sockets.h"
#include "lwip/lwip_napt.h"
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
static struct { uint32_t arp_answered, arp_seen, rewritten_out, rewritten_in, from_dc, to_home, picks, conflicts; } cnt;
static volatile uint32_t first_out_ms;              /* when the Dreamcast first sent out this call (0 = not yet) */
static bool said_nothing_back, said_first_in;
static uint32_t bad_ip, bad_gw;                  /* a network that took the home address but delivered nothing back: NAT there (until the network changes) */
static char why_none[96] = "waiting for the home network";
static volatile uint16_t ah_port;                 /* local TCP port of the request, 0 = none */
static volatile uint32_t ah_peer;                 /* the router (host order) */
static volatile uint8_t ah_proto = 6;             /* TCP (UPnP) or UDP (the check below) */

#define say say_line
static const char *ips(uint32_t h, char *b) { snprintf(b, 16, "%u.%u.%u.%u", (unsigned)(h >> 24), (unsigned)(h >> 16 & 255), (unsigned)(h >> 8 & 255), (unsigned)(h & 255)); return b; }

bool dclan_addrs(uint32_t *this_ip, uint32_t *dc_ip) {
    uint32_t t = lan_this, d = lan_dc;
    if (!t || !d) return false;
    if (this_ip) *this_ip = t;
    if (dc_ip) *dc_ip = d;
    return true;
}
uint32_t dclan_dc_ip(void) { return lan_dc; }
/* has the pick for the current home network been made (an address, or a reason for none)? */
bool dclan_decided(void) { return (lan_dc && lan_this) || (why_none[0] && strcmp(why_none, "waiting for the home network")); }

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

/* the first places the Dreamcast talks to in a call, for the log (which DNS server, which game
   server): filled here on lwIP's thread, written to the log by the task */
#define SEEN_N 8
static struct { uint32_t ip; uint16_t port; uint8_t proto; } seen[SEEN_N];
static volatile int seen_n, seen_logged;
/* how many different outside addresses the Dreamcast tried this call (one dead server is no proof
   that the network drops answers: the Quake master behind a filtered DNS looked exactly like that) */
static int outside_dests(void) {
    int k = 0;
    for (int i = 0; i < seen_n; i++) {
        uint32_t a = seen[i].ip; bool dup = false;
        if (a == lan_this || is_own_addr(a)) continue;
        for (int j = 0; j < i; j++) if (seen[j].ip == a) dup = true;
        if (!dup) k++;
    }
    return k;
}
static void note_dest(const uint8_t *h, size_t n, uint32_t dst) {
    if (seen_n >= SEEN_N || n < 20) return;
    size_t hl = (size_t)(h[0] & 15) * 4;
    uint8_t pr = h[9];
    uint16_t port = (pr == 6 || pr == 17) && n >= hl + 4 ? (uint16_t)(h[hl + 2] << 8 | h[hl + 3]) : 0;
    for (int i = 0; i < seen_n; i++) if (seen[i].ip == dst && seen[i].port == port && seen[i].proto == pr) return;
    seen[seen_n].ip = dst; seen[seen_n].port = port; seen[seen_n].proto = pr;
    seen_n++;
}

/* the 1:1 rewrite (DreamPi's SNAT/DNAT); never eats a packet */
int dclan_ip4_input(struct pbuf *p, struct netif *inp) {
    struct netif *dcn = in_nif;
    uint32_t in = inside, lan = lan_dc;
    if (!lan || !p || p->len < 20) return 0;
    const uint8_t *h = (const uint8_t *)p->payload;
    if ((h[0] >> 4) != 4) return 0;
    uint32_t src = (uint32_t)h[12] << 24 | (uint32_t)h[13] << 16 | (uint32_t)h[14] << 8 | h[15];
    uint32_t dst = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
    uint16_t ap = ah_port;
    if (ap && inp == up_nif && dst == lan && src == ah_peer && h[9] == ah_proto) {   /* the answer to a request sent as the home address: to this box */
        size_t hl = (size_t)(h[0] & 15) * 4;
        if (p->tot_len >= hl + 4 && ((uint16_t)pbuf_get_at(p, (u16_t)(hl + 2)) << 8 | pbuf_get_at(p, (u16_t)(hl + 3))) == ap) {
            dcl_pkt_t pk = { pg, pp, p, p->tot_len };
            dcl_rewrite(&pk, DCL_DST, lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(inp))));
            return 0;
        }
    }
    if (!dcn || !in) return 0;
    if (inp == dcn && src == in) {
        note_dest(h, p->len, dst);
        if (!is_own_addr(dst)) { cnt.from_dc++; if (!first_out_ms) first_out_ms = (uint32_t)(esp_timer_get_time() / 1000) | 1; }
    }
    if (inp == up_nif && dst == lan) cnt.to_home++;                       /* something for its home address came */
    if (in == lan) return 0;                                            /* its home address on PPP: nothing to rewrite */
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

/* UPnP from the Dreamcast's address: routers in "secure mode" (miniupnpd's default, OpenWrt,
   many others) only map ports to the address that asks. This box holds the Dreamcast's address
   (it answers ARP for it), so portfwd.c's requests go out from it: one TCP connection at a time,
   marked by its local port, rewritten on the way out (from this box's address to the home one)
   and back in. */
void dclan_as_home(uint16_t port, uint32_t peer_n) { ah_peer = lwip_ntohl(peer_n); ah_proto = 6; ah_port = port; }
static void as_home_udp(uint16_t port, uint32_t peer_h) { ah_peer = peer_h; ah_proto = 17; ah_port = port; }
void dclan_as_home_end(void) { ah_port = 0; }

static netif_output_fn up_output_orig;
static err_t up_output(struct netif *n, struct pbuf *p, const ip4_addr_t *dst) {
    uint16_t ap = ah_port; uint32_t lan = lan_dc;
    if (ap && lan && n == up_nif && p && p->len >= 24) {
        const uint8_t *h = (const uint8_t *)p->payload;
        size_t hl = (size_t)(h[0] & 15) * 4;
        uint32_t s = (uint32_t)h[12] << 24 | (uint32_t)h[13] << 16 | (uint32_t)h[14] << 8 | h[15];
        uint32_t d = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
        if ((h[0] >> 4) == 4 && h[9] == ah_proto && p->tot_len >= hl + 8 && d == ah_peer &&
            s == lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n))) &&
            ((uint16_t)pbuf_get_at(p, (u16_t)hl) << 8 | pbuf_get_at(p, (u16_t)(hl + 1))) == ap) {
            dcl_pkt_t pk = { pg, pp, p, p->tot_len };
            dcl_rewrite(&pk, DCL_SRC, lan);
        }
    }
    return up_output_orig(n, p, dst);
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
            } else if (dcl_arp_asks_for(a, 28, lan, mac, &sip)) {      /* the address is this box's to answer for once picked */
                cnt.arp_seen++;
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
/* "lan is at our MAC" to everyone (gratuitous ARP request): access points that answer ARP for
   their clients (proxy ARP, mesh systems) learn the second address only this way. Core locked. */
static void announce(struct netif *n, uint32_t lan) {
    static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, zero[6] = { 0 };
    struct pbuf *q = pbuf_alloc(PBUF_LINK, 28, PBUF_RAM);
    if (!q) return;
    uint8_t *a = (uint8_t *)q->payload;
    dcl_arp_reply(a, n->hwaddr, lan, zero, lan);
    a[7] = 1;                                                    /* a request, as gratuitous ARP is sent */
    ethernet_output(n, q, (const struct eth_addr *)n->hwaddr, (const struct eth_addr *)bc, ETHTYPE_ARP);
    pbuf_free(q);
}
static err_t wrapped_input(struct pbuf *p, struct netif *inp) { return tcpip_inpkt(p, inp, eth_in); }

static struct netif *wrapped[3];
static void wrap(struct netif *n) {                /* core locked */
    for (int i = 0; i < 3; i++) if (wrapped[i] == n) return;
    if (!(n->flags & NETIF_FLAG_ETHARP)) return;
    if (n->input != tcpip_input) return;           /* not the usual esp_netif set-up: leave it */
    for (int i = 0; i < 3; i++) if (!wrapped[i]) {
        wrapped[i] = n; n->input = wrapped_input;
        if (!up_output_orig || n->output == up_output_orig) { up_output_orig = n->output; n->output = up_output; }   /* etharp_output for all of them */
        return;
    }
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

/* One DNS question from this box's address, then the same from `lan`. 1 = the answer to lan
   came back, 0 = only this box's came back (the network drops the second address), -1 = no
   answer at all (nothing known: lan is used, as on DreamPi). */
static int ask_dns(uint32_t server, bool as_lan) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
    socklen_t ml = sizeof me;
    int got = -1;
    if (bind(s, (struct sockaddr *)&me, sizeof me) == 0 && getsockname(s, (struct sockaddr *)&me, &ml) == 0) {
        struct timeval tv = { .tv_sec = 2 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (as_lan) as_home_udp(ntohs(me.sin_port), server);
        static const uint8_t q[] = { 0x44, 0x43, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,                 /* id "DC", RD, 1 question */
                                     13, 'd', 'r', 'e', 'a', 'm', 'c', 'a', 's', 't', 'l', 'i', 'v', 'e', 3, 'n', 'e', 't', 0, 0, 1, 0, 1 };
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = lwip_htonl(server) };
        static uint8_t b[512];                                   /* the dclan task only */
        for (int tries = 0; tries < 2 && got < 0; tries++) {
            sendto(s, q, sizeof q, 0, (struct sockaddr *)&to, sizeof to);
            int k = recv(s, b, sizeof b, 0);
            if (k >= 12 && b[0] == 0x44 && b[1] == 0x43) got = 1;
        }
        if (as_lan) dclan_as_home_end();
    }
    close(s);
    return got;
}
static int check_home(struct netif *n, uint32_t lan) {
    (void)n; (void)lan;
    static const uint32_t servers[] = { 0x2e655b7bu /* 46.101.91.123, DreamPi's */, 0x01010101u, 0x08080808u };
    for (size_t i = 0; i < sizeof servers / sizeof servers[0]; i++) {
        if (ask_dns(servers[i], false) < 0) continue;                 /* this server does not answer even this box */
        return ask_dns(servers[i], true) > 0 ? 1 : 0;
    }
    return -1;
}
static void pick(struct netif *n, uint32_t ip, uint32_t gw, uint32_t mask) {
    char a[16], b[16], c[16];
    probe_nif = n;
    uint32_t t, d;
    if (!gw) { snprintf(why_none, sizeof why_none, "the home network has no gateway"); lan_this = lan_dc = 0; return; }
    if (!dcl_pick(gw, ip, mask, used, NULL, &t, &d)) {
        char w[96]; snprintf(w, sizeof w, "no free address in %s/24", ips(gw & 0xffffff00u, a));
        if (strcmp(w, why_none)) say("home network: no free address for the Dreamcast in %s/24; it stays behind this box's NAT", a);
        snprintf(why_none, sizeof why_none, "%s", w);
        lan_this = lan_dc = 0;
        return;
    }
    if (bad_ip == ip && bad_gw == gw) {              /* this network already failed in a real call */
        snprintf(why_none, sizeof why_none, "this network did not deliver answers to the home address");
        lan_this = lan_dc = 0;
        return;
    }
    /* does the network deliver to a second address on this box? (phone hotspots, guest and mesh
       Wi-Fi often do not: then NAT, as before) */
    LOCK_TCPIP_CORE(); wrap(n); up_nif = n; lan_dc = d; UNLOCK_TCPIP_CORE();     /* answer ARP for it during the check */
    int ok = check_home(n, d);
    if (ok == 0) {
        LOCK_TCPIP_CORE(); lan_dc = 0; UNLOCK_TCPIP_CORE();
        char w[96]; snprintf(w, sizeof w, "this network does not deliver to a second address (hotspot, guest or mesh Wi-Fi?)");
        if (strcmp(w, why_none)) say("home network: answers to %s do not come back here, so this network delivers only to this box's own address (a phone hotspot, guest or mesh Wi-Fi does that); the Dreamcast stays behind this box's NAT", ips(d, a));
        snprintf(why_none, sizeof why_none, "%s", w);
        return;
    }
    LOCK_TCPIP_CORE();
    wrap(n);
    up_nif = n;
    lan_this = t; lan_dc = d;
    UNLOCK_TCPIP_CORE();
    up_ip = ip; up_gw = gw; conflict = false; cnt.picks++;
    why_none[0] = 0;
    say("home network: the Dreamcast gets %s, this box is %s on its PPP link (as a DreamPi picks them, gateway %s)%s", ips(d, a), ips(t, b), ips(gw, c),
        ok < 0 ? "; could not check that answers reach it (no DNS answer at all)" : "; checked: answers reach it");
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
    seen_n = 0; seen_logged = 0;
    first_out_ms = 0; said_nothing_back = said_first_in = false;
    if (n && up_nif) announce(up_nif, lan_dc);
    UNLOCK_TCPIP_CORE();
    if (n && a == lan_dc) say("home network: the Dreamcast is %s there; this box answers for it (proxy ARP, announced), no NAT", ips(a, x));
    else if (n) say("home network: the Dreamcast (%s here) is %s at home; this box answers for it (proxy ARP, announced)", ips(a, x), ips(lan_dc, y));
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
        if (in_nif || ppp_dc_ip() || dc_eth_dc_ip() || wired_dc_ip()) {   /* never move or change the address under a connected Dreamcast (NAT calls too) */
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
                pick(n, ip, gw, have ? lwip_ntohl(ii.netmask.addr) : 0);
                if (!lan_dc) failed_at = esp_timer_get_time();
            }
        }
        track();
        if (in_nif) {                                 /* does anything come back for the home address? */
            static uint32_t home0; if (!first_out_ms) home0 = cnt.to_home;
            char a[16];
            if (!said_first_in && cnt.to_home != home0 && first_out_ms) { said_first_in = true; say("home network: answers for %s arrive (the router delivers to it)", ips(lan_dc, a)); }
            else if (!said_first_in && !said_nothing_back && first_out_ms && (uint32_t)(esp_timer_get_time() / 1000) - first_out_ms > 15000 && cnt.from_dc >= 3 && outside_dests() >= 2) {
                said_nothing_back = true;
                say("home network: nothing has come back for %s in 15 s from %d servers (%lu sent out; the router asked for it %lu times): this network does not deliver to a second address on this box. Switching this call to NAT, and this network stays on NAT",
                    ips(lan_dc, a), outside_dests(), (unsigned long)cnt.from_dc, (unsigned long)cnt.arp_seen);
                bad_ip = ip; bad_gw = gw;
                struct netif *pn = ppp_dc_ip() ? ppp_dc_netif() : NULL;
                LOCK_TCPIP_CORE(); lan_this = lan_dc = 0; UNLOCK_TCPIP_CORE();
                if (pn) ip_napt_enable_no(pn->num, 1);            /* a PPP call in LAN mode had no NAT: on, as for a call without a home address */
                snprintf(why_none, sizeof why_none, "this network did not deliver answers to the home address");
            }
        }
        while (seen_logged < seen_n) {                /* where the Dreamcast goes (first few per call) */
            int i = seen_logged++;
            char a[16]; uint32_t ip = seen[i].ip; bool own = false;
            uint32_t t_ = lan_this; own = ip == t_;
            const char *pr = seen[i].proto == 6 ? "TCP" : seen[i].proto == 17 ? "UDP" : seen[i].proto == 1 ? "ICMP" : "IP";
            if (seen[i].port) say("home network: the Dreamcast talks to %s:%u (%s)%s", ips(ip, a), (unsigned)seen[i].port, pr,
                                  seen[i].port == 53 && !own && ip != inside + 1 ? ": a DNS server of its own settings, not this box" : "");
            else say("home network: the Dreamcast talks to %s (%s)", ips(ip, a), pr);
        }
    }
}

size_t dclan_info_json(char *o, size_t cap) {
    char a[16], b[16], c[16];
    return (size_t)snprintf(o, cap, "{\"dc\":\"%s\",\"this\":\"%s\",\"connected_as\":\"%s\",\"none\":\"%s\",\"arp_answered\":%lu,\"arp_seen\":%lu,\"from_dc\":%lu,\"to_home\":%lu,\"out\":%lu,\"in\":%lu,\"conflict\":%s}",
        lan_dc ? ips(lan_dc, a) : "", lan_this ? ips(lan_this, b) : "", in_nif ? ips(inside, c) : "", why_none,
        (unsigned long)cnt.arp_answered, (unsigned long)cnt.arp_seen, (unsigned long)cnt.from_dc, (unsigned long)cnt.to_home, (unsigned long)cnt.rewritten_out, (unsigned long)cnt.rewritten_in, conflict ? "true" : "false");
}

void dclan_start(void) { xTaskCreate(task, "dclan", 3072, NULL, 4, NULL); }
