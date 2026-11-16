/*
 * A real W5500 module on the ESP32's own SPI pins: the wired Ethernet port.
 *
 * The port finds its role by itself when a cable is plugged in:
 *   uplink  somebody on the wire answers DHCP (a router): the wired network becomes the way
 *           out, ahead of Wi-Fi.
 *   BBA     nobody answers within 10 s (a Dreamcast broadband adapter never does): this box
 *           is 192.168.98.1, hands out addresses and DNS, and routes the Dreamcast out through
 *           Wi-Fi with NAT, the same way as the cable's own network (dc_eth.c).
 * Safety: a DHCP server on somebody's home network is harmful, so in the BBA role every frame
 * from another DHCP server sends the port straight back to the uplink role for a minute.
 *
 * Without a module on the pins the driver's chip check fails at start and the port stays off.
 */
#include "sdkconfig.h"
#include "app.h"
#if CONFIG_DCL_W5500
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "ipfilter_model.h"

#define BBA_IP        0xC0A86201u                 /* 192.168.98.1 */
#define DETECT_US     (10 * 1000000LL)            /* no DHCP answer for this long: BBA role */
#define HOLD_US       (60 * 1000000LL)            /* after seeing a foreign DHCP server */
#define QUIET_US      (30 * 1000000LL)

enum { R_OFF, R_DETECT, R_UPLINK, R_BBA };
static const char *const role_name[] = { "none", "detect", "uplink", "bba" };

static esp_eth_handle_t eth;
static esp_netif_t *up_if, *bba_if;
static esp_netif_t *volatile cur;                 /* where received frames go */
static volatile bool present, link_on, foreign_dhcp;
static volatile int role = R_OFF;
static volatile int64_t last_dc_us = -QUIET_US * 2;
static ipf_t ipf, ipf_in;
static struct { uint32_t rx, tx, rx_drop, tx_drop, leases, to_bba, foreign; char ip[16]; } cnt;

static void say(const char *m) { log_add("esp32", m, strlen(m)); }

/* a DHCP server's packet (UDP from port 67) that isn't ours */
static bool is_dhcp_server_frame(const uint8_t *f, uint32_t n) {
    if (n < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[23] != 17) return false;
    uint32_t ihl = (uint32_t)(f[14] & 15) * 4;
    if (ihl < 20 || n < 14 + ihl + 8) return false;
    if ((f[20] & 0x1F) || f[21]) return false;                     /* not the first fragment */
    return f[14 + ihl] == 0 && f[14 + ihl + 1] == 67;
}

/* the driver's receive path (its own task); buf is malloc'ed and ours to free or pass on */
static esp_err_t eth_in(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv) {
    (void)h; (void)priv;
    esp_netif_t *to = cur;
    if (!to || len < 14 || len > 1514) { free(buf); cnt.rx_drop++; return ESP_OK; }
    if (to == bba_if) {
        if (is_dhcp_server_frame(buf, len)) { foreign_dhcp = true; cnt.foreign++; free(buf); return ESP_OK; }
        ipf.public_ip = pubip_get();
        if (!ipf_ether(&ipf, buf, len, (uint32_t)(esp_timer_get_time() / 1000))) { free(buf); return ESP_OK; }
        last_dc_us = esp_timer_get_time();
    }
    cnt.rx++;
    return esp_netif_receive(to, buf, len, buf);                   /* lwIP frees it through free_cb */
}

static esp_err_t tx_cb(void *h, void *buf, size_t len) {
    if (h == bba_if) {
        if (role != R_BBA) { cnt.tx_drop++; return ESP_OK; }
        ipf_in.public_ip = pubip_get();
        ipf_ether_to_dc(&ipf_in, buf, len);
    }
    if (esp_eth_transmit(eth, buf, (uint32_t)len) != ESP_OK) { cnt.tx_drop++; return ESP_OK; }
    cnt.tx++;
    return ESP_OK;
}

static void free_cb(void *h, void *buf) { (void)h; free(buf); }

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (*(esp_eth_handle_t *)data != eth) return;
    if (id == ETHERNET_EVENT_CONNECTED) link_on = true;
    else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) link_on = false;
}

static void on_lease(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_ap_staipassigned_t *e = data;
    if (e->esp_netif != bba_if) return;
    cnt.leases++;
    char t[96];
    int k = snprintf(t, sizeof t, "wired port: the Dreamcast's broadband adapter got " IPSTR " by DHCP", IP2STR(&e->ip));
    log_add("esp32", t, (size_t)k);
}

