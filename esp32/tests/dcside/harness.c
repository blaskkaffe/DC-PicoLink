/*
 * Host test of the ESP32's Dreamcast network (dc_eth.c) with ESP-IDF's own lwIP: Ethernet + ARP,
 * NAPT and ESP-IDF's DHCP server, set up exactly as dc_eth_start() does (192.168.98.1/24, pool
 * .2-.99, DNS offered = this box), the routing hook of dcl_lwip_hooks.h (dcside_route), the frame
 * filters (ipfilter_model.h) both ways, and a DNS relay bound to 192.168.98.1:53 like dnsd.c.
 *
 * On one side a Dreamcast (a KOS-like client: DHCP or a fixed address, ARP, DNS, a UDP game query),
 * on the other the home network (Wi-Fi 192.168.1.186/24; a router at .1 that answers ARP, DNS and
 * plays the Quake III master server). The scenarios are the ones from the field logs (Nov 2026):
 * openMenu by DHCP, hang up and connect again, Quake III with a new MAC each start, a fixed address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_napt.h"
#include "lwip/prot/ethernet.h"
#include "netif/ethernet.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "ipplan_model.h"
#include "ipfilter_model.h"
#include "ethtrace_model.h"

static int fails, verbose;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* simulated clock: lwIP's timers run on it */
static u32_t now_ms;
u32_t sys_now(void) { return now_ms; }

/* ---------- frames on the two wires ---------- */
typedef struct { uint8_t b[1600]; size_t n; } frame_t;
#define QN 256
typedef struct { frame_t f[QN]; int rd, wr; } fq_t;
static fq_t to_dc, to_home;
static void q_put(fq_t *q, const uint8_t *p, size_t n) { if (q->wr - q->rd < QN && n <= 1600) { memcpy(q->f[q->wr % QN].b, p, n); q->f[q->wr % QN].n = n; q->wr++; } }
static frame_t *q_get(fq_t *q) { return q->rd < q->wr ? &q->f[q->rd++ % QN] : NULL; }

static struct netif eth, wifi;
static ipf_t ipf, ipf_in;
static uint8_t box_mac[6] = { 0x34, 0x85, 0x18, 0x00, 0x00, 0x03 }, wifi_mac[6] = { 0x34, 0x85, 0x18, 0x00, 0x00, 0x00 };
static uint8_t router_mac[6] = { 0xaa, 0xbb, 0xcc, 0x00, 0x00, 0x01 };

static err_t pbuf_to(fq_t *q, struct pbuf *p, bool to_dc_side) {
    uint8_t b[1600];
    size_t n = pbuf_copy_partial(p, b, sizeof b, 0);
    if (to_dc_side) {
        ipf_ether_to_dc(&ipf_in, b, n);
        if (verbose) { char l[160]; ethtrace_line(b, n, l, sizeof l); printf("   >DC %s\n", l); }
    }
    q_put(q, b, n);
    return ERR_OK;
}
static err_t eth_link_out(struct netif *n, struct pbuf *p) { (void)n; return pbuf_to(&to_dc, p, true); }
static err_t wifi_link_out(struct netif *n, struct pbuf *p) { (void)n; return pbuf_to(&to_home, p, false); }

static err_t eth_init(struct netif *n) {
    n->linkoutput = eth_link_out; n->output = etharp_output; n->mtu = 1500; n->hwaddr_len = 6;
    memcpy(n->hwaddr, box_mac, 6); n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->name[0] = 'd'; n->name[1] = 'c'; return ERR_OK;
}
static err_t wifi_init(struct netif *n) {
    n->linkoutput = wifi_link_out; n->output = etharp_output; n->mtu = 1500; n->hwaddr_len = 6;
    memcpy(n->hwaddr, wifi_mac, 6); n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->name[0] = 's'; n->name[1] = 't'; return ERR_OK;
}

