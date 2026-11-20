/*
 * The box's IP plan (ESP32), in one place. Host byte order.
 *
 *   192.168.220.0/24  setup Wi-Fi; the box (and the web page) is .1
 *   192.168.98.0/24   the Dreamcast network (98: the Dreamcast's launch year in Japan; easy to
 *                     remember next to its .98 on the home network): however it connects (the cable's W5500,
 *                     the BBA socket on the W5500 module, a PPP call without its home address),
 *                     the box is .1 (gateway and DNS); DHCP hands out .2-.99, a fixed address
 *                     goes at .100 or above; a PPP call gets .2
 *   home network      a PPP call with its home address, picked as DreamPi's autoconfigure_ppp does
 *                     (dclan.c): counting down from .99, the first free address is this box's end
 *                     of the link (usually .99), the next the Dreamcast's (usually .98); no NAT
 * One Dreamcast uses one way at a time, so the ways share the network: the box sends to the
 * interface the Dreamcast was last heard on (ipp_route / dcside_route in dc_eth.c).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define IPP_SETUP        0xC0A8DC01u    /* 192.168.220.1 */
#define IPP_SETUP_TEXT   "192.168.220.1"
#define IPP_DCNET        0xC0A86200u    /* 192.168.98.0 */
#define IPP_MASK         0xFFFFFF00u
#define IPP_BOX          0xC0A86201u    /* 192.168.98.1: gateway and DNS for the Dreamcast */
#define IPP_PPP_DC       0xC0A86202u    /* 192.168.98.2: the Dreamcast in a PPP call without its home address */
#define IPP_DHCP_FIRST   0xC0A86202u    /* .2 */
#define IPP_DHCP_LAST    0xC0A86263u    /* .99 */
#define IPP_ESP(a)       ((((a) >> 24) & 0xFFu) | (((a) >> 8) & 0xFF00u) | (((a) << 8) & 0xFF0000u) | ((a) << 24))   /* to esp_ip4_addr_t / lwIP order (little-endian CPU) */

static inline bool ipp_in_dcnet(uint32_t h) { return (h & IPP_MASK) == IPP_DCNET; }
/* an address the Dreamcast can have there: not the box, not the network or broadcast address */
static inline bool ipp_dc_host(uint32_t h) { return ipp_in_dcnet(h) && h != IPP_BOX && (h & 0xFFu) != 0 && (h & 0xFFu) != 255; }
