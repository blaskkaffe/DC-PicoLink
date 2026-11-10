/*
 * Internet for the Dreamcast over the cable's W5500 network (E-4).
 *
 * The cable emulates the W5500; its Ethernet frames come in channel ETH (the same frames a
 * Pi sees on USB). Here they are the "wire" of a second network interface, 192.168.99.1/24
 * (the same address a DreamPi uses on the cable), with a DHCP server (the Dreamcast gets
 * 192.168.99.2 and up), this box as gateway and DNS (dnsd.c), and NAT out through the Wi-Fi.
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
#include "link_model.h"
#include "ipfilter_model.h"
#include "app.h"

static const char *TAG = "dc_eth";
#define ETH_IP   0xC0A86301u          /* 192.168.99.1 */
#define QUIET_US (30 * 1000000LL)

static esp_netif_t *eth_if;
static ipf_t ipf;                     /* only used from the link's reader task */
static volatile int64_t last_dc_us = -QUIET_US * 2;
static volatile bool active;
static struct { uint32_t rx, tx, tx_drop, rx_drop, leases; char last_ip[16]; } cnt;

static esp_err_t tx_cb(void *h, void *buf, size_t len) {
    (void)h;
    if (!link_is_up() || len > 1514) { cnt.tx_drop++; return ESP_OK; }
    for (int i = 0; i < 20; i++) {
        if (link_send(LINK_CH_ETH, buf, len)) { cnt.tx++; return ESP_OK; }
        vTaskDelay(1);
    }
    cnt.tx_drop++;
    return ESP_OK;                    /* ARP / TCP try again */
}

static void free_cb(void *h, void *buf) { (void)h; free(buf); }

/* a frame from the Dreamcast (link reader task) */
void eth_from_dc(const uint8_t *p, size_t n) {
    if (!eth_if || n < 14 || n > 1514) return;
    uint8_t *c = malloc(n);
    if (!c) { cnt.rx_drop++; return; }
    memcpy(c, p, n);
    if (!ipf_ether(&ipf, c, n, (uint32_t)(esp_timer_get_time() / 1000))) { free(c); return; }
    last_dc_us = esp_timer_get_time();
    cnt.rx++;
    esp_netif_receive(eth_if, c, n, c);       /* lwIP frees it through free_cb */
}

static void on_lease(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_ap_staipassigned_t *e = data;
    if (e->esp_netif != eth_if) return;
    cnt.leases++;
    snprintf(cnt.last_ip, sizeof cnt.last_ip, IPSTR, IP2STR(&e->ip));
    char t[96];
    int k = snprintf(t, sizeof t, "Ethernet: the Dreamcast got " IPSTR " by DHCP", IP2STR(&e->ip));
    log_add("esp32", t, (size_t)k);
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
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 99, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 99, 1) },
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
    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 99, 1) } };
    esp_netif_set_dns_info(eth_if, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer = 1;                                    /* OFFER_DNS */
    esp_netif_dhcps_option(eth_if, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(eth_if);

    esp_netif_napt_enable(eth_if);                        /* NAT out through the Wi-Fi */
    dnsd_set_listen(1, true, ETH_IP);
    xTaskCreate(watch_task, "dc_eth", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Ethernet side up: 192.168.99.1/24, DHCP, NAT");
}
