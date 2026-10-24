#!/bin/bash
# Installs the Pi/DreamPi side for the RP2040 Dreamcast cable. Run: sudo ./install.sh
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
cd "$(dirname "$0")"

echo "== packages"
export PATH="$PATH:/usr/sbin:/sbin"
APT_READY=0
apt_ready() {   # apt-get update; DreamPi is Raspbian Stretch, whose mirrors moved to legacy.raspbian.org
    [ "$APT_READY" = 1 ] && return 0
    if apt-get update; then APT_READY=1; return 0; fi
    codename=$(. /etc/os-release 2>/dev/null; echo "${VERSION_CODENAME:-}")
    if [ -n "$codename" ] && grep -qs raspbian /etc/apt/sources.list; then
        echo "apt mirror unreachable; switching to the Raspbian legacy archive for $codename"
        cp -n /etc/apt/sources.list /etc/apt/sources.list.dc-cable-backup
        sed -i "s#^deb http://[^ ]*raspbian[^ ]*/raspbian/\? #deb http://legacy.raspbian.org/raspbian/ #" /etc/apt/sources.list
        apt-get -o Acquire::Check-Valid-Until=false update || true
    fi
    APT_READY=1     # try the install anyway: a stale side repo doesn't stop it
}
need() {        # need <command> <package>
    command -v "$1" >/dev/null 2>&1 && return 0
    apt_ready
    apt-get install -y "$2" && return 0
    echo "could not install $2: install it by hand (sudo apt-get install $2), then run this again" >&2
    return 1
}
need dnsmasq dnsmasq-base
command -v iptables >/dev/null 2>&1 || command -v nft >/dev/null 2>&1 || need iptables iptables
need mkfs.fat dosfstools
need python3 python3

echo "== files"
install -m 644 70-dc-cable.rules /etc/udev/rules.d/70-dc-cable.rules
# network settings: keep the user's own, but replace an untouched old default (192.168.50.x)
if [ ! -f /etc/dcusb0.conf ] || grep -q '^PI_ADDR=192\.168\.50\.1$' /etc/dcusb0.conf; then
    install -m 644 dcusb0.conf /etc/dcusb0.conf
fi
install -m 755 dcusb0-up.sh /usr/local/sbin/dcusb0-up.sh
install -m 755 dcusb0-dnsmasq.sh /usr/local/sbin/dcusb0-dnsmasq.sh
install -m 644 dcusb0.service /etc/systemd/system/dcusb0.service
install -m 755 dcusb0-vpn /usr/local/sbin/dcusb0-vpn
install -m 644 dcusb0-vpn.service /etc/systemd/system/dcusb0-vpn.service
# options added later: append them to an existing /etc/dcusb0.conf
grep -q '^VPN_MAP=' /etc/dcusb0.conf || sed -n '/^# Dreamcast Live VPN/,$p' dcusb0.conf >> /etc/dcusb0.conf
echo 'net.ipv4.ip_forward=1' > /etc/sysctl.d/90-dcusb0.conf
# emulated SD card
[ -f /etc/dcsd.conf ] || install -m 644 dcsd.conf /etc/dcsd.conf
install -m 755 dcsd.py /usr/local/bin/dcsd.py
install -m 755 dcsd-mount /usr/local/sbin/dcsd-mount
install -m 755 dcsd-umount /usr/local/sbin/dcsd-umount
install -m 755 dcsd-mode /usr/local/sbin/dcsd-mode
install -m 755 dcsd-capture /usr/local/sbin/dcsd-capture
install -m 644 dcsd.service /etc/systemd/system/dcsd.service

echo "== keep other network managers off dcusb0"
if [ -f /etc/dhcpcd.conf ] && ! grep -q '^denyinterfaces.*dcusb0' /etc/dhcpcd.conf; then
    printf '\n# Dreamcast USB network is configured by dcusb0.service\ndenyinterfaces dcusb0\n' >> /etc/dhcpcd.conf
fi
if [ -d /etc/NetworkManager/conf.d ]; then
    printf '[keyfile]\nunmanaged-devices=interface-name:dcusb0\n' > /etc/NetworkManager/conf.d/90-dcusb0.conf
    systemctl reload NetworkManager 2>/dev/null || true
fi

echo "== activate"
systemctl daemon-reload
systemctl enable dcusb0.service
systemctl enable dcusb0-vpn.service
systemctl restart dcusb0-vpn.service
systemctl enable dcsd.service
systemctl restart dcsd.service   # creates the image on first start
udevadm control --reload
udevadm trigger --subsystem-match=net --subsystem-match=tty --action=add
sysctl -q -p /etc/sysctl.d/90-dcusb0.conf
sleep 1
if ip link show dcusb0 >/dev/null 2>&1; then
    systemctl restart dcusb0.service
    systemctl --no-pager --lines=5 status dcusb0.service || true
else
    echo "Cable not plugged in (or interface not renamed yet): unplug/replug it, then check"
    echo "  ip addr show dcusb0 ; systemctl status dcusb0"
fi
. /etc/dcusb0.conf
echo "Done. Serial port: /dev/dccable   Network: dcusb0 ($PI_ADDR)"
echo "      Dreamcast network settings: DHCP, or by hand: IP ${PI_ADDR%.*}.60  netmask 255.255.255.0"
echo "      gateway $PI_ADDR  DNS ${DNS:-$(awk '$1=="nameserver" && $2 !~ /^127\./ && $2 !~ /:/ {print $2; exit}' /etc/resolv.conf 2>/dev/null)}"
echo "      SD card image: $(. /etc/dcsd.conf; echo $DCSD_IMAGE)  (sudo dcsd-mount / sudo dcsd-umount to edit)"
