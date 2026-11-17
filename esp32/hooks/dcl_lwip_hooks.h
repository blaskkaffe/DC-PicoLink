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
static inline struct netif *dcl_route_src(const ip4_addr_t *src, const ip4_addr_t *dest) {
    struct netif *n = ip4_route_src_hook(src, dest);
    return n ? n : vpn_route(dest);
}
#undef LWIP_HOOK_IP4_ROUTE_SRC
#define LWIP_HOOK_IP4_ROUTE_SRC dcl_route_src
