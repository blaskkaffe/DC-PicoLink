#!/bin/bash
# Test of host/dreampi/dcusb0-masterfix: a fake master that, like the real one, only
# answers a Quake 3 query without "dreamcast"; the relay (no iptables) must fix it.
set -eu
cd "$(dirname "$0")/.."
python3 -I - <<'PY'
import importlib.machinery, importlib.util, os, socket, subprocess, sys, time
l = importlib.machinery.SourceFileLoader('mf', 'host/dreampi/dcusb0-masterfix')
m = importlib.util.module_from_spec(importlib.util.spec_from_loader('mf', l)); l.exec_module(m)
H = b'\xff\xff\xff\xff'
assert m.fix_query(H + b'getservers 43 dreamcast empty full') == H + b'getservers 43 empty full'
assert m.fix_query(H + b'getservers 43 empty full') == H + b'getservers 43 empty full'
assert m.fix_query(b'x dreamcast y') == b'x dreamcast y'
master = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); master.bind(('127.0.0.1', 0)); master.settimeout(10)
mport = master.getsockname()[1]
env = dict(os.environ, NO_IPTABLES='1', MASTER_ADDR='127.0.0.1', MASTER_PORT=str(mport),
           LISTEN='127.0.0.1', RELAY_PORT='27951', MASTER_FIX='yes')
p = subprocess.Popen([sys.executable, '-I', 'host/dreampi/dcusb0-masterfix'], env=env, stdout=subprocess.DEVNULL)
time.sleep(1)
c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); c.settimeout(5)
c.sendto(H + b'getservers 43 dreamcast empty full', ('127.0.0.1', 27951))
d, a = master.recvfrom(4096)
assert d == H + b'getservers 43 empty full', d
master.sendto(H + b'getserversResponse part1', a); master.sendto(H + b'getserversResponse part2', a)
assert c.recvfrom(4096)[0].endswith(b'part1') and c.recvfrom(4096)[0].endswith(b'part2')
p.terminate(); p.wait()
print('dcusb0-masterfix: ok')
PY
