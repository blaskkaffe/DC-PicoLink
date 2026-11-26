#!/usr/bin/env python3
"""Tests for host/linux/dc-modemd.py: the parsers, and a whole call against a fake modem on a pty."""
import importlib.machinery, importlib.util, os, pty, stat, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "..", "dc-modemd.py")
loader = importlib.machinery.SourceFileLoader("dcmodemd", PATH)
spec = importlib.util.spec_from_loader("dcmodemd", loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)

fails = 0
def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += 0 if ok else 1

# ---- parsers ----
r = m.Rx()
evs = [r.line_byte(b) for b in b"\r\nOK\r\n"]
check("OK line", "OK" in evs)
r = m.Rx(); evs = [r.line_byte(b) for b in b"\r\nCONNECT 33600\r\n"]
check("CONNECT with speed", "CONNECT" in evs and r.last == "CONNECT 33600")
r = m.Rx(); evs = [r.line_byte(b) for b in b"NO DIALTONE\r"]
check("NO DIALTONE is no carrier", "NO CARRIER" in evs)
r = m.Rx()
keys = [r.voice_byte(b) for b in bytes([0x10, ord('5'), 0x10, 0x10, 0x10, ord('*'), 0x10, ord('/'), 0x10, ord('~')])]
check("voice keys only", [k for k in keys if k] == ['5', '*'])
r = m.Rx(); got = [r.data_byte(b) for b in b"ppp\x7e\r\nNO CARRIER\r\n"]
check("NO CARRIER in data", got[-1] is True and not any(got[:-1]))
r = m.Rx(); got = [r.data_byte(b) for b in b"\r\r\nNO CARRIER\r\n"]
check("NO CARRIER after a stray CR", got[-1] is True)
for num, kind in [("", "idle"), ("*70", "idle"), ("0", "wait"), ("*69", "dcnet_prefix"), ("0642542154", "capcom"),
                  ("18002071194", "xband"), ("#1*2*3*4#", "netlink"), ("1234567", "ppp"), ("0120717360", "xband")]:
    check("classify %r -> %s" % (num, kind), m.classify(num) == kind)
t = m.tone_block()
check("tone: 800 samples, no DLE", len(t) == 800 and 0x10 not in t)

# ---- a whole call ----
tmp = tempfile.mkdtemp()
fake_pppd = os.path.join(tmp, "pppd")
with open(fake_pppd, "w") as f:
    f.write('#!/bin/sh\necho "$@" > %s/pppd.args\necho "local  IP address 192.168.98.1"\n'
            'echo "remote IP address 192.168.98.2"\necho "Connection terminated."\n' % tmp)
os.chmod(fake_pppd, 0o755)

master, slave = pty.openpty()
log = []
def fake_modem():
    voice, buf, tone_seen, sent = False, b"", False, False
    os.set_blocking(master, False)
    while True:
        time.sleep(0.005)
        try:
            d = os.read(master, 4096)
        except (BlockingIOError, OSError):
            d = b""
        if voice:
            if b"\x10\x03" in d:
                voice = False; log.append("end-tone"); buf = b""
            elif d:
                tone_seen = True
            if tone_seen and not sent:
                sent = True
                os.write(master, b"\x10" + b"1" + b"\x10" + b"2" + b"\x10" + b"3")
            continue
        buf += d
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            line = line.strip(b"\0\x10\x03\n").replace(b"+++", b"").decode()
            if not line.startswith("AT"):
                continue
            log.append(line)
            if line == "AT+VTX":
                os.write(master, b"\r\nCONNECT\r\n"); voice = True
            elif line == "ATA":
                os.write(master, b"\r\nCONNECT 33600\r\n")
            else:
                os.write(master, b"\r\nOK\r\n")
        if log and log[-1] == "ATA" and False:
            return
threading.Thread(target=fake_modem, daemon=True).start()

t0 = time.time()
p = subprocess.run([sys.executable, PATH, "--device", os.ttyname(slave), "--pppd", fake_pppd, "--no-nat",
                    "--once", "--key-timeout", "0.5", "--answer-delay", "0.3", "--dns", "9.9.9.9", "--dns-file", "", "--dns-port", "15353"],
                   capture_output=True, text=True, timeout=60)
