#!/bin/bash
# Bring up dcusb0 (Dreamcast W5500 via RP2040) and NAT it through the Pi's uplink.
# Run by dcusb0.service; "stop" removes the NAT rule again.
set -eu
. /etc/dcusb0.conf
IF=dcusb0

nat_rule() {   # $1 = -A / -D / -C ; fails if iptables is not installed
    command -v iptables >/dev/null 2>&1 || return 1
    iptables -t nat "$1" POSTROUTING -s "$SUBNET" ! -o "$IF" -j MASQUERADE
}

if [ "${1:-start}" = stop ]; then
    while nat_rule -C 2>/dev/null; do nat_rule -D; done
    if command -v nft >/dev/null 2>&1; then nft delete table ip dcusb0 2>/dev/null || true; fi
    exit 0
fi

ip link set "$IF" up
ip addr replace "$PI_ADDR/$PREFIX" dev "$IF"
sysctl -q -w net.ipv4.ip_forward=1
if command -v iptables >/dev/null 2>&1; then
    nat_rule -C 2>/dev/null || nat_rule -A
elif command -v nft >/dev/null 2>&1; then
    nft list table ip dcusb0 >/dev/null 2>&1 && nft delete table ip dcusb0
    nft add table ip dcusb0
    nft add chain ip dcusb0 post '{ type nat hook postrouting priority 100 ; }'
    nft add rule ip dcusb0 post ip saddr "$SUBNET" oifname != "$IF" masquerade
else
    echo "dcusb0: neither iptables nor nft found, no NAT" >&2
fi
