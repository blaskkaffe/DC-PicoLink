#!/bin/bash
# Test of host/dreampi/dcusb0-vpn's routing in network namespaces (needs root, ip, iptables, python3).
#   pi  = the DreamPi: dcusb0 (192.168.99.1), eth0 (LAN, 192.168.1.111), tun0 (VPN, 10.200.2.212)
#   dc  = the Dreamcast, 192.168.99.9 (DHCP lease faked in /run/dcusb0.leases)
#   net = the Internet behind the LAN, vpn = the VPN server; both own 20.102.63.253 and answer
#         UDP 27950 with their own name, so the reply shows which way the query went.
# Checks: VPN_ROUTE=none -> LAN; a list -> only those addresses through tun0 (source 10.200.2.213);
# all -> everything through tun0 (ping stubbed); all with a VPN that does not forward -> LAN;
# stop -> everything removed again.
set -u
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 2; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=$HERE/host/dreampi/dcusb0-vpn
fail=0
ok()  { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; [ -z "${DEBUG:-}" ] || debug; }
debug() {
    echo "--- rules:"; ip netns exec pi ip rule; echo "--- table 217:"; ip netns exec pi ip route show table 217
    echo "--- route get:"; ip netns exec pi ip route get 20.102.63.253 from 192.168.99.9 iif dcusb0
    echo "--- nat:"; ip netns exec pi iptables -t nat -S | grep DCUSB0; echo "--- forward:"; ip netns exec pi iptables -S DCUSB0_FWD
    echo "--- counters:"; ip netns exec pi iptables -t nat -nvL DCUSB0_POST | tail -2; ip netns exec pi iptables -nvL DCUSB0_FWD | tail -2
    echo "--- tun0 / v0:"; ip netns exec pi ip -s link show tun0 | sed -n 3,6p; ip netns exec vpn ip -s link show v0 | sed -n 3,6p; ip netns exec pi ip neigh show dev tun0
    echo "--- log:"; cat "$W"/log* 2>/dev/null | tail -8
}
W=$(mktemp -d); trap 'cleanup' EXIT
cleanup() {
    [ -n "${VPN_PID:-}" ] && kill "$VPN_PID" 2>/dev/null
    for n in pi dc net vpn; do ip netns pids $n 2>/dev/null | xargs -r kill 2>/dev/null; ip netns del $n 2>/dev/null; done
    rm -rf "$W" /run/dcusb0.leases
}
for n in pi dc net vpn; do ip netns del $n 2>/dev/null; ip netns add $n; ip -n $n link set lo up; done
ip link add dcusb0 netns pi type veth peer name eth0 netns dc
ip link add eth0 netns pi type veth peer name lan netns net
ip link add tun0 netns pi type veth peer name v0 netns vpn
P="ip netns exec pi"
$P ip addr add 192.168.99.1/24 dev dcusb0; $P ip link set dcusb0 up
ip netns exec dc ip addr add 192.168.99.9/24 dev eth0; ip netns exec dc ip link set eth0 up
ip netns exec dc ip route add default via 192.168.99.1
$P ip addr add 192.168.1.111/24 dev eth0; $P ip link set eth0 up
ip netns exec net ip addr add 192.168.1.1/24 dev lan; ip netns exec net ip link set lan up
ip netns exec net ip addr add 20.102.63.253/32 dev lo
$P ip route add default via 192.168.1.1
$P ip addr add 10.200.2.212/16 dev tun0; $P ip link set tun0 up
ip netns exec vpn ip addr add 10.200.0.1/16 dev v0; ip netns exec vpn ip link set v0 up
ip netns exec vpn ip addr add 20.102.63.253/32 dev lo
TUNMAC=$($P cat /sys/class/net/tun0/address)
ip netns exec vpn ip neigh add 10.200.2.213 lladdr "$TUNMAC" dev v0 nud permanent
# Some kernels (small VM kernels without NAT) never apply the SNAT rule; the reply then comes to the
# Dreamcast's own address, so the VPN end can reach that as well. Routing is what this test checks.
ip netns exec vpn ip route add 192.168.99.0/24 dev v0
ip netns exec vpn ip neigh add 192.168.99.9 lladdr "$TUNMAC" dev v0 nud permanent
ip netns exec net ip route add 192.168.99.0/24 via 192.168.1.111      # test only: no NAT on the LAN side
$P sysctl -qw net.ipv4.ip_forward=1
for n in pi net vpn; do ip netns exec $n sysctl -qw net.ipv4.conf.all.rp_filter=1; done
echo "0 02:16:e4:11:22:33 192.168.99.9 dc 01" > /run/dcusb0.leases

