#!/usr/bin/env python3
"""
Talks PPP to the host build of ESP-IDF's lwIP PPP server (harness.c), behind our frame filter,
the way a Dreamcast does: LCP, then IPCP asking for an address and DNS, then a ping, then hang up.
usage: test_ppp.py /path/to/ppp_harness
"""
import os, select, struct, subprocess, sys, time

def fcs16(data, fcs=0xFFFF):
    for b in data:
        fcs ^= b
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if fcs & 1 else fcs >> 1
    return fcs

def encode(body):
    fcs = fcs16(body) ^ 0xFFFF
    body = body + struct.pack('<H', fcs)
    out = bytearray([0x7E])
    for c in body:
        if c < 0x20 or c in (0x7D, 0x7E):
            out += bytes([0x7D, c ^ 0x20])
        else:
            out.append(c)
    out.append(0x7E)
    return bytes(out)

class Decoder:
    def __init__(self): self.buf = bytearray(); self.esc = False; self.inf = False
    def feed(self, data):
        frames = []
        for c in data:
            if c == 0x7E:
                if self.inf and len(self.buf) >= 4 and fcs16(bytes(self.buf)) == 0xF0B8:
                    frames.append(bytes(self.buf[:-2]))
                self.buf = bytearray(); self.esc = False; self.inf = True
            elif self.inf:
                if c == 0x7D: self.esc = True
                else:
                    if self.esc: c ^= 0x20; self.esc = False
                    self.buf.append(c)
        return frames

def parse(frame):
    if frame[:2] == b'\xff\x03': frame = frame[2:]
    if frame[0] & 1: proto, rest = frame[0], frame[1:]
    else: proto, rest = (frame[0] << 8) | frame[1], frame[2:]
    return proto, rest

def opts(data):
    out = []; i = 0
    while i + 2 <= len(data):
        t, l = data[i], data[i + 1]
        if l < 2 or i + l > len(data): break
        out.append((t, data[i + 2:i + l])); i += l
    return out
def pack_opts(lst): return b''.join(bytes([t, len(v) + 2]) + v for t, v in lst)

