#!/bin/bash
# DC-PicoLink on any Linux computer: the USB modem server (dc-modemd) and, unless --no-cable,
# the RP2040 cable's side too (network, DHCP/DNS/NAT, SD image: host/dreampi/install.sh, which
# does not need a DreamPi). Run: sudo ./install.sh [--no-cable] [--no-modem] [--cable-ppp]
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
cd "$(dirname "$0")"
cable=1; modem=1; ppp=0
for a in "$@"; do case "$a" in
    --no-cable) cable=0 ;; --no-modem) modem=0 ;; --cable-ppp) ppp=1 ;;
    *) echo "usage: sudo ./install.sh [--no-cable] [--no-modem] [--cable-ppp]" >&2; exit 1 ;;
esac; done
export PATH="$PATH:/usr/sbin:/sbin"
command -v systemctl >/dev/null 2>&1 || { echo "this needs systemd (systemctl not found)" >&2; exit 1; }

if [ "$modem" = 1 ]; then
    echo "== packages for the modem"
    if ! command -v pppd >/dev/null 2>&1; then
        if command -v apt-get >/dev/null 2>&1; then apt-get update || true; apt-get install -y ppp
        elif command -v dnf >/dev/null 2>&1; then dnf install -y ppp
        elif command -v pacman >/dev/null 2>&1; then pacman -S --noconfirm ppp
        else echo "install pppd (the 'ppp' package) by hand, then run this again" >&2; exit 1; fi
    fi
    command -v iptables >/dev/null 2>&1 || echo "note: no iptables, so the Dreamcast gets no NAT from dc-modemd (run it with --no-nat and route yourself)"
    command -v python3 >/dev/null 2>&1 || { echo "python3 is needed" >&2; exit 1; }
    echo "== modem files"
    install -m 755 dc-modemd.py /usr/local/sbin/dc-modemd
    [ -f /etc/dc-modemd.conf ] || install -m 644 dc-modemd.conf /etc/dc-modemd.conf
    install -m 644 dc-modemd.service /etc/systemd/system/dc-modemd.service
    # the modem is a plain serial port: allow dialout users and keep ModemManager's hands off it
    if [ -d /etc/udev/rules.d ]; then
        printf '# DC-PicoLink: ModemManager must not probe the voice modem\nSUBSYSTEM=="tty", ENV{ID_BUS}=="usb", ENV{ID_MM_DEVICE_IGNORE}="1"\n' > /etc/udev/rules.d/71-dc-modem.rules
        udevadm control --reload 2>/dev/null || true
    fi
    install -m 644 dc-cabled.service /etc/systemd/system/dc-cabled.service
    systemctl daemon-reload
    systemctl enable dc-modemd.service
    systemctl restart dc-modemd.service
    if [ "$ppp" = 1 ]; then     # serial PPP on the cable: takes /dev/dccable, so not on by default
        systemctl disable --now dc-modemd.service
        systemctl enable --now dc-cabled.service
    fi
fi

if [ "$cable" = 1 ]; then
    echo "== the cable (RP2040): network, DHCP/DNS/NAT, SD image"
    ../dreampi/install.sh
fi
echo
echo "Done. Follow the modem with:  journalctl -fu dc-modemd"