responder() {   # $1 = namespace, $2 = name
    ip netns exec "$1" python3 -I -c "
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('20.102.63.253',27950))
while True:
    d,a=s.recvfrom(512); s.sendto(b'$2 '+a[0].encode(),a)" &
}
responder net LAN; responder vpn VPN
sleep 1
QP=27960
query() {       # prints what the master answered, e.g. "VPN 10.200.2.213" (a new source port each time:
                # an old flow keeps its old path and NAT, as in the real world until it times out)
    QP=$((QP + 1))
    ip netns exec dc python3 -I -c "
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2); s.bind(('0.0.0.0',$QP))
s.sendto(b'\xff\xff\xff\xffgetservers 68',('${1:-20.102.63.253}',27950))
try: print(s.recvfrom(512)[0].decode())
except Exception: print('none')"
}
mkdir -p "$W/bin"
printf '#!/bin/sh\nexit 0\n' > "$W/bin/ping"; chmod +x "$W/bin/ping"           # a VPN that forwards
printf '#!/bin/sh\nexit 1\n' > "$W/bin/ping_fail"; chmod +x "$W/bin/ping_fail"   # one that does not
run() {         # $1 = VPN_ROUTE, $2 = bin dir with the stub ping; starts the script, waits for the mapping
    [ -n "${VPN_PID:-}" ] && { kill "$VPN_PID" 2>/dev/null; wait "$VPN_PID" 2>/dev/null; $P env PATH="$W/bin:$PATH" bash "$SCRIPT" stop; }
    mkdir -p "$W/b$3"; cp "$W/bin/$2" "$W/b$3/ping"
    $P env PATH="$W/b$3:$PATH" VPN_ROUTE="$1" bash "$SCRIPT" > "$W/log$3" 2>&1 &
    VPN_PID=$!
    sleep 3
}
expect() {      # $1 = what, $2 = expected answer (prefix); "VPN" accepts the source with or without SNAT
    got=$(query "${3:-}")
    case "$got" in
    "$2 10.200.2.213") ok "$1 -> $got (SNAT applied)" ;;
    "$2 192.168.99.9") if [ "$2" = VPN ]; then ok "$1 -> $got (routing ok; this kernel did not apply SNAT)"; else ok "$1 -> $got"; fi ;;
    *) bad "$1: wanted '$2', got '$got'" ;;
    esac
}

run none ping 1;  expect "VPN_ROUTE=none: master reached over the LAN" "LAN"
run "20.102.63.253" ping 2
expect "VPN_ROUTE=list: master through tun0" "VPN"
$P ip rule | grep -q "to 20.102.63.253" && ok "rule is only for the listed address" || bad "no rule for the listed address"
run all ping 3;   expect "VPN_ROUTE=all, VPN forwards: through tun0" "VPN"
run all ping_fail 4; expect "VPN_ROUTE=all, VPN does not forward: back on the LAN" "LAN"
grep -q "does not answer through tun0" "$W/log4" && ok "the log says why" || bad "no explanation in the log"
$P ip rule | grep -q "lookup 217" && bad "rule left although the VPN does not forward" || ok "no rule left"
run all ping 5
kill "$VPN_PID"; wait "$VPN_PID" 2>/dev/null; VPN_PID=
$P env PATH="$W/b5:$PATH" bash "$SCRIPT" stop
$P ip rule | grep -q "lookup 217" && bad "stop left the rule" || ok "stop removes the rule"
$P iptables -t nat -S | grep -q DCUSB0 && bad "stop left iptables chains" || ok "stop removes the iptables chains"
[ "$fail" = 0 ] && echo "all passed" || echo "FAILED"
exit $fail