def ip_sum(h):
    if len(h) % 2: h += b'\0'
    s = sum(struct.unpack('>%dH' % (len(h) // 2), h))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def icmp_echo(src, dst, ttl, ident=0x1234, seq=1, payload=b'dreamcast'):
    icmp = struct.pack('>BBHHH', 8, 0, 0, ident, seq) + payload
    icmp = icmp[:2] + struct.pack('>H', ip_sum(icmp)) + icmp[4:]
    ip = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(icmp), 1, 0, ttl, 1, 0, bytes(src), bytes(dst))
    ip = ip[:10] + struct.pack('>H', ip_sum(ip)) + ip[12:]
    return ip + icmp

def main():
    harness = sys.argv[1]
    p = subprocess.Popen([harness], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False); os.set_blocking(p.stderr.fileno(), False)
    dec = Decoder(); err = b''; backlog = []
    def pump(t=0.05):
        nonlocal err, backlog
        r, _, _ = select.select([p.stdout, p.stderr], [], [], t)
        got = []
        if p.stdout in r:
            try: got = dec.feed(os.read(p.stdout.fileno(), 65536))
            except BlockingIOError: pass
        if p.stderr in r:
            try: err += os.read(p.stderr.fileno(), 65536)
            except BlockingIOError: pass
        got, backlog = backlog + got, []
        return got
    def send(raw): p.stdin.write(raw); p.stdin.flush()
    def check(c, msg):
        if not c:
            print('FAIL:', msg); print(err.decode(errors='replace')); p.kill(); sys.exit(1)

    t0 = time.time()
    while b'READY' not in err and time.time() - t0 < 3: backlog += pump()
    check(b'READY' in err, 'harness did not start')

    # ---- LCP ----
    magic = 0xDC15C001
    my_lcp = [(1, struct.pack('>H', 1500)), (2, b'\x00\x00\x00\x00'), (5, struct.pack('>I', magic)), (7, b''), (8, b'')]
    ident = 1
    def lcp(code, i, data): return encode(b'\xff\x03\xc0\x21' + bytes([code, i]) + struct.pack('>H', 4 + len(data)) + data)
    send(lcp(1, ident, pack_opts(my_lcp)))
    lcp_ours_acked = lcp_theirs_acked = False
    server_lcp_opts = None
    t0 = time.time()
    while not (lcp_ours_acked and lcp_theirs_acked) and time.time() - t0 < 5:
        for f in pump():
            proto, rest = parse(f)
            if proto != 0xC021: continue
            code, i, ln = rest[0], rest[1], struct.unpack('>H', rest[2:4])[0]
            data = rest[4:ln]
            if code == 1:                                   # their request: accept it
                server_lcp_opts = opts(data); send(lcp(2, i, data)); lcp_theirs_acked = True
            elif code == 2: lcp_ours_acked = True
            elif code == 3:                                 # nak: take their values
                nk = dict(opts(data)); my_lcp = [(t, nk.get(t, v)) for t, v in my_lcp]
                ident += 1; send(lcp(1, ident, pack_opts(my_lcp)))
            elif code == 4:                                 # reject: drop those options
                rj = {t for t, _ in opts(data)}; my_lcp = [(t, v) for t, v in my_lcp if t not in rj]
                ident += 1; send(lcp(1, ident, pack_opts(my_lcp)))
    check(lcp_ours_acked and lcp_theirs_acked, 'LCP did not open')
    print('LCP open; the server asked for', [t for t, _ in server_lcp_opts])

    # ---- IPCP: ask for an address and both DNS servers ----
    def ipcp(code, i, data): return encode(b'\xff\x03\x80\x21' + bytes([code, i]) + struct.pack('>H', 4 + len(data)) + data)
    my_ip = [(3, bytes(4)), (0x81, bytes(4)), (0x83, bytes(4))]
    ident = 1
    send(ipcp(1, ident, pack_opts(my_ip)))
    ip_ours_acked = ip_theirs_acked = False
    their_ip = None
    t0 = time.time()
    while not (ip_ours_acked and ip_theirs_acked) and time.time() - t0 < 5:
        for f in pump():
            proto, rest = parse(f)
            if proto != 0x8021: continue
            code, i, ln = rest[0], rest[1], struct.unpack('>H', rest[2:4])[0]
            data = rest[4:ln]
            if code == 1:
                o = dict(opts(data)); their_ip = o.get(3); send(ipcp(2, i, data)); ip_theirs_acked = True
            elif code == 2: ip_ours_acked = True
            elif code == 3:
                nk = dict(opts(data)); my_ip = [(t, nk.get(t, v)) for t, v in my_ip]
                ident += 1; send(ipcp(1, ident, pack_opts(my_ip)))
            elif code == 4:
                rj = {t for t, _ in opts(data)}; my_ip = [(t, v) for t, v in my_ip if t not in rj]
                print('server rejected IPCP options', sorted(rj))
                ident += 1; send(ipcp(1, ident, pack_opts(my_ip)))
    check(ip_ours_acked and ip_theirs_acked, 'IPCP did not open')
    mine = dict(my_ip)
    ip = lambda b: '.'.join(map(str, b))
    print('IPCP open: our address %s, gateway %s, DNS %s' % (ip(mine[3]), ip(their_ip), ip(mine[0x81])))
    check(ip(mine[3]) == '10.64.0.2', 'Dreamcast address')
    check(ip(their_ip) == '10.64.0.1', 'gateway address')
    check(ip(mine[0x81]) == '10.64.0.1', 'DNS address handed out')
    t0 = time.time()
    while b'UP our=10.64.0.1 his=10.64.0.2' not in err and time.time() - t0 < 2: pump()
    check(b'UP our=10.64.0.1 his=10.64.0.2' in err, 'lwIP did not report the link up')

    # ---- ping the server with TTL 30; the filter sets it to 65 ----
    pkt = icmp_echo((10, 64, 0, 2), (10, 64, 0, 1), 30)
    send(encode(b'\xff\x03\x00\x21' + pkt))
    reply = None; t0 = time.time()
    while reply is None and time.time() - t0 < 3:
        for f in pump():
            proto, rest = parse(f)
            if proto == 0x0021 and rest[9] == 1 and rest[20] == 0: reply = rest
    check(reply is not None, 'no ping answer')
    check(reply[12:16] == bytes((10, 64, 0, 1)) and reply[16:20] == bytes((10, 64, 0, 2)), 'ping answer addresses')
    check(reply[20 + 8:] == b'dreamcast', 'ping answer payload')
    print('ping answered (answer TTL %d)' % reply[8])

    # ---- a ping to somewhere else gets forwarded? (no uplink in this test: expect nothing, no crash) ----
    send(encode(b'\xff\x03\x00\x21' + icmp_echo((10, 64, 0, 2), (8, 8, 8, 8), 30, seq=2)))
    pump(0.3)

    # ---- hang up with LCP Terminate-Request; the server answers and reports ----
    send(lcp(5, 99, b''))
    got = False; t0 = time.time()
    while time.time() - t0 < 3:
        for f in pump():
            proto, rest = parse(f)
            if proto == 0xC021 and rest[0] == 6: got = True
        if got and b'STATUS' in err.split(b'UP our')[-1]: break
    check(got, 'no Terminate-Ack')
    # ---- the Dreamcast is back at its modem: AT where a frame should start ----
    send(b'ATZ\r')
    t0 = time.time()
    while b'SAW_AT' not in err and time.time() - t0 < 2: pump()
    check(b'SAW_AT' in err, 'AT not noticed')
    p.stdin.close(); time.sleep(0.2); pump(0.2)
    try: p.wait(timeout=3)
    except Exception: p.kill()
    stats = [l for l in err.decode(errors='replace').splitlines() if l.startswith('STATS')]
    print(stats[-1] if stats else 'no stats')
    check(stats and 'ttl=' in stats[-1] and 'ttl=0' not in stats[-1], 'the TTL filter did not act')
    print('ppp negotiation: ok')

main()