/* dcl_lwip_hooks.h: the Dreamcast network goes to where it was last heard (dc_eth.c dcside_route) */
static struct netif *dc_heard_nif;
struct netif *sim_route_src(const struct ip4_addr *src, const struct ip4_addr *dest) {
    (void)src;
    if (!dest || !ipp_dc_host(lwip_ntohl(ip4_addr_get_u32(dest)))) return NULL;
    struct netif *n = dc_heard_nif;
    return n && netif_is_up(n) && netif_is_link_up(n) ? n : NULL;
}

/* eth_from_dc(): the filter, "heard here", into lwIP */
static void eth_from_dc(const uint8_t *p, size_t n) {
    uint8_t c[1600]; memcpy(c, p, n);
    if (!ipf_ether(&ipf, c, n, now_ms)) return;
    if (verbose) { char l[160]; ethtrace_line(c, n, l, sizeof l); printf("   DC> %s\n", l); }
    dc_heard_nif = &eth;
    struct pbuf *b = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    pbuf_take(b, c, (u16_t)n);
    if (eth.input(b, &eth) != ERR_OK) pbuf_free(b);
}

/* ---------- dnsd.c in short: a socket on 192.168.98.1:53, questions on to the home DNS ---------- */
static struct udp_pcb *dns_listen, *dns_up;
static ip_addr_t dns_client; static u16_t dns_client_port; static int dns_questions;
static void dns_from_dc(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    dns_questions++; dns_client = *addr; dns_client_port = port;
    ip_addr_t up; IP_ADDR4(&up, 192, 168, 1, 1);
    udp_sendto(dns_up, p, &up, 53);
    pbuf_free(p);
}
static void dns_from_home(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    udp_sendto(dns_listen, p, &dns_client, dns_client_port);
    pbuf_free(p);
}

