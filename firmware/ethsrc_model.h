/*
 * The Dreamcast's address on the cable network, from its own frames: a Dreamcast set to a fixed
 * address never asks DHCP, so the box would not know it (port forwarding and "the Dreamcast is
 * online" use it). Returns the IPv4 source address (network byte order, as lwIP keeps it) of an
 * Ethernet frame when it is a host address in the network gw/mask other than gw; else 0.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline uint32_t ethsrc_dc_ip(const uint8_t *f, size_t n, uint32_t gw_be, uint32_t mask_be) {
    if (n < 34 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4) return 0;
    uint32_t a;
    memcpy(&a, f + 26, 4);
    if (a == gw_be || (a & mask_be) != (gw_be & mask_be)) return 0;
    if ((a & ~mask_be) == 0 || (a | mask_be) == 0xFFFFFFFFu) return 0;   /* network / broadcast address */
    return a;
}
