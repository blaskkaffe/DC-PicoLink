#!/bin/bash
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
systemctl disable --now dcusb0.service 2>/dev/null
systemctl disable --now dcsd.service 2>/dev/null
systemctl disable --now dcusb0-vpn.service 2>/dev/null
/usr/local/sbin/dcusb0-vpn stop 2>/dev/null
/usr/local/sbin/dcusb0-up.sh stop 2>/dev/null
command -v nft >/dev/null && nft delete table ip dcusb0 2>/dev/null
rm -f /etc/udev/rules.d/70-dc-cable.rules /etc/systemd/system/dcusb0.service \
      /usr/local/sbin/dcusb0-up.sh /usr/local/sbin/dcusb0-dnsmasq.sh \
      /usr/local/sbin/dcusb0-vpn /etc/systemd/system/dcusb0-vpn.service \
      /etc/sysctl.d/90-dcusb0.conf /etc/NetworkManager/conf.d/90-dcusb0.conf \
      /etc/systemd/system/dcsd.service /usr/local/bin/dcsd.py \
      /usr/local/sbin/dcsd-mount /usr/local/sbin/dcsd-umount /usr/local/sbin/dcsd-mode /usr/local/sbin/dcsd-capture
[ -f /etc/dhcpcd.conf ] && sed -i '/Dreamcast USB network is configured by dcusb0.service/d; /^denyinterfaces dcusb0$/d' /etc/dhcpcd.conf
systemctl daemon-reload; udevadm control --reload
echo "Removed (kept /etc/dcusb0.conf, /etc/dcsd.conf and the SD image). ip_forward stays on until reboot."
