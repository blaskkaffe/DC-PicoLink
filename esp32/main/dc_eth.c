/*
 * Internet for the Dreamcast over the cable's W5500 network (E-4).
 *
 * The cable emulates the W5500; its Ethernet frames come in channel ETH (the same frames a
 * Pi sees on USB). Here they are the "wire" of a second network interface, 192.168.98.1/24 (the Dreamcast network, shared with the BBA socket and PPP: ipplan_model.h)
 * (the same address a DreamPi uses on the cable), with a DHCP server (the Dreamcast gets
 * 192.168.98.2-.99), this box as gateway and DNS (dnsd.c), and NAT out through the Wi-Fi.
 * The TTL / PowerSmash rules (ipfilter_model.h) are applied to what the Dreamcast sends.
 * DCNow! reports run while the Dreamcast has sent frames in the last 30 seconds.
 *
 * Difference to the Pi: the Pi hands out the revival DNS server directly; here the DHCP offer
 * names this box, so DCNow! can see which game is looking up its server.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "link_model.h"
#include "ipfilter_model.h"
#include "app.h"
#include "ethsrc_model.h"
#include "ipplan_model.h"
#include "lwip/netif.h"
#include "dhcpserver/dhcpserver.h"
#include "ethtrace_model.h"
#include "lwip/lwip_napt.h"
#include "lwip/tcpip.h"

static const char *TAG = "dc_eth";
#define ETH_IP   IPP_BOX               /* 192.168.98.1 (ipplan_model.h) */
#define QUIET_US (30 * 1000000LL)

static esp_netif_t *eth_if;
static ipf_t ipf;                     /* only used from the link's reader task */
static ipf_t ipf_in;                  /* packets to the Dreamcast (lwIP's task): DreamPi helper rules */
static volatile int64_t last_dc_us = -QUIET_US * 2;
static volatile bool active;
static uint32_t lease_ip;              /* the Dreamcast's address: DHCP lease, or learnt from its frames */
static struct { uint32_t rx, tx, tx_drop, rx_drop, leases; char last_ip[16]; } cnt;

/* the first frames of each Dreamcast session, both ways, one line each in the log: shows where a
   game stops (no DNS question, an ARP without answer, a SYN without SYN-ACK) */
#define TRACE_N 12
static volatile int trace_in, trace_out;
static void trace(bool from_dc, const uint8_t *f, size_t n) {
    volatile int *c = from_dc ? &trace_in : &trace_out;
    if (*c >= TRACE_N) return;
    (*c)++;
    char l[120];
    ethtrace_line(f, n, l, sizeof l);
    say_line("Ethernet %s %s", from_dc ? "DC>" : ">DC", l);
}

static esp_err_t tx_cb(void *h, void *buf, size_t len) {
    (void)h;
    if (esp_timer_get_time() - last_dc_us < QUIET_US) trace(false, buf, len);
    if (!link_is_up() || len > 1514) { cnt.tx_drop++; return ESP_OK; }
    ipf_in.public_ip = pubip_get(); ipf_in.dnat = dnat_rules();
    ipf_ether_to_dc(&ipf_in, buf, len);         /* dc2k2 / dcvoip rules (ipfilter_model.h) */
    for (int i = 0; i < 20; i++) {
        if (link_send(LINK_CH_ETH, buf, len)) { cnt.tx++; return ESP_OK; }
        link_wait_room(10);           /* back as soon as the cable takes bytes (was a fixed 10 ms tick) */
    }
    cnt.tx_drop++;
    return ESP_OK;                    /* ARP / TCP try again */
}

static void free_cb(void *h, void *buf) { (void)h; free(buf); }

/* ---- the Dreamcast network (ipplan_model.h): the W5500 here, the BBA socket (wired.c) and a PPP
   call without its home address (ppp_srv.c) share 192.168.98.0/24; one Dreamcast uses one at a
   time, so what goes to an address there goes to the interface it was last heard on. ---- */
static struct netif *volatile dc_heard_nif;
void dcside_heard(struct netif *n) { if (n) dc_heard_nif = n; }
/* lwIP's routing hook (hooks/dcl_lwip_hooks.h; lwIP's thread) */
struct netif *dcside_route(const ip4_addr_t *dest) {
    if (!dest || !ipp_dc_host(lwip_ntohl(ip4_addr_get_u32(dest)))) return NULL;
    struct netif *n = dc_heard_nif;
    return n && netif_is_up(n) && netif_is_link_up(n) ? n : NULL;
}
/* NAT out for one interface, leaving the others as they are: ESP-IDF's esp_netif_napt_enable()
   switches NAT off on every other interface (with the BBA socket up, the cable's network lost its
   way out). lwIP's own call, under its lock. */
void dcside_nat_on(esp_netif_t *nif) {
    struct netif *n = (struct netif *)esp_netif_get_netif_impl(nif);
    if (!n) return;
    LOCK_TCPIP_CORE();
    ip_napt_enable_netif(n, 1);
    UNLOCK_TCPIP_CORE();
}
/* DHCP for the Dreamcast: .2-.99 (a fixed address at .100 and above never meets a lease); call with the server stopped */
void dcside_dhcp_pool(esp_netif_t *nif) {
    dhcps_lease_t l = { .enable = true };
    l.start_ip.addr = IPP_ESP(IPP_DHCP_FIRST);
    l.end_ip.addr = IPP_ESP(IPP_DHCP_LAST);
    esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &l, sizeof l);
}

