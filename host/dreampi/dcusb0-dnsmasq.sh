#!/bin/bash
# DHCP-only dnsmasq for dcusb0 (port=0: never touches DNS port 53, so it can't
# clash with any other DNS server DreamPi may run).
set -eu
. /etc/dcusb0.conf
dns="$DNS"
if [ -z "$dns" ]; then
    dns=$(awk '$1=="nameserver" && $2 !~ /^127\./ && $2 !~ /:/ {print $2; exit}' /etc/resolv.conf || true)
    [ -n "$dns" ] || dns=1.1.1.1
fi
exec dnsmasq --keep-in-foreground --conf-file=/dev/null --port=0 \
    --interface=dcusb0 --bind-dynamic --except-interface=lo \
    --dhcp-range="$DHCP_START,$DHCP_END,12h" \
    --dhcp-option=option:router,"$PI_ADDR" \
    --dhcp-option=option:dns-server,"$dns" \
    --dhcp-leasefile=/run/dcusb0.leases --pid-file=/run/dcusb0-dnsmasq.pid \
    --log-dhcp
