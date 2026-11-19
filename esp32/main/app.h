/* DC-PicoLink ESP32 side: shared declarations */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- link to the cable (link.c) ---- */
void link_start(void);
bool link_send(uint8_t ch, const void *p, size_t n);   /* false if the queue is full */
bool link_is_up(void);

typedef struct {
    uint32_t good, crc_bad, broken, lost;     /* from the cable */
    uint32_t sent, send_full;                 /* to the cable */
    uint32_t ser_in, ser_out, eth_in;         /* bytes / frames */
    uint32_t test_echo;                       /* self-test packets sent back to the cable */
} link_stats_t;
void link_get_stats(link_stats_t *s);

void link_note_ser_out(size_t n);                     /* bytes sent to the DC (counter) */

/* What the cable said in its last answer to "get hello" / "get link.*" (text, one key per line) */
bool link_eth_is_computer(void);                       /* cable says: network frames are from a computer on its USB */
size_t link_cable_info(char *out, size_t cap);
/* Send a dc1 message to the cable; the answer appears in link_last_answer() */
bool link_dc1(const char *msg, size_t n);
size_t link_last_answer(char *out, size_t cap, uint32_t *seq);

/* ---- logs kept for the web page (main.c) ---- */
/* status lines from the cable and the ESP32's own notes; each has a running number */
void log_add(const char *src, const char *text, size_t n);
size_t log_since(uint32_t since, char *json_out, size_t cap, uint32_t *next);
/* serial bytes seen in both directions, for the monitor */
void ser_mon_add(bool from_dc, const uint8_t *p, size_t n);
size_t ser_mon_since(uint32_t since, char *json_out, size_t cap, uint32_t *next);

/* ---- Internet for the Dreamcast over the serial cable (E-3) ---- */
/* ppp_srv.c: the Hayes modem stub and the PPP server on the serial channel */
void ppp_srv_start(void);
void ser_from_dc(const uint8_t *p, size_t n);          /* serial bytes from the Dreamcast */
void ppp_srv_set_enabled(bool on);                      /* off: serial bytes only go to the monitor */
size_t net_info_json(char *out, size_t cap);            /* one JSON object for the web page */
/* dcnet.c: a call passed to DCNet (flyinghead's access points) instead of our PPP server */
typedef size_t (*dcnet_out_t)(const void *data, size_t len);
typedef void (*dcnet_done_t)(void);
void dcnet_start(void);
int dcnet_mode(void);                                   /* DCNET_OFF / _PREFIX (*69) / _ALWAYS */
void dcnet_set_mode(int m);
bool dcnet_call(const char *number, dcnet_out_t out, dcnet_done_t done);
void dcnet_from_dc(const uint8_t *p, size_t n);
void dcnet_hangup(void);
bool dcnet_active(void);
size_t dcnet_info_json(char *out, size_t cap);
bool ppp_srv_up(void);                                  /* a PPP call is up */
/* a call through the USB modem uses the same PPP server */
bool ppp_modem_call_start(bool capcom);
void ppp_modem_bytes(const uint8_t *p, size_t n);
void ppp_modem_call_end(void);
/* usbmodem.c: a USB voice modem on this board's USB socket (the Dreamcast's own modem calls it) */
void usbmodem_start(void);
size_t usbmodem_write(const void *data, size_t len);
void usbmodem_call_over(void);                          /* PPP ended: hang up */
size_t usbmodem_info_json(char *out, size_t cap);
/* the modem, for netlink.c (from the modem task only) */
bool um_cmd(const char *c, int ms);
bool um_connect(bool originate, int ms);                /* ATA / ATX1D, then the data goes to um_read */
int  um_read(uint8_t *b, size_t cap, int ms);           /* 0 = nothing yet, -1 = the call is over */
bool um_carrier(void);
bool um_gone(void);
void um_hangup(void);
void um_stop_tone(void);
/* netlink.c: Netlink tunnel (direct dial, game servers, XBAND), as DreamPi's netlink.py */
struct nl_servers_s;
void netlink_start(void);
const void *netlink_servers_raw(void);
#define netlink_servers() ((const nl_servers_t *)netlink_servers_raw())
const char *netlink_mode_name(int mode);
void netlink_run(const void *dial);                     /* a nl_dial_t from nl_classify */
bool netlink_idle_poll(void);                           /* during the dial tone: true if it handled a call */
size_t netlink_info_json(char *out, size_t cap);
/* dc_eth.c: the cable's W5500 network as a second interface (E-4) */
void dc_eth_start(void);
void eth_from_dc(const uint8_t *p, size_t n);          /* one Ethernet frame from the Dreamcast */
size_t eth_info_json(char *out, size_t cap);
/* dnsd.c: the small DNS server for the Dreamcast */
/* one secure (HTTPS) connection at a time: each needs about 30 KB while it runs */
void tls_lock(void);
void tls_unlock(void);
void dnsd_start(void);
int dns_tcp_ask(uint32_t server_host_order, const uint8_t *q, size_t qn, uint8_t *a, size_t cap, int ms);
void dnsd_use_tcp(bool on);                             /* the router fakes UDP DNS answers: forward over TCP */
bool dnsd_tcp(void);
void dnsd_set_listen(int which, bool on, uint32_t ip_host_order);   /* 0 = PPP call, 1 = Ethernet side, 2 = wired port (BBA) */
void dnsd_info(uint32_t *q, uint32_t *fwd, uint32_t *ovr, uint32_t *nx, int *n_ovr, int *n_up, char *last, size_t cap);
/* dcnow.c: DCNow! reports */
void dcnow_start(void);
void dcnow_note_domain(const char *domain);
void dcnow_set_online(int source, bool on);                /* 0 = PPP call, 1 = Ethernet side, 2 = wired port (BBA), 3 = Netlink game server; reports while any is on */
void dcnow_set_enabled(bool on);
bool dcnow_enabled(void);
void dcnow_info(uint32_t *ok, uint32_t *fail);
bool dcnow_set_id(const char *mac_or_id);              /* typed: "" = back to this box's own; a DreamPi's MAC or ID. Refused once the box has joined a Wi-Fi network (wifi_joined_once) */
const char *dcnow_shared_id(void);                         /* the other device's ID (typed or cloned), "" if none */
void dcnow_use_other(bool on);                             /* report as that device (true) or with this box's own ID (false) */
void dcnow_clone(const char *ip);                          /* copy the ID of a DreamPi on this network (ip "" = find dreampi.local); runs in its own task */
size_t dcnow_id_json(char *o, size_t cap);                 /* {"own","other","use","locked","busy","msg"} for the status page */