/* a frame from the Dreamcast (link reader task) */
void eth_from_dc(const uint8_t *p, size_t n) {
    if (!eth_if || n < 14 || n > 1514) return;
    uint8_t *c = malloc(n);
    if (!c) { cnt.rx_drop++; return; }
    memcpy(c, p, n);
    ipf.public_ip = pubip_get(); ipf.dnat = dnat_rules();
    if (!ipf_ether(&ipf, c, n, (uint32_t)(esp_timer_get_time() / 1000))) { free(c); return; }
    if (!link_eth_is_computer()) {
        int64_t t = esp_timer_get_time();
        if (t - last_dc_us >= QUIET_US || ethtrace_is_discover(c, n)) trace_in = trace_out = 0;   /* a new session or program: trace it */
        trace(true, c, n);
        last_dc_us = t;   /* a computer on the cable's USB is not a Dreamcast session (DCNow!) */
        /* a Dreamcast with a fixed address never asks DHCP: learn it from its frames */
        dcside_heard(dc_eth_netif());          /* replies for the Dreamcast network go here now */
        uint32_t a = ethsrc_dc_ip(c, n, lwip_htonl(ETH_IP), lwip_htonl(IPP_MASK));
        if (a && a != lease_ip) {
            lease_ip = a;
            esp_ip4_addr_t ip = { .addr = a };
            snprintf(cnt.last_ip, sizeof cnt.last_ip, IPSTR, IP2STR(&ip));
            say_line("Ethernet: the Dreamcast uses " IPSTR " (a fixed address; gateway and DNS must be 192.168.98.1)", IP2STR(&ip));
        }
    }
    cnt.rx++;
    esp_netif_receive(eth_if, c, n, c);       /* lwIP frees it through free_cb */
}

uint32_t dc_eth_dc_ip(void) { return active ? lease_ip : 0; }
struct netif *dc_eth_netif(void) { return eth_if ? (struct netif *)esp_netif_get_netif_impl(eth_if) : NULL; }

static void on_lease(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_ap_staipassigned_t *e = data;
    if (e->esp_netif != eth_if) return;
    cnt.leases++;
    lease_ip = e->ip.addr;
    snprintf(cnt.last_ip, sizeof cnt.last_ip, IPSTR, IP2STR(&e->ip));
    say_line("Ethernet: the Dreamcast got " IPSTR " by DHCP", IP2STR(&e->ip));
}

static void watch_task(void *arg) {
    (void)arg;
    for (;;) {
        bool now = link_is_up() && esp_timer_get_time() - last_dc_us < QUIET_US;
        if (now != active) {
            active = now;
            const char *m = now ? "Ethernet: the Dreamcast is sending" : "Ethernet: the Dreamcast has been quiet for 30 s";
            log_add("esp32", m, strlen(m));
            dcnow_set_online(1, now);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

size_t eth_info_json(char *out, size_t cap) {
    return (size_t)snprintf(out, cap,
        "{\"up\":%s,\"active\":%s,\"frames_in\":%lu,\"frames_out\":%lu,\"out_dropped\":%lu,\"in_dropped\":%lu,"
        "\"leases\":%lu,\"dc_ip\":\"%s\",\"ttl_fixed\":%lu,\"syn_dropped\":%lu}",
        eth_if ? "true" : "false", active ? "true" : "false",
        (unsigned long)cnt.rx, (unsigned long)cnt.tx, (unsigned long)cnt.tx_drop, (unsigned long)cnt.rx_drop,
        (unsigned long)cnt.leases, cnt.last_ip, (unsigned long)ipf.ttl_fixed, (unsigned long)ipf.syn_dropped);
}

void dc_eth_start(void) {
    static const esp_netif_ip_info_t ipi = {
        .ip      = { .addr = IPP_ESP(IPP_BOX) },
        .netmask = { .addr = IPP_ESP(IPP_MASK) },
        .gw      = { .addr = IPP_ESP(IPP_BOX) },
    };
    static esp_netif_driver_ifconfig_t drv;
    drv.handle = &drv;                         /* any non-NULL value */
    drv.transmit = tx_cb;
    drv.driver_free_rx_buffer = free_cb;
    esp_netif_inherent_config_t base = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ipi,
        .get_ip_event = 0, .lost_ip_event = 0,
        .if_key = "DC_ETH", .if_desc = "dcl_eth",
        .route_prio = 5,                       /* below Wi-Fi: never the way out */
    };
    esp_read_mac(base.mac, ESP_MAC_ETH);
    esp_netif_config_t cfg = { .base = &base, .driver = &drv, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    eth_if = esp_netif_new(&cfg);
    if (!eth_if) { ESP_LOGE(TAG, "no memory for the interface"); return; }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_lease, NULL));
    esp_netif_action_start(eth_if, NULL, 0, NULL);        /* adds the lwIP interface and starts the DHCP server */

    /* hand out this box as DNS */
    esp_netif_dhcps_stop(eth_if);
    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = IPP_ESP(IPP_BOX) } };
    esp_netif_set_dns_info(eth_if, ESP_NETIF_DNS_MAIN, &dns);
    dcside_dhcp_pool(eth_if);
    uint8_t offer = 1;                                    /* OFFER_DNS */
    esp_netif_dhcps_option(eth_if, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(eth_if);

    dcside_nat_on(eth_if);                                /* NAT out through the Wi-Fi */
    dnsd_set_listen(1, true, ETH_IP);
    xTaskCreate(watch_task, "dc_eth", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "Ethernet side up: 192.168.98.1/24, DHCP, NAT");
}
