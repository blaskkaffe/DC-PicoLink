#!/bin/bash
# Host test of the PPP server: ESP-IDF's own lwIP (PPP server, no OS) behind firmware/hdlc_model.h,
# talked to by a small Python client the way a Dreamcast would (LCP, IPCP with address and DNS, ping,
# hang up, "AT"). Needs ESP-IDF (IDF_PATH), gcc and python3. Nothing is flashed.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
: "${IDF_PATH:?set IDF_PATH to an ESP-IDF checkout (5.4)}"
L="$IDF_PATH/components/lwip/lwip/src"
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
srcs="$L/core/init.c $L/core/def.c $L/core/dns.c $L/core/inet_chksum.c $L/core/ip.c $L/core/mem.c $L/core/memp.c
 $L/core/netif.c $L/core/pbuf.c $L/core/raw.c $L/core/stats.c $L/core/sys.c $L/core/timeouts.c $L/core/udp.c
 $L/core/ipv4/icmp.c $L/core/ipv4/ip4.c $L/core/ipv4/ip4_addr.c $L/core/ipv4/ip4_frag.c
 $(ls "$L"/netif/ppp/*.c | grep -v -E 'pppapi|pppoe|pppol2tp|chap|eap|mppe|ecp|ccp|multilink|polarssl|pppcrypt')"
# shellcheck disable=SC2086
gcc -O1 -w -DESP_LWIP=0 -I"$here/ppp_negotiation/port" -I"$L/include" -I"$here/../../firmware" \
    $srcs "$here/ppp_negotiation/harness.c" -o "$out/ppp_harness"
python3 -I "$here/ppp_negotiation/test_ppp.py" "$out/ppp_harness"