/* ---------- the home network: the router at 192.168.1.1 ---------- */
static const uint8_t MASTER[4] = { 100, 10, 51, 236 };
static uint16_t csum(const uint8_t *p, size_t n) { uint32_t s = 0; for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]); if (n & 1) s += (uint32_t)p[n - 1] << 8; while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
/* an IPv4/UDP frame */
static size_t udp_frame(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], const uint8_t s[4], const uint8_t d[4],
                        unsigned sp, unsigned dp, const uint8_t *pl, size_t pn, uint8_t ttl) {
    memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 8; f[13] = 0;
    uint8_t *ip = f + 14; memset(ip, 0, 20);
    ip[0] = 0x45; put16(ip + 2, 20 + 8 + (unsigned)pn); ip[8] = ttl; ip[9] = 17;
    memcpy(ip + 12, s, 4); memcpy(ip + 16, d, 4); put16(ip + 10, csum(ip, 20));
    uint8_t *u = ip + 20; put16(u, sp); put16(u + 2, dp); put16(u + 4, 8 + (unsigned)pn); u[6] = u[7] = 0;
    memcpy(u + 8, pl, pn);
    return 14 + 20 + 8 + pn;
}
static size_t arp_frame(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], int op, const uint8_t sip[4], const uint8_t tmac[6], const uint8_t tip[4]) {
    memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 8; f[13] = 6;
    uint8_t *a = f + 14; put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, (unsigned)op);
    memcpy(a + 8, smac, 6); memcpy(a + 14, sip, 4); memcpy(a + 18, tmac, 6); memcpy(a + 24, tip, 4);
    return 42;
}
static int master_queries;
static void home_router(void) {
    frame_t *f;
    static const uint8_t rip[4] = { 192, 168, 1, 1 };
    while ((f = q_get(&to_home))) {
        uint8_t o[1600]; size_t on = 0;
        if (get16(f->b + 12) == 0x0806 && get16(f->b + 14 + 6) == 1 && !memcmp(f->b + 14 + 24, rip, 4)) {
            on = arp_frame(o, f->b + 6, router_mac, 2, rip, f->b + 6, f->b + 14 + 14);
        } else if (get16(f->b + 12) == 0x0800 && f->b[14 + 9] == 17) {
            const uint8_t *ip = f->b + 14, *u = ip + 20;
            unsigned dp = get16(u + 2), sp = get16(u);
            if (!memcmp(ip + 16, rip, 4) && dp == 53) {          /* DNS: every name is the master server */
                uint8_t a[512]; size_t qn = get16(u + 4) - 8; memcpy(a, u + 8, qn);
                a[2] = 0x81; a[3] = 0x80; a[7] = 1;
                static const uint8_t rr[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
                memcpy(a + qn, rr, sizeof rr); memcpy(a + qn + sizeof rr, MASTER, 4);
                on = udp_frame(o, f->b + 6, router_mac, rip, ip + 12, 53, sp, a, qn + sizeof rr + 4, 64);
            } else if (!memcmp(ip + 16, MASTER, 4)) {              /* the master server answers the query */
                master_queries++;
                static const uint8_t ans[] = "\xff\xff\xff\xffgetserversResponse\\";
                on = udp_frame(o, f->b + 6, router_mac, MASTER, ip + 12, dp, sp, ans, sizeof ans - 1, 50);
            }
        }
        if (on) {
            struct pbuf *b = pbuf_alloc(PBUF_RAW, (u16_t)on, PBUF_POOL);
            pbuf_take(b, o, (u16_t)on);
            if (wifi.input(b, &wifi) != ERR_OK) pbuf_free(b);
        }
    }
}

/* ---------- the Dreamcast: a KOS-like client on the W5500 ---------- */
typedef struct {
    uint8_t mac[6], ip[4], gw[4], dns[4], mask[4];
    bool have_ip; bool bcast_flag;
    uint32_t xid;
    uint8_t gw_mac[6]; bool gw_known;
    int dns_answers, master_answers, arp_answers;
    uint8_t got_dns_opt, got_router_opt;
    uint8_t offered[4]; int offers, acks, naks;
} dc_t;
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, ZERO4[4] = { 0 }, BC4[4] = { 255, 255, 255, 255 };

static void dc_send(const uint8_t *p, size_t n) {
    uint8_t c[1600]; memcpy(c, p, n);
    if (n < 60) { memset(c + n, 0, 60 - n); n = 60; }     /* the W5500 pads short frames */
    eth_from_dc(c, n);
}
static void dhcp_msg(dc_t *d, int type, const uint8_t req_ip[4]) {
    uint8_t m[300]; memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6; memcpy(m + 4, &d->xid, 4);
    if (d->bcast_flag) m[10] = 0x80;
    memcpy(m + 28, d->mac, 6);
    m[236] = 99; m[237] = 130; m[238] = 83; m[239] = 99;
    size_t o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = (uint8_t)type;
    if (req_ip) { m[o++] = 50; m[o++] = 4; memcpy(m + o, req_ip, 4); o += 4; m[o++] = 54; m[o++] = 4; m[o++] = 192; m[o++] = 168; m[o++] = 98; m[o++] = 1; }
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;
    m[o++] = 255;
    uint8_t f[600];
    size_t n = udp_frame(f, BCAST, d->mac, ZERO4, BC4, 68, 67, m, 300, 64);
    dc_send(f, n);
}
/* what the Dreamcast receives (frames for its MAC or broadcast) */
static void dc_receive(dc_t *d) {
    frame_t *f;
    while ((f = q_get(&to_dc))) {
        const uint8_t *b = f->b;
        if (memcmp(b, d->mac, 6) && memcmp(b, BCAST, 6)) continue;
        unsigned et = get16(b + 12);
        if (et == 0x0806) {
            const uint8_t *a = b + 14;
            if (get16(a + 6) == 2 && d->have_ip && !memcmp(a + 24, d->ip, 4)) { memcpy(d->gw_mac, a + 8, 6); d->gw_known = true; d->arp_answers++; }
            if (get16(a + 6) == 1 && d->have_ip && !memcmp(a + 24, d->ip, 4)) {   /* who has my address */
                uint8_t o[60]; size_t on = arp_frame(o, a + 8, d->mac, 2, d->ip, a + 8, a + 14); dc_send(o, on);
            }
            continue;
        }
        if (et != 0x0800 || b[14 + 9] != 17) continue;
        const uint8_t *ip = b + 14, *u = ip + 20;
        unsigned sp = get16(u), dp = get16(u + 2);
        if (sp == 67 && dp == 68) {
            const uint8_t *m = u + 8; size_t ml = get16(u + 4) - 8;
            if (memcmp(m + 4, &d->xid, 4)) continue;
            int type = 0; uint8_t dns[4] = { 0 }, rt[4] = { 0 }, mk[4] = { 0 }; bool hd = false, hr = false;
            for (size_t i = 240; i + 1 < ml && m[i] != 255;) {
                if (m[i] == 0) { i++; continue; }
                unsigned c = m[i], l = m[i + 1];
                if (c == 53) type = m[i + 2];
                if (c == 6 && l >= 4) { memcpy(dns, m + i + 2, 4); hd = true; }
                if (c == 3 && l >= 4) { memcpy(rt, m + i + 2, 4); hr = true; }
                if (c == 1 && l >= 4) memcpy(mk, m + i + 2, 4);
                i += 2 + l;
            }
            if (type == 2) { d->offers++; memcpy(d->offered, m + 16, 4); }
            if (type == 5) {
                d->acks++; memcpy(d->ip, m + 16, 4); d->have_ip = true;
                memcpy(d->dns, dns, 4); memcpy(d->gw, rt, 4); memcpy(d->mask, mk, 4);
                d->got_dns_opt = hd; d->got_router_opt = hr;
            }
            if (type == 6) d->naks++;
            continue;
        }
        if (!d->have_ip || memcmp(ip + 16, d->ip, 4)) continue;
        if (sp == 53) d->dns_answers++;
        else if (!memcmp(ip + 12, MASTER, 4)) d->master_answers++;
    }
}
static void run_ms(int ms) {
    for (int i = 0; i < ms; i++) { now_ms++; sys_check_timeouts(); home_router(); }
}
/* DHCP like KOS: DISCOVER, then REQUEST for what was offered */
static bool dc_dhcp(dc_t *d) {
    d->xid = (uint32_t)rand();
    d->offers = d->acks = 0;
    for (int t = 0; t < 4 && !d->offers; t++) { dhcp_msg(d, 1, NULL); run_ms(50); dc_receive(d); if (!d->offers) run_ms(1000); }
    if (!d->offers) return false;
    for (int t = 0; t < 4 && !d->acks; t++) { dhcp_msg(d, 3, d->offered); run_ms(50); dc_receive(d); }
    return d->acks > 0;
}
/* ARP for the gateway, a DNS question, a UDP query to the master server */
static void dc_online(dc_t *d, const char *name) {
    d->gw_known = false;
    for (int t = 0; t < 3 && !d->gw_known; t++) {
        uint8_t o[60]; size_t on = arp_frame(o, BCAST, d->mac, 1, d->ip, (const uint8_t[6]){ 0 }, d->dns);
        dc_send(o, on); run_ms(20); dc_receive(d);
    }
    if (!d->gw_known) return;
    uint8_t q[64]; memset(q, 0, sizeof q); q[0] = 0x12; q[1] = 0x34; q[2] = 1; q[5] = 1;
    size_t qn = 12; const char *s = name;
    while (*s) { const char *e = strchr(s, '.'); size_t l = e ? (size_t)(e - s) : strlen(s); q[qn++] = (uint8_t)l; memcpy(q + qn, s, l); qn += l; s += l; if (*s) s++; }
    q[qn++] = 0; q[qn++] = 0; q[qn++] = 1; q[qn++] = 0; q[qn++] = 1;
    uint8_t f[600];
    dc_send(f, udp_frame(f, d->gw_mac, d->mac, d->ip, d->dns, 1024, 53, q, qn, 64));
    run_ms(30); dc_receive(d);
    static const uint8_t gs[] = "\xff\xff\xff\xffgetservers 68 empty full";
    dc_send(f, udp_frame(f, d->gw_mac, d->mac, d->ip, MASTER, 27960, 27950, gs, sizeof gs - 1, 64));
    run_ms(30); dc_receive(d);
}
static void dc_new(dc_t *d, const uint8_t mac[6], bool bcast) { memset(d, 0, sizeof *d); memcpy(d->mac, mac, 6); d->bcast_flag = bcast; }

/* esp_netif's lease callback (dc_eth.c on_lease logs "the Dreamcast got ... by DHCP") */
static int leases;
static void on_lease(void *arg, u8_t ip[4], u8_t mac[6]) { (void)arg; (void)mac; leases++; if (verbose) printf("   lease %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]); }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    verbose = argc > 1;
    lwip_init();
    memset(&ipf, 0, sizeof ipf); memset(&ipf_in, 0, sizeof ipf_in);
    ip4_addr_t a, m, g;
    /* the Wi-Fi uplink (route_prio 100 on the ESP32: the default way out) */
    IP4_ADDR(&a, 192, 168, 1, 186); IP4_ADDR(&m, 255, 255, 255, 0); IP4_ADDR(&g, 192, 168, 1, 1);
    netif_add(&wifi, &a, &m, &g, NULL, wifi_init, ethernet_input);
    netif_set_up(&wifi); netif_set_link_up(&wifi); netif_set_default(&wifi);
    /* dc_eth_start(): 192.168.98.1/24, gateway itself */
    a.addr = lwip_htonl(IPP_BOX); m.addr = lwip_htonl(IPP_MASK); g.addr = lwip_htonl(IPP_BOX);
    netif_add(&eth, &a, &m, &g, NULL, eth_init, ethernet_input);
    netif_set_up(&eth); netif_set_link_up(&eth);
    ip_napt_enable_netif(&eth, 1);                                      /* dcside_nat_on */
    dhcps_t *dh = dhcps_new();
    ip_addr_t dnsip; IP_ADDR4(&dnsip, 192, 168, 98, 1);
    dhcps_dns_setserver(dh, &dnsip);                                     /* esp_netif_set_dns_info */
    dhcps_lease_t l = { .enable = true };
    l.start_ip.addr = lwip_htonl(IPP_DHCP_FIRST); l.end_ip.addr = lwip_htonl(IPP_DHCP_LAST);
    dhcps_set_option_info(dh, REQUESTED_IP_ADDRESS, &l, sizeof l);       /* dcside_dhcp_pool */
    dhcps_offer_t offer = OFFER_DNS;
    dhcps_set_option_info(dh, DOMAIN_NAME_SERVER, &offer, sizeof offer);
    dhcps_set_new_lease_cb(dh, on_lease, NULL);                          /* esp_netif_dhcps_start */
    dhcps_set_option_info(dh, SUBNET_MASK, &m, sizeof m);
    CHECK(dhcps_start(dh, &eth, a) == ERR_OK, "DHCP server did not start");
    dns_listen = udp_new(); udp_bind(dns_listen, &eth.ip_addr, 53); udp_recv(dns_listen, dns_from_dc, NULL);
    dns_up = udp_new(); udp_bind(dns_up, IP_ADDR_ANY, 0); udp_recv(dns_up, dns_from_home, NULL);

    dc_t d;
    static const uint8_t om_mac[6] = { 0x02, 0x16, 0xe4, 0x11, 0x22, 0x33 };
    /* 1. openMenu by DHCP (both kinds of client: broadcast flag set and not) */
    for (int bc = 0; bc < 2; bc++) {
        dc_new(&d, om_mac, bc);
        bool ok = dc_dhcp(&d);
        CHECK(ok, "openMenu (broadcast flag %d): no DHCP lease", bc);
        if (!ok) continue;
        CHECK(d.got_dns_opt && !memcmp(d.dns, (uint8_t[4]){ 192, 168, 98, 1 }, 4), "DHCP gave DNS %u.%u.%u.%u (option %s)", d.dns[0], d.dns[1], d.dns[2], d.dns[3], d.got_dns_opt ? "present" : "missing");
        CHECK(d.got_router_opt && !memcmp(d.gw, (uint8_t[4]){ 192, 168, 98, 1 }, 4), "DHCP gave gateway %u.%u.%u.%u", d.gw[0], d.gw[1], d.gw[2], d.gw[3]);
        dc_online(&d, "dcnow.dreamcast.online");
        printf("%s openMenu by DHCP (broadcast flag %d): %u.%u.%u.%u, DNS %u.%u.%u.%u, ARP %d, DNS answers %d, master answers %d\n",
               d.dns_answers && d.master_answers ? "PASS" : "FAIL", bc, d.ip[0], d.ip[1], d.ip[2], d.ip[3], d.dns[0], d.dns[1], d.dns[2], d.dns[3],
               d.arp_answers, d.dns_answers, d.master_answers);
        fails += !(d.dns_answers && d.master_answers);
        /* 2. hang up and connect again, the same program (same MAC): a new DISCOVER */
        run_ms(5000);
        uint8_t old[4]; memcpy(old, d.ip, 4);
        dc_new(&d, om_mac, bc);
        ok = dc_dhcp(&d);
        printf("%s openMenu again after hanging up (broadcast flag %d): %s %u.%u.%u.%u%s\n", ok ? "PASS" : "FAIL", bc, ok ? "lease" : "no DHCP answer",
               d.ip[0], d.ip[1], d.ip[2], d.ip[3], ok && memcmp(old, d.ip, 4) ? " (a different address)" : "");
        fails += !ok;
        if (ok) { dc_online(&d, "dcnow.dreamcast.online"); CHECK(d.dns_answers && d.master_answers, "after reconnecting: DNS %d master %d", d.dns_answers, d.master_answers); }
    }
    /* 3. Quake III: a new MAC on each start (02:44:43:xx:4c:00 in the logs), twelve starts */
    int q3_ok = 0;
    for (int i = 0; i < 12; i++) {
        uint8_t mac[6] = { 0x02, 0x44, 0x43, (uint8_t)(0xa3 + i * 7), 0x4c, 0x00 };
        dc_new(&d, mac, false);
        if (dc_dhcp(&d)) { dc_online(&d, "master.id-q3a.games.sega.net"); if (d.dns_answers && d.master_answers) q3_ok++; else if (verbose) printf("   Q3 start %d: DNS %d master %d\n", i, d.dns_answers, d.master_answers); }
        else if (verbose) printf("   Q3 start %d: no lease\n", i);
        run_ms(3000);
    }
    printf("%s Quake III, twelve starts with a new MAC each: %d online\n", q3_ok == 12 ? "PASS" : "FAIL", q3_ok);
    fails += q3_ok != 12;
    /* 4. a fixed address (the user's setting): 192.168.98.100, gateway and DNS 192.168.98.1 */
    dc_new(&d, (uint8_t[6]){ 0x02, 0x44, 0x43, 0x55, 0x4c, 0x00 }, false);
    memcpy(d.ip, (uint8_t[4]){ 192, 168, 98, 100 }, 4); d.have_ip = true;
    memcpy(d.dns, (uint8_t[4]){ 192, 168, 98, 1 }, 4); memcpy(d.gw, d.dns, 4);
    dc_online(&d, "master.id-q3a.games.sega.net");
    printf("%s fixed address 192.168.98.100: DNS answers %d, master answers %d\n", d.dns_answers && d.master_answers ? "PASS" : "FAIL", d.dns_answers, d.master_answers);
    fails += !(d.dns_answers && d.master_answers);
    printf("dcside: %d leases, %d DNS questions relayed, %d master queries; %s\n", leases, dns_questions, master_queries, fails ? "FAILURES" : "all ok");
    return fails != 0;
}