static void up_on(void) {
    esp_netif_action_start(up_if, NULL, 0, NULL);
    esp_netif_action_connected(up_if, NULL, 0, NULL);              /* starts the DHCP client */
    cur = up_if;
}

static void up_off(void) {
    cur = NULL;
    esp_netif_action_disconnected(up_if, NULL, 0, NULL);
    esp_netif_action_stop(up_if, NULL, 0, NULL);
}

static void bba_on(void) {
    esp_netif_action_start(bba_if, NULL, 0, NULL);                 /* AUTOUP: also starts the DHCP server */
    esp_netif_dhcps_stop(bba_if);
    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 98, 1) } };
    esp_netif_set_dns_info(bba_if, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer = 1;                                             /* OFFER_DNS */
    esp_netif_dhcps_option(bba_if, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(bba_if);
    esp_netif_napt_enable(bba_if);
    dnsd_set_listen(2, true, BBA_IP);
    foreign_dhcp = false;
    cur = bba_if;
}

static void bba_off(void) {
    cur = NULL;
    dnsd_set_listen(2, false, BBA_IP);
    dcnow_set_online(2, false);
    esp_netif_action_stop(bba_if, NULL, 0, NULL);
}

static void all_off(void) {
    if (role == R_BBA) bba_off();
    else if (role == R_DETECT || role == R_UPLINK) up_off();
    role = R_OFF;
}

static void manager(void *arg) {
    (void)arg;
    int64_t t_role = 0, hold_until = 0;
    bool dc_active = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t t = esp_timer_get_time();
        if (!link_on) {
            if (role != R_OFF) { all_off(); dc_active = false; cnt.ip[0] = 0; say("wired port: cable unplugged"); }
            continue;
        }
        esp_netif_ip_info_t ip = { 0 };
        switch (role) {
        case R_OFF:
            up_on(); role = R_DETECT; t_role = t;
            say("wired port: cable plugged in, asking for an address");
            break;
        case R_DETECT:
            esp_netif_get_ip_info(up_if, &ip);
            if (ip.ip.addr) {
                role = R_UPLINK;
                snprintf(cnt.ip, sizeof cnt.ip, IPSTR, IP2STR(&ip.ip));
                char m[120];
                int k = snprintf(m, sizeof m, "wired port: uplink, address %s; the wired network is now the way out", cnt.ip);
                log_add("esp32", m, (size_t)k);
            } else if (t - t_role > DETECT_US && t > hold_until) {
                up_off(); bba_on(); role = R_BBA; cnt.to_bba++;
                strlcpy(cnt.ip, "192.168.98.1", sizeof cnt.ip);
                say("wired port: nobody answered DHCP, so this is the Dreamcast's broadband adapter: handing out 192.168.98.x");
            }
            break;
        case R_UPLINK:
            esp_netif_get_ip_info(up_if, &ip);
            if (!ip.ip.addr) { role = R_DETECT; t_role = t; cnt.ip[0] = 0; say("wired port: the address is gone, asking again"); }
            break;
        case R_BBA:
            if (foreign_dhcp) {
                bba_off(); dc_active = false;
                up_on(); role = R_DETECT; t_role = t; hold_until = t + HOLD_US; cnt.ip[0] = 0;
                say("wired port: another DHCP server is on this cable, so it is a network, not a Dreamcast: back to uplink");
                break;
            }
            {
                bool now = t - last_dc_us < QUIET_US;
                if (now != dc_active) {
                    dc_active = now;
                    say(now ? "wired port: the Dreamcast's broadband adapter is sending"
                            : "wired port: the Dreamcast's broadband adapter has been quiet for 30 s");
                    dcnow_set_online(2, now);
                }
            }
            break;
        }
    }
}

bool wired_uplink_up(void) { return role == R_UPLINK; }

size_t wired_info_json(char *out, size_t cap) {
    return (size_t)snprintf(out, cap,
        "{\"present\":%s,\"link\":%s,\"role\":\"%s\",\"ip\":\"%s\",\"frames_in\":%lu,\"frames_out\":%lu,"
        "\"in_dropped\":%lu,\"out_dropped\":%lu,\"leases\":%lu,\"other_dhcp_seen\":%lu}",
        present ? "true" : "false", link_on ? "true" : "false", role_name[role], cnt.ip,
        (unsigned long)cnt.rx, (unsigned long)cnt.tx, (unsigned long)cnt.rx_drop, (unsigned long)cnt.tx_drop,
        (unsigned long)cnt.leases, (unsigned long)cnt.foreign);
}

