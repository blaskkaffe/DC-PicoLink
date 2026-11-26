#!/bin/bash
# Removes dc-modemd (the cable's side: host/dreampi/uninstall.sh). Run: sudo ./uninstall.sh
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
systemctl disable --now dc-modemd.service 2>/dev/null || true
rm -f /etc/systemd/system/dc-modemd.service /usr/local/sbin/dc-modemd /etc/udev/rules.d/71-dc-modem.rules
systemctl daemon-reload
iptables -t nat -D POSTROUTING -j DCMODEM 2>/dev/null || true
iptables -t nat -F DCMODEM 2>/dev/null && iptables -t nat -X DCMODEM 2>/dev/null || true
echo "dc-modemd removed (/etc/dc-modemd.conf kept). The cable's side: ../dreampi/uninstall.sh"