out = p.stdout
print(out)
check("daemon exits cleanly", p.returncode == 0)
check("voice mode sequence", all(c in log for c in ["ATZ0", "AT+FCLASS=8", "AT+VLS=1", "AT+VSM=1,8000", "AT+VTX"]))
check("tone ended before ATA", "end-tone" in log and "ATA" in log and log.index("end-tone") < log.index("ATA"))
check("hangs up before answering", "ATH0" in log[log.index("end-tone"):log.index("ATA")])
check("dialled number heard", "the Dreamcast dialled 123" in out)
check("PPP started", "answered, CONNECT 33600" in out and os.path.exists(os.path.join(tmp, "pppd.args")))
if os.path.exists(os.path.join(tmp, "pppd.args")):
    a = open(os.path.join(tmp, "pppd.args")).read()
    check("pppd gets the addresses and DNS", "192.168.98.1:192.168.98.2" in a and "ms-dns 192.168.98.1" in a and "noauth" in a)
check("PPP over is reported", "PPP is over" in out)

# ---- DNS ----
import socket, struct
def query(name, qtype=1, qid=0x1234):
    q = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    for l in name.split("."):
        q += bytes([len(l)]) + l.encode()
    return q + b"\0" + struct.pack(">HH", qtype, 1)

up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); up.bind(("127.0.0.1", 0)); up.settimeout(5)
upport = up.getsockname()[1]
m_up = []
def fake_up():
    try:
        p, who = up.recvfrom(1500)
    except OSError:
        return
    m_up.append(p)
    qd = m.dns_parse(p)
    up.sendto(m.dns_answer_a(p, qd[4], "1.2.3.4"), who)
conf = m.DnsConf(["127.0.0.1"])
n = conf.add_text("# comment\naddress=/dcnow.example/10.0.0.9\nserver=/special.example/127.0.0.1\nserver=8.8.4.4\njunk\n")
check("dns conf: three lines taken", n == 3 and conf.up == ["127.0.0.1", "8.8.4.4"])
check("dns conf: override by suffix", conf.override("a.dcnow.example") == "10.0.0.9" and conf.override("xdcnow.example") is None)
check("dns conf: server= for a domain", conf.upstream("x.special.example") == ["127.0.0.1"])
check("dns: private reverse is local", m.dns_local_only("5.1.168.192.in-addr.arpa", 12) and not m.dns_local_only("5.1.8.8.in-addr.arpa", 12))
check("dns: plain name is local", m.dns_local_only("router", 1) and not m.dns_local_only("a.b", 1))
srv = m.DnsServer("127.0.0.1", 0, lambda: conf)
srv.up_port = upport
srv.start()
cl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); cl.settimeout(5)
def ask(name, qtype=1):
    cl.sendto(query(name, qtype), ("127.0.0.1", srv.port)); return cl.recvfrom(1500)[0]
r = ask("www.dcnow.example")
check("dns: override answers A 10.0.0.9", r[-4:] == bytes([10, 0, 0, 9]) and r[3] & 15 == 0 and r[:2] == b"\x12\x34")
r = ask("router")
check("dns: plain name gets NXDOMAIN", r[3] & 15 == 3)
t = threading.Thread(target=fake_up); t.start()
r = ask("game.example")
t.join()
check("dns: other names are forwarded and the answer returned", r[-4:] == bytes([1, 2, 3, 4]) and len(m_up) == 1)
srv.stop()
check("dns: counted", srv.stats["overridden"] == 1 and srv.stats["refused"] == 1 and srv.stats["forwarded"] == 1)

# ---- DCNet ----
check("dcnet: *69 prefix", m.dcnet_wanted("prefix", "*69555", False) == (True, "555") and m.dcnet_wanted("prefix", "555", False) == (False, "555"))
check("dcnet: prefix heard before", m.dcnet_wanted("prefix", "555", True)[0] is True)
check("dcnet: always and off", m.dcnet_wanted("always", "5", False)[0] and not m.dcnet_wanted("off", "*695", True)[0])
check("dcnet: Power Smash numbers use 7656", m.dcnet_port_for("005360010001") == 7656 and m.dcnet_port_for("005360010011") == 7654 and m.dcnet_port_for("555") == 7654)
st = [False]
check("dcnet: lone ~ is doubled", m.dcnet_fix_to_dc(st, b"a~b~~c") == b"a~~b~~c")
st = [False]; o = m.dcnet_fix_to_dc(st, b"x~") + m.dcnet_fix_to_dc(st, b"y")
check("dcnet: ~ across two reads", o == b"x~~y")

