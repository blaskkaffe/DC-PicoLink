#!/bin/bash
# OpenVPN client test on the PC: builds ovpn.c against ESP-IDF's mbedTLS, starts a real
# OpenVPN server with the DreamPi 2.1 settings (UDP, AES-128-GCM, SHA256, tls-auth, client
# certificate, no compression) and pings through the tunnel.
# Needs: openvpn, openssl, a C compiler, ESP-IDF (IDF_PATH) for the mbedTLS sources, root + /dev/net/tun.
# usage: run-ovpn-test.sh [mode]      mode: full (default, ~20 s) | up | ...
set -e
here=$(cd "$(dirname "$0")" && pwd)
idf=${IDF_PATH:-/home/claude/esp-idf}
work=${OVPN_TEST_DIR:-/tmp/ovpn-test}
mode=${1:-full}
mkdir -p "$work"
mb=$idf/components/mbedtls/mbedtls

# --- mbedTLS for the PC (cached)
if [ ! -f "$work/libmbedtls-pc.a" ]; then
    mkdir -p "$work/mb"
    for f in "$mb"/library/*.c; do
        gcc -O1 -c -I"$mb/include" -I"$mb/library" "$f" -o "$work/mb/$(basename "$f" .c).o" &
        while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.1; done
    done
    wait
    ar rcs "$work/libmbedtls-pc.a" "$work"/mb/*.o
fi
gcc -O1 -Wall -Wextra -Werror -I"$mb/include" "$here/harness.c" "$here/../../main/ovpn.c" "$work/libmbedtls-pc.a" -o "$work/ovpn-harness"

# --- keys and certificates (fresh each run)
pki=$work/pki; rm -rf "$pki"; mkdir -p "$pki"; cd "$pki"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -subj "/CN=Test CA" -days 3 2>/dev/null
mk() {  # name, eku
    openssl req -newkey rsa:2048 -nodes -keyout $1.key -out $1.csr -subj "/CN=$1" 2>/dev/null
    printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=$2\n" > $1.ext
    openssl x509 -req -in $1.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out $1.crt -days 3 -extfile $1.ext 2>/dev/null
}
mk server serverAuth
mk client clientAuth
openvpn --genkey tls-auth ta.key 2>/dev/null || openvpn --genkey --secret ta.key
port=${OVPN_TEST_PORT:-19091}

cat > server.conf <<EOS
port $port
proto udp
dev tun
ca ca.crt
cert server.crt
key server.key
dh none
tls-auth ta.key 0
cipher AES-128-GCM
data-ciphers AES-128-GCM
auth SHA256
server 10.88.0.0 255.255.255.0
topology subnet
keepalive 5 30
persist-key
verb 3
EOS
cat > client.conf <<EOC
client
dev tun0
proto udp
remote 127.0.0.1 $port
nobind
remote-cert-tls server
auth SHA256
cipher AES-128-GCM
key-direction 1
verb 3
ping 5
ping-restart 30
explicit-exit-notify 2
<ca>
$(cat ca.crt)
</ca>
<cert>
$(openssl x509 -in client.crt -text)
</cert>
<key>
$(cat client.key)
</key>
<tls-auth>
$(cat ta.key)
</tls-auth>
EOC
openvpn --config server.conf > server.log 2>&1 &
spid=$!
trap 'kill $spid 2>/dev/null; wait $spid 2>/dev/null' EXIT
for i in $(seq 50); do grep -q "Initialization Sequence Completed" server.log && break; sleep 0.2; done
grep -q "Initialization Sequence Completed" server.log || { echo "server did not start"; cat server.log; exit 1; }
set +e
"$work/ovpn-harness" client.conf $port "$mode"
rc=$?
if [ $rc -ne 0 ]; then echo "--- server log"; tail -30 server.log; fi
exit $rc