/* ---- VPN to Dreamcast Live (vpn.c, E-7) ---- */
void vpn_start(void);
void vpn_set_enabled(bool on);
void vpn_auto_on(const char *why);
int vpn_profiles_n(void);
uint32_t vpn_dc_addr(void);                             /* the Dreamcast's address in the VPN (host order), 0 = no tunnel */
bool vpn_profiles_write(const void *d, size_t n, bool first, bool last);
uint32_t ppp_dc_ip(void);                               /* network order, 0 = no PPP call */
uint32_t dc_eth_dc_ip(void);
uint32_t wired_dc_ip(void);
struct netif;
struct netif *ppp_dc_netif(void);                        /* the PPP call's interface */
bool ppp_dc_lanmode(void);                               /* this call gives the Dreamcast its home address (dclan.c) */
struct netif *dc_eth_netif(void);
struct netif *wired_dc_netif(void);
struct netif *vpn_netif(void);                           /* the tunnel, NULL when down */
/* dclan.c: the Dreamcast's own address on the home network, as on a DreamPi */
void dclan_start(void);
bool dclan_addrs(uint32_t *this_ip, uint32_t *dc_ip);    /* host order; false = none (NAT as before) */
uint32_t dclan_dc_ip(void);
bool dclan_decided(void);                               /* the home-address pick is done (either way) */                              /* host order, 0 = none */
size_t dclan_info_json(char *o, size_t cap);
void dclan_as_home(uint16_t port, uint32_t peer_n);      /* next TCP connection from local port `port` to peer goes out as the home address */
void dclan_as_home_end(void);
const void *dnat_rules(void);                         /* dnat.c: the address swaps now in force (an ipf_dnat_t *), or NULL */
void dnat_fetch_poll(void);                             /* dnat.c: DreamPi's dnat.txt address swaps, run from dcnow.c's task */
void dnsd_fetch_poll(void);                              /* dnsd.c: the DNS list download, run from dcnow.c's task */
/* portfwd.c: game ports to the Dreamcast (NAT port maps) and from the router (UPnP), as DreamPi */
void portfwd_start(void);
void portfwd_set_enabled(bool on);
size_t portfwd_info_json(char *out, size_t cap);
bool vpn_enabled(void);
void vpn_set_pick(const char *host);                    /* "auto" or a server from the profiles */
size_t vpn_info_json(char *out, size_t cap);

/* ---- Wi-Fi (wifi.c) ---- */
bool wifi_sta_up(void);

/* pubip.c: our public Internet address (0 = not known yet) */
uint32_t pubip_get(void);
void pubip_hint(uint32_t host_order);                  /* the router's public outside address (UPnP), a fallback */
void pubip_start(void);
void wifi_start(void);
bool wifi_joined_once(void);                               /* this box has been on a home network (kept in flash) */
#define DCL_AP_IP "192.168.98.1"                           /* the setup network's address, and the page on it */
void wifi_set_sta(const char *ssid, const char *pass);
size_t wifi_info_json(char *out, size_t cap);

/* ---- the wired Ethernet port: a real W5500 module on the ESP32's SPI pins (wired.c) ---- */
void wired_start(void);
bool wired_uplink_up(void);                             /* a router answered: the wired network is the way out */
size_t wired_info_json(char *out, size_t cap);
/* is there a way to the Internet (Wi-Fi joined, or the wired uplink) */
static inline bool inet_up(void) { return wifi_sta_up() || wired_uplink_up(); }

/* ---- web page (web.c) ---- */
void web_start(void);

/* JSON string escaping into out; returns bytes written (no terminator counted) */
size_t json_escape(char *out, size_t cap, const char *s, size_t n);