# a fake access point: discovery + ping on UDP, the call on TCP (the ports are set for the test)
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(("127.0.0.1", 0)); udp.settimeout(10)
tcp = socket.socket(); tcp.bind(("127.0.0.1", 0)); tcp.listen(1); tcp.settimeout(15)
mg = struct.pack("<I", m.DCNET_MAGIC)
got_call = []
def ap_udp():
    try:
        while True:
            p, who = udp.recvfrom(100)
            if p[4:5] == b"\x03":
                udp.sendto(mg + b"\x03" + socket.inet_aton("127.0.0.1") + bytes([4]) + b"test" + socket.inet_aton("127.0.0.2") + bytes([5]) + b"other", who)
            elif p[4:5] == b"\x01":
                udp.sendto(mg + b"\x02" + p[5:13], who)
    except OSError:
        pass
def ap_tcp():
    c, _ = tcp.accept(); c.settimeout(10)
    c.sendall(b"hello~world")
    d = b""
    while b"\r\nNO CARRIER\r\n" not in d:
        x = c.recv(100)
        if not x: break
        d += x
    got_call.append(d); c.close()
m.socket.gethostbyname = lambda h: "127.0.0.1"
m.DCNET_DISCO = udp.getsockname()[1]; m.DCNET_PORT = tcp.getsockname()[1]
threading.Thread(target=ap_udp, daemon=True).start(); tth = threading.Thread(target=ap_tcp); tth.start()
logs = []
ap = m.dcnet_best_ap(logs.append)
check("dcnet: closest access point found", ap is not None and ap[0] == "127.0.0.1" and ap[1] == "test")
pm, ps = pty.openpty(); os.set_blocking(pm, False)
mod = m.Modem(os.ttyname(ps))
def dc_side():
    time.sleep(1.0)
    os.write(pm, b"\x7epppdata")
    time.sleep(0.5)
    os.write(pm, b"\r\nNO CARRIER\r\n")
threading.Thread(target=dc_side).start()
res = m.dcnet_relay(mod, "555", logs.append)
tth.join(5)
back = b""
try: back = os.read(pm, 4096)
except OSError: pass
check("dcnet: Dreamcast bytes reach the access point", got_call and got_call[0].startswith(b"\x7epppdata"))
check("dcnet: access point bytes reach the Dreamcast with ~ doubled", back == b"hello~~world")
check("dcnet: call ended on NO CARRIER", res is True and any("NO CARRIER" in l for l in logs))
udp.close(); tcp.close()

# ---- the browser page's relay (WebSocket in, pppd on a pty) ----
import base64, hashlib
echo_pppd = os.path.join(tmp, "echo-pppd")
with open(echo_pppd, "w") as f:
    f.write("#!%s\nimport sys, os, tty\nfd = os.open(sys.argv[1], os.O_RDWR)\ntty.setraw(fd)\n"
            "print('local  IP address 192.168.98.1', flush=True)\n"
            "while True:\n    d = os.read(fd, 100)\n    if not d: break\n    os.write(fd, d.upper())\n" % sys.executable)
