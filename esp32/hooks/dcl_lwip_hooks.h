/*
 * lwIP hook (included into lwIP by ESP-IDF, see esp32/CMakeLists.txt): routes pushed by the
 * VPN server go into the tunnel. lwIP has no routing table of its own; this hook is asked
 * after the interfaces' own subnets, before the default route. ESP-IDF's own source-address
 * routing (ip4_route_src_hook) still comes first.
 */
#pragma once
#include "lwip/ip4_addr.h"
struct netif;
struct netif *ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest);
struct netif *vpn_route(const ip4_addr_t *dest);           /* esp32/main/vpn.c */
struct pbuf;
struct netif *dclan_route(const ip4_addr_t *dest);         /* esp32/main/dclan.c: the Dreamcast's address -> where it is */
struct netif *dcside_route(const ip4_addr_t *dest);        /* esp32/main/dc_eth.c: the Dreamcast network -> where it was last heard */
int dclan_ip4_input(struct pbuf *p, struct netif *inp);     /* esp32/main/dclan.c: DreamPi's 1:1 SNAT/DNAT */
static inline struct netif *dcl_route_src(const ip4_addr_t *src, const ip4_addr_t *dest) {
    struct netif *n = dclan_route(dest);                    /* first: its home address is inside the uplink's subnet */
    if (n) return n;
    n = dcside_route(dest);                                 /* 192.168.221.x: W5500, BBA socket and PPP share it */
    if (n) return n;
    n = ip4_route_src_hook(src, dest);
    return n ? n : vpn_route(dest);
}
#undef LWIP_HOOK_IP4_ROUTE_SRC
#define LWIP_HOOK_IP4_ROUTE_SRC dcl_route_src
#undef LWIP_HOOK_IP4_INPUT
#define LWIP_HOOK_IP4_INPUT(p, inp) dclan_ip4_input((p), (inp))
