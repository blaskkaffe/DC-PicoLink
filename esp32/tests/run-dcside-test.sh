#!/bin/bash
# Host test of the ESP32's Dreamcast network (dc_eth.c): ESP-IDF's own lwIP with Ethernet/ARP, NAPT and
# its DHCP server, set up as dc_eth_start() does, between a KOS-like Dreamcast client and a home router.
# Needs ESP-IDF (IDF_PATH) and gcc. Nothing is flashed. "-v" prints every frame.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
: "${IDF_PATH:?set IDF_PATH to an ESP-IDF checkout (5.4)}"
L="$IDF_PATH/components/lwip/lwip/src"; A="$IDF_PATH/components/lwip"
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
srcs="$L/core/init.c $L/core/def.c $L/core/inet_chksum.c $L/core/ip.c $L/core/mem.c $L/core/memp.c
 $L/core/netif.c $L/core/pbuf.c $L/core/raw.c $L/core/stats.c $L/core/sys.c $L/core/timeouts.c $L/core/udp.c
 $L/core/tcp.c $L/core/tcp_in.c $L/core/tcp_out.c
 $L/core/ipv4/icmp.c $L/core/ipv4/ip4.c $L/core/ipv4/ip4_addr.c $L/core/ipv4/ip4_frag.c $L/core/ipv4/etharp.c
 $L/core/ipv4/ip4_napt.c $L/netif/ethernet.c $A/apps/dhcpserver/dhcpserver.c"
# shellcheck disable=SC2086
gcc -O1 -w -I"$here/dcside/port" -I"$L/include" -I"$A/include/apps" -I"$here/../../firmware" \
    $srcs "$here/dcside/harness.c" -o "$out/dcside"
"$out/dcside" "$@" 2>&1