os.chmod(echo_pppd, 0o755)
RP = 18099
rp = subprocess.Popen([sys.executable, PATH, "--relay", "--relay-port", str(RP), "--pppd", echo_pppd, "--no-nat",
                       "--dns-file", "", "--dns-port", "15355", "--answer-delay", "0.3"],
                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
for _ in range(50):
    try: socket.create_connection(("127.0.0.1", RP), 1).close(); break
    except OSError: time.sleep(0.1)
def http(path, origin=None, upgrade=False):
    c = socket.create_connection(("127.0.0.1", RP), 5); c.settimeout(5)
    h = "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n" % (path, RP)
    if origin: h += "Origin: %s\r\n" % origin
    if upgrade: h += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
    c.sendall((h + "\r\n").encode())
    return c
def readall(c):
    d = b""
    try:
        while True:
            x = c.recv(65536)
            if not x: break
            d += x
    except OSError: pass
    return d
def wsframe(op, data):
    key = b"\x01\x02\x03\x04"
    n = len(data)
    h = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    return h + key + bytes(b ^ key[i & 3] for i, b in enumerate(data))
def wsread(c, buf):
    while True:
        if len(buf) >= 2:
            n, p = buf[1] & 0x7F, 2
            if n == 126 and len(buf) >= 4: n, p = struct.unpack(">H", buf[2:4])[0], 4
            if len(buf) >= p + n: return buf[0] & 15, buf[p:p + n], buf[p + n:]
        buf += c.recv(4096)
page = readall(http("/"))
check("relay serves the page", b"200 OK" in page and b"Dreamcast modem in the browser" in page)
check("relay refuses another site's page", b"403" in readall(http("/ws", "http://evil.example", True)))
c = http("/ws", "http://127.0.0.1:%d" % RP, True)
hs = c.recv(1000)
acc = base64.b64encode(hashlib.sha1(b"dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
check("relay: WebSocket handshake", b"101" in hs and acc in hs)
c.sendall(wsframe(1, b"dial #1*2*3*4#"))
op, d, buf = wsread(c, b"")
check("relay: Netlink number is reported as not carried out", op == 1 and d.startswith(b"unsupported netlink"))
c.close()
time.sleep(0.5)
c = http("/ws", "http://127.0.0.1:%d" % RP, True); c.recv(1000)
c.sendall(wsframe(1, b"dial 5551234"))
op, d, buf = wsread(c, b"")
check("relay: PPP number answered with the delay", op == 1 and d == b"ok ppp 0.3")
time.sleep(1.0)
c.sendall(wsframe(2, b"hello ppp"))
got = b""
c.settimeout(5)
while len(got) < 9:
    op, d, buf = wsread(c, buf)
    if op == 2: got += d
check("relay: bytes go through the pppd's tty both ways", got == b"HELLO PPP")
c.close()
time.sleep(1.0)
rp.terminate()
rlog = rp.communicate(timeout=10)[0]
check("relay: logs the call", "the Dreamcast dialled 5551234 (ppp)" in rlog and "PPP is over" in rlog)

# ---- the serial cable: Hayes stub, then PPP ----
def feed(h, text):
    out, ev = b"", None
    for c in text:
        o, e = h.byte(c); out += o; ev = e or ev
    return out, ev
h = m.Hayes()
o, e = feed(h, b"ATZ0\r")
check("Hayes: echo and OK", o == b"ATZ0\r\r\nOK\r\n" and e is None)
o, e = feed(h, b"ATE0V0\r")
check("Hayes: E0 V0 gives a number", o.endswith(b"0\r") and not h.echo and not h.verbose)
h = m.Hayes(); o, e = feed(h, b"ATDT5551234\r")
check("Hayes: dial gives CONNECT 33600", e == "dial" and b"CONNECT 33600" in o)
h = m.Hayes(); o, e = feed(h, b"\x7e")
check("Hayes: bare PPP flag", e == "ppp" and o == b"")
h = m.Hayes(); o, e = feed(h, b"noise\r")
check("Hayes: non-AT line gets no answer", o == b"noise\r" and e is None)

cm, cs = pty.openpty()
os.set_blocking(cm, False)
def cread(sec):
    end, d = time.time() + sec, b""
    while time.time() < end:
        try: d += os.read(cm, 4096)
        except (BlockingIOError, OSError): time.sleep(0.02)
    return d
for f in ("pppd.args",):
    try: os.remove(os.path.join(tmp, f))
    except OSError: pass
cp = subprocess.Popen([sys.executable, PATH, "--cable", os.ttyname(cs), "--pppd", fake_pppd, "--no-nat", "--once", "--dns-file", "", "--dns-port", "15354"],
                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(1.0)
os.write(cm, b"ATZ0\r"); r1 = cread(0.5)
os.write(cm, b"ATDT5551234\r"); r2 = cread(1.0)
try: cout = cp.communicate(timeout=15)[0]
except subprocess.TimeoutExpired: cp.kill(); cout = cp.communicate()[0]
print(cout)
check("cable: OK for ATZ0", b"OK" in r1)
check("cable: CONNECT after ATDT", b"CONNECT 33600" in r2)
check("cable: pppd started", os.path.exists(os.path.join(tmp, "pppd.args")) and "PPP is over" in cout)

import shutil
if shutil.which("node"):
    nj = subprocess.run(["node", os.path.join(HERE, "test_page.js")], capture_output=True, text=True)
    print(nj.stdout, end="")
    check("page parsers (node)", nj.returncode == 0)
else:
    print("skip page parsers (no node)")

sys.exit(1 if fails else 0)