static esp_netif_t *make_netif(bool server, const uint8_t mac[6]) {
    static const esp_netif_ip_info_t ipi = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 98, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 98, 1) },
    };
    static esp_netif_driver_ifconfig_t drv[2];
    esp_netif_driver_ifconfig_t *d = &drv[server];
    d->transmit = tx_cb;
    d->driver_free_rx_buffer = free_cb;
    esp_netif_inherent_config_t base = { 0 };
    if (server) {
        base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
        base.ip_info = &ipi;
        base.if_key = "WIRED_BBA"; base.if_desc = "dcl_bba";
        base.route_prio = 4;                        /* never the way out */
    } else {
        base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
        base.get_ip_event = IP_EVENT_ETH_GOT_IP;
        base.lost_ip_event = IP_EVENT_ETH_LOST_IP;
        base.if_key = "WIRED_UP"; base.if_desc = "dcl_wired";
        base.route_prio = 150;                      /* ahead of Wi-Fi (100) */
    }
    memcpy(base.mac, mac, 6);
    esp_netif_config_t cfg = { .base = &base, .driver = d, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    esp_netif_t *n = esp_netif_new(&cfg);
    d->handle = n;                                  /* tx_cb tells the two apart by this */
    if (n) esp_netif_set_driver_config(n, d);
    return n;
}

void wired_start(void) {
    char m[160];
    spi_bus_config_t bus = {
        .miso_io_num = CONFIG_DCL_W5500_MISO, .mosi_io_num = CONFIG_DCL_W5500_MOSI,
        .sclk_io_num = CONFIG_DCL_W5500_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { say("wired port: the SPI bus could not be set up"); return; }
    gpio_set_pull_mode(CONFIG_DCL_W5500_MISO, GPIO_PULLUP_ONLY);   /* no module: reads as ff, the chip check fails cleanly */
    gpio_install_isr_service(0);                                    /* fine if it is already there */

    spi_device_interface_config_t dev = {
        .mode = 0, .clock_speed_hz = CONFIG_DCL_W5500_MHZ * 1000000, .queue_size = 20,
        .spics_io_num = CONFIG_DCL_W5500_CS,
    };
    eth_w5500_config_t wc = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
    wc.int_gpio_num = CONFIG_DCL_W5500_INT;
    if (CONFIG_DCL_W5500_INT < 0) wc.poll_period_ms = 5;
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.reset_gpio_num = -1;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&wc, &mc);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&pc);
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(mac, phy);
    if (!mac || !phy || esp_eth_driver_install(&ec, &eth) != ESP_OK) {
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        spi_bus_free(SPI2_HOST);
        eth = NULL;
        int k = snprintf(m, sizeof m, "wired port: no W5500 module found (clock GPIO%d, MOSI %d, MISO %d, CS %d): off",
                         CONFIG_DCL_W5500_SCLK, CONFIG_DCL_W5500_MOSI, CONFIG_DCL_W5500_MISO, CONFIG_DCL_W5500_CS);
        log_add("esp32", m, (size_t)k);
        return;
    }
    uint8_t addr[6];
    esp_read_mac(addr, ESP_MAC_ETH);
    addr[0] |= 0x02;                                /* locally administered: not the cable network's address */
    addr[5] ^= 0x55;
    esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, addr);
    up_if = make_netif(false, addr);
    bba_if = make_netif(true, addr);
    if (!up_if || !bba_if) { say("wired port: no memory for the interfaces"); return; }
    esp_eth_update_input_path(eth, eth_in, NULL);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_lease, NULL);
    present = true;
    xTaskCreate(manager, "wired", 4096, NULL, 5, NULL);
    if (esp_eth_start(eth) != ESP_OK) { say("wired port: the W5500 did not start"); return; }
    int k = snprintf(m, sizeof m, "wired port: W5500 module found (%02x:%02x:%02x:%02x:%02x:%02x), waiting for a cable",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    log_add("esp32", m, (size_t)k);
}
#else
bool wired_uplink_up(void) { return false; }
size_t wired_info_json(char *out, size_t cap) { return (size_t)snprintf(out, cap, "{\"present\":false}"); }
void wired_start(void) { }
#endif
