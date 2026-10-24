#!/usr/bin/env python3
"""dctest.py: host side of the DC-PicoLink serial / W5500 test tool (Dreamcast side: dctest.c).

Runs on the Mac or the DreamPi (needs pyserial for the real cable: pip install pyserial).
Every run writes a timestamped log (and a .jsonl with one result per test) that can be
shared as is. See README.md.

    dctest.py ports
    dctest.py hello  --port P
    dctest.py serial --port P [--bauds common|uncommon|all|1200,...] [--step-seconds 6] [--full]
    dctest.py ramp   --port P --baud 115200
    dctest.py hop    --port P --bauds 115200,1500000 --cycles 50
    dctest.py net    --port P | --dc-ip IP
    dctest.py switch --port P --cycles 10
    dctest.py soak   --port P --minutes 30
    dctest.py selftest
"""
import argparse, collections, json, os, random, select, socket, struct, subprocess, sys
import threading, time, zlib

# ---------------------------------------------------------------- protocol (mirror of proto.h)
S0, S1, HDR, MAXPAY = 0xD3, 0x7C, 10, 2048
HELLO, ECHO, SINK, STATS, CLEAR, SRC, SETBAUD, DATA, NETSTART, NETSTOP, TCPSTART = range(1, 12)
REPLY = 0x80
PAT_NAMES = ["random", "zero", "ones", "alt", "ramp", "walk", "ppp", "edges"]
STAT_FIELDS = ["frames_ok", "crc_bad", "resync", "bytes_rx", "bytes_tx", "pat_bad", "seq_gap",
               "frames_tx", "baud", "uptime_ms", "reverts", "echo_frames", "sink_frames"]
M32 = 0xFFFFFFFF


def pattern(pat, seed, seq, n):
    x = (seed ^ (seq * 0x9E3779B1)) & M32 or 1
    out = bytearray(n)
    edges = (0x00, 0xFF, 0x80, 0x01, 0x7F, 0xFE)
    for i in range(n):
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        r = (x >> 8) & 0xFF
        if pat == 0: b = r
        elif pat == 1: b = 0x00
        elif pat == 2: b = 0xFF
        elif pat == 3: b = 0xAA if (i + seq) & 1 else 0x55
        elif pat == 4: b = (i + seq) & 0xFF
        elif pat == 5: b = 1 << ((i + seq) & 7)
        elif pat == 6: b = 0x7E if r < 48 else 0x7D if r < 80 else 0x11 if r < 96 else 0x13 if r < 112 else r
        else: b = edges[(i + seq) % 6]
        out[i] = b
    return bytes(out)


def encode(typ, payload=b"", flags=0, seq=0):
    body = struct.pack("<BBIH", typ, flags, seq, len(payload)) + payload
    return bytes((S0, S1)) + body + struct.pack("<I", zlib.crc32(body))


Frame = collections.namedtuple("Frame", "type flags seq payload")


class Parser:
    """Stream parser with resync; counters match the C parser."""
    def __init__(self):
        self.buf = bytearray()
        self.crc_bad = self.resync = 0

    def feed(self, data):
        self.buf += data
        out, b = [], self.buf
        while True:
            if b and b[0] != S0:
                i = b.find(bytes((S0,)))
                k = len(b) if i < 0 else i
                self.resync += k
                del b[:k]
                continue
            if len(b) >= 2 and b[1] != S1:
                del b[:1]; self.resync += 1
                continue
            if len(b) < HDR:
                break
            n = b[8] | b[9] << 8
            if n > MAXPAY:
                self.crc_bad += 1; self.resync += 1; del b[:1]
                continue
            total = HDR + n + 4
            if len(b) < total:
                break
            if zlib.crc32(bytes(b[2:HDR + n])) != struct.unpack_from("<I", b, HDR + n)[0]:
                self.crc_bad += 1; self.resync += 1; del b[:1]
                continue
            out.append(Frame(b[2], b[3], struct.unpack_from("<I", b, 4)[0], bytes(b[HDR:HDR + n])))
            del b[:total]
        return out


def dc_effective(req):
    """The rate the DC really runs for a baud request (dcload / KOS divider, see baud_model.h)."""
    if req > 115200:
        return 1562500.0 / (((1562500 // req) - 1) % 256 + 1)
    return float(req)


# ---------------------------------------------------------------- logging
class Log:
    def __init__(self, path):
        self.t0 = time.time()
        self.f = open(path, "w", buffering=1)
        self.j = open(os.path.splitext(path)[0] + ".jsonl", "w", buffering=1)
        self.lock = threading.Lock()
        self.results = []
        self.fw_lines = collections.Counter()

    def line(self, msg, prefix=""):
        s = "[%8.3f] %s%s" % (time.time() - self.t0, prefix, msg)
        with self.lock:
            print(s, flush=True)
            self.f.write(s + "\n")

    def result(self, r):
        self.results.append(r)
        with self.lock:
            self.j.write(json.dumps(dict(r, t=round(time.time() - self.t0, 3))) + "\n")
        tag = "PASS" if r["ok"] else ("INFO" if r.get("expected") else "FAIL")
        extra = " ".join("%s=%s" % (k, v) for k, v in r.items()
                         if k not in ("ok", "name", "expected", "note") and v not in (None, 0, ""))
        self.line("%s %-10s %s%s" % (tag, r["name"], extra, ("  (" + r["note"] + ")") if r.get("note") else ""))

    def fw_watch(self, cmd):
        """Merge another command's output (e.g. `journalctl -fu dcsd -n0`, or ssh pi ...) into the log."""
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        def run():
            for ln in p.stdout:
                ln = ln.rstrip()
                self.line(ln, "fw| ")
                low = ln.lower()
                for key in ("resync", "underrun", "late", "error", "bad", "overrun"):
                    if key in low:
                        self.fw_lines[key] += 1
        threading.Thread(target=run, daemon=True).start()
        return p

    def summary(self):
        bad = [r for r in self.results if not r["ok"] and not r.get("expected")]
        self.line("=" * 60)
        self.line("%d tests, %d passed, %d failed, %d informational" % (
            len(self.results), sum(r["ok"] for r in self.results), len(bad),
            sum(1 for r in self.results if not r["ok"] and r.get("expected"))))
        if self.fw_lines:
            self.line("firmware log lines mentioning: " + ", ".join("%s x%d" % kv for kv in sorted(self.fw_lines.items())))
        for r in bad:
            self.line("FAILED: " + json.dumps(r))
        return not bad


# ---------------------------------------------------------------- channels
class SerialChan:
    def __init__(self, port, baud, rtscts=False):
        import serial  # pip install pyserial
        self.s = serial.Serial(port, baud, timeout=0, write_timeout=15, rtscts=rtscts)
        self.baud = baud

    def set_baud(self, b):
        self.s.baudrate = b
        self.baud = b

    def write(self, b):
        self.s.write(b)

    def read(self, timeout):
        end = time.perf_counter() + timeout
        while True:
            n = self.s.in_waiting
            if n:
                return self.s.read(n)
            if time.perf_counter() >= end:
                return b""
            time.sleep(0.0005)

    def drain(self):
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()


class ProcChan:
    """The host build of dctest.c over pipes, optionally corrupting one bit per `noise` bytes."""
    def __init__(self, exe, noise=0):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.fd = self.p.stdout.fileno()
        self.noise, self.count, self.baud = noise, 0, 0

    def _hit(self, data):
        if not self.noise:
            return data
        b = bytearray(data)
        for i in range(len(b)):
            self.count += 1
            if self.count % self.noise == 0:
                b[i] ^= 1 << (self.count // self.noise % 8)
        return bytes(b)

    def set_baud(self, b):
        self.baud = b

    def write(self, b):
        self.p.stdin.write(self._hit(b))

    def read(self, timeout):
        if select.select([self.fd], [], [], timeout)[0]:
            return self._hit(os.read(self.fd, 65536))
        return b""

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, 65536)

    def close(self):
        self.p.kill()


class UdpChan:
    def __init__(self, port, addr=None):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.s.bind(("", 0))                      # the DC answers to whatever port we send from
        self.port, self.addr = port, addr

    def write(self, b):
        self.s.sendto(b, (self.addr, self.port))

    def read(self, timeout):
        if select.select([self.s], [], [], timeout)[0]:
            data, src = self.s.recvfrom(65536)
            if self.addr is None or src[0] == self.addr:
                return data
        return b""

    def drain(self):
        while select.select([self.s], [], [], 0)[0]:
            self.s.recvfrom(65536)

    def discover(self, timeout):
        """Wait for the DC's beacon (or any dctest frame) and remember its address."""
        end = time.time() + timeout            # the DC broadcasts its beacon to port + 1
        b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        b.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        b.bind(("", self.port + 1))
        p = Parser()
        try:
            while time.time() < end:
                if select.select([b], [], [], 0.5)[0]:
                    data, src = b.recvfrom(65536)
                    if p.feed(data):
                        self.addr = src[0]
                        return src[0]
        finally:
            b.close()
        return None

    def close(self):
        self.s.close()


class TcpChan:
    def __init__(self, addr, port, timeout=20):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((addr, port), 2)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.3)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def write(self, b):
        self.s.sendall(b)

    def read(self, timeout):
        if select.select([self.s], [], [], timeout)[0]:
            return self.s.recv(65536)
        return b""

    def drain(self):
        pass

    def close(self):
        self.s.close()


# ---------------------------------------------------------------- link: frames over a channel
class Link:
    def __init__(self, chan):
        self.chan, self.parser, self.q = chan, Parser(), collections.deque()
        self.rid = 0x40000000

    def send(self, typ, payload=b"", flags=0, seq=0):
        self.chan.write(encode(typ, payload, flags, seq))

    def pump(self, timeout=0.0):
        data = self.chan.read(timeout)
        if data:
            self.q.extend(self.parser.feed(data))
        return bool(data)

    def get(self, timeout):
        end = time.perf_counter() + timeout
        while True:
            if self.q:
                return self.q.popleft()
            left = end - time.perf_counter()
            if left <= 0:
                return None
            self.pump(min(left, 0.05))

    def quiesce(self, quiet=0.15):
        """Throw away whatever is still arriving (late replies of an earlier test)."""
        end = time.perf_counter() + quiet
        while time.perf_counter() < end:
            if self.chan.read(0.02):
                end = time.perf_counter() + quiet
        self.q.clear()
        self.parser.buf.clear()

    def request(self, typ, payload=b"", timeout=1.0, tries=1):
        for _ in range(tries):
            self.rid += 1
            self.send(typ, payload, seq=self.rid)
            end = time.perf_counter() + timeout
            while True:
                f = self.get(max(0.0, end - time.perf_counter()))
                if f is None:
                    break
                if f.type == typ | REPLY and f.seq == self.rid:
                    return f
        return None

    def stats(self, tries=5):
        f = self.request(STATS, timeout=1.0, tries=tries)
        if not f or len(f.payload) < 52:
            return None
        return dict(zip(STAT_FIELDS, struct.unpack("<13I", f.payload[:52])))


def pace(deadline):
    while True:
        d = deadline - time.perf_counter()
        if d <= 0:
            return
        if d > 0.003:
            time.sleep(d - 0.002)


# ---------------------------------------------------------------- test primitives
class Ctx:
    def __init__(self, link, log, args, tr=None):
        self.link, self.log, self.args, self.tr = link, log, args, tr
        self.baud = args.start_baud if hasattr(args, "start_baud") else 0
        self.echo_seq = 0
        self.rng = random.Random(args.seed)


def stall_for(c, length, window=1, interval=0.0):
    """Seconds without progress after which outstanding frames count as lost."""
    baud = getattr(c.tr, "baud", 0) or 0
    ft = (length + 14) * 10.0 / baud if baud else 0.01      # one frame on the wire
    return max(1.0, 4 * window * ft + 4 * interval + 0.5)


def frames_for(rate_bps, seconds, length, lo=3, hi=4000):
    return max(lo, min(hi, int(rate_bps / 10.0 * seconds / (length + 14))))


def echo_run(c, name, count, length, pat, seed, window=8, interval=0.0, **info):
    """Send `count` ECHO frames (window outstanding), verify every reply. Duplex traffic."""
    link = c.link
    link.quiesce()
    p0 = (link.parser.crc_bad, link.parser.resync)
    pend, ok, bad, dup = {}, 0, 0, 0
    base = c.echo_seq
    c.echo_seq += count + 1000
    sent = 0
    t0 = time.perf_counter()
    nxt = t0
    last = t0
    stall = stall_for(c, length, window, interval)
    while sent < count or pend:
        while sent < count and len(pend) < window:
            if interval:
                pace(nxt)
                nxt += interval
            seq = base + sent
            pay = pattern(pat, seed, seq, length)
            pend[seq] = pay
            link.send(ECHO, pay, flags=pat, seq=seq)
            sent += 1
            last = time.perf_counter()
        link.pump(0.002 if len(pend) >= window or sent < count else 0.02)
        while link.q:
            f = link.q.popleft()
            if f.type != ECHO | REPLY:
                continue
            exp = pend.pop(f.seq, None)
            if exp is None:
                dup += 1
            elif exp == f.payload and f.flags == pat:
                ok += 1
                last = time.perf_counter()
            else:
                bad += 1
        if pend and time.perf_counter() - last > stall:
            break
    el = time.perf_counter() - t0
    lost = len(pend)
    hb = link.parser.crc_bad - p0[0]
    c.log.result(dict(name=name, ok=lost == 0 and bad == 0 and dup == 0 and hb == 0, pat=PAT_NAMES[pat], len=length,
                      n=count, lost=lost, bad=bad, dup=dup, host_crc=hb, host_skipped=link.parser.resync - p0[1],
                      kBps_each_way=round(count * length / el / 1000.0, 1), **info))
    return lost == 0 and bad == 0


def sink_run(c, name, count, length, pat, seed, interval=0.0, **info):
    """Host -> DC. The DC checks every payload; its counters are compared afterwards."""
    link = c.link
    link.quiesce()
    if not link.request(CLEAR, struct.pack("<I", seed), tries=5):
        c.log.result(dict(name=name, ok=False, note="DC does not answer CLEAR", **info))
        return False
    t0 = nxt = time.perf_counter()
    for i in range(count):
        if interval:
            pace(nxt)
            nxt += interval
        link.send(SINK, pattern(pat, seed, i, length), flags=pat, seq=i)
    sent_t = time.perf_counter() - t0
    s, last_n, last_change = None, -1, time.perf_counter()
    while True:                                  # wait until the DC has counted everything or stops changing
        s = link.stats()
        if s is None:
            break
        if s["sink_frames"] != last_n:
            last_n, last_change = s["sink_frames"], time.perf_counter()
        if s["sink_frames"] + s["crc_bad"] >= count or time.perf_counter() - last_change > 2.0:
            break
        time.sleep(0.05)
    el = last_change - t0 if s else sent_t
    if not s:
        c.log.result(dict(name=name, ok=False, note="DC does not answer STATS", **info))
        return False
    ok = s["sink_frames"] == count and s["pat_bad"] == 0 and s["crc_bad"] == 0 and s["seq_gap"] == 0
    c.log.result(dict(name=name, ok=ok, pat=PAT_NAMES[pat], len=length, n=count, dc_got=s["sink_frames"],
                      dc_crc_bad=s["crc_bad"], dc_skipped=s["resync"], dc_pat_bad=s["pat_bad"], dc_gap=s["seq_gap"],
                      kBps=round(count * length / max(el, 1e-6) / 1000.0, 1), **info))
    return ok


def src_run(c, name, count, length, pat, seed, interval_us=0, **info):
    """DC -> host. The host checks every payload and counts missing sequence numbers."""
    link = c.link
    link.quiesce()
    p0 = (link.parser.crc_bad, link.parser.resync)
    link.rid += 1
    rid = link.rid
    link.send(SRC, struct.pack("<IHBBII", count, length, pat, 0, seed, interval_us), seq=rid)
    got, bad, dup, done = set(), 0, 0, False
    t0 = last = time.perf_counter()
    stall = stall_for(c, length, 4, interval_us / 1e6)
    t_last = t0
    while len(got) + bad < count:
        f = link.get(0.1)
        if f is None:
            if time.perf_counter() - last > stall:
                break
            continue
        last = time.perf_counter()
        if f.type == SRC | REPLY and f.seq == rid:
            done = True
            if not link.q:
                link.pump(0.05)
            if not link.q:
                break
            continue
        if f.type != DATA:
            continue
        if f.seq in got:
            dup += 1
        elif f.seq < count and f.payload == pattern(pat, seed, f.seq, length) and f.flags == pat:
            got.add(f.seq); t_last = last
        else:
            bad += 1
    el = t_last - t0
    lost = count - len(got) - bad
    hb = link.parser.crc_bad - p0[0]
    c.log.result(dict(name=name, ok=lost == 0 and bad == 0 and dup == 0 and hb == 0, pat=PAT_NAMES[pat], len=length,
                      n=count, lost=lost, bad=bad, dup=dup, host_crc=hb, host_skipped=link.parser.resync - p0[1],
                      kBps=round(len(got) * length / max(el, 1e-6) / 1000.0, 1), **info))
    return lost == 0 and bad == 0


def hello(link, tries=10, timeout=0.3):
    f = link.request(HELLO, timeout=timeout, tries=tries)
    return f.payload.decode(errors="replace") if f else None


def set_baud(c, new):
    """Move DC and cable to `new`. Returns True if the DC answers there, else restores the old rate."""
    link, old = c.link, c.baud
    link.quiesce(0.05)
    r = link.request(SETBAUD, struct.pack("<I", new), timeout=1.0, tries=3)
    if not r:
        c.log.line("baud %d: DC did not acknowledge the switch" % new)
        return False
    time.sleep(0.15)
    c.tr.set_baud(new)
    link.chan.drain() if hasattr(link.chan, "drain") else None
    link.q.clear(); link.parser.buf.clear()
    c.baud = new
    h = hello(link, tries=8, timeout=0.4)
    if h:
        return True
    c.log.line("baud %d: no answer; DC reverts to %d in 5 s" % (new, old))
    c.tr.set_baud(old)
    c.baud = old
    time.sleep(6.0)
    link.quiesce(0.3)
    if hello(link, tries=6, timeout=0.5):
        return True
    raise RuntimeError("lost the DC after trying baud %d (restart dctest on the Dreamcast)" % new)


# ---------------------------------------------------------------- test suites
COMMON = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 921600, 1000000, 1500000]
UNCOMMON = [14400, 28800, 31250, 76800, 153600, 200000, 250000, 256000, 312500, 614400, 750000, 1562500, 1600000, 2000000]


def parse_bauds(s):
    if s == "common": return COMMON
    if s == "uncommon": return UNCOMMON
    if s == "all": return sorted(set(COMMON + UNCOMMON))
    return [int(x) for x in s.split(",")]


def serial_step(c, baud, full):
    eff = dc_effective(baud)
    secs = c.args.step_seconds
    pats = list(range(len(PAT_NAMES))) if full else [0, 6, 7]
    lens = [1, 7, 64, 256, 1024, 2048] if full else [1, 64, 1024]
    tag = dict(baud=baud, dc_rate=round(eff) if round(eff) != baud else None)
    nsub = len(pats) * 1 + len(lens) + 2
    per = secs / nsub
    for pat in pats:                              # duplex echo, every pattern
        ln = 512
        echo_run(c, "echo", frames_for(eff, per, ln), ln, pat, c.rng.getrandbits(32), **tag)
    for ln in lens:                               # frame sizes, random data
        echo_run(c, "echo", frames_for(eff, per, ln), ln, 0, c.rng.getrandbits(32), **tag)
    sink_run(c, "sink", frames_for(eff, per * 2, 512), 512, 6, c.rng.getrandbits(32), **tag)
    src_run(c, "src", frames_for(eff, per * 2, 512), 512, 6, c.rng.getrandbits(32), **tag)
    for gap in (0.1, 1.0, 4.0) if full else (0.1, 1.0):   # first frame after an idle line
        time.sleep(gap)
        echo_run(c, "idle%.1fs" % gap, 3, 16, 0, c.rng.getrandbits(32), window=1, **tag)


def cmd_serial(c):
    bauds = parse_bauds(c.args.bauds)
    c.log.line("baud list: %s" % bauds)
    for b in bauds:
        c.log.line("---- %d baud (DC runs %.0f)%s" % (b, dc_effective(b), "  [divider wraps: expected 6103]" if b > 1562500 else ""))
        try:
            ok = set_baud(c, b)
        except RuntimeError as e:
            c.log.result(dict(name="baud", ok=False, baud=b, note=str(e)))
            return
        if not ok:
            c.log.result(dict(name="baud", ok=False, baud=b, note="DC did not answer at this rate"))
            continue
        serial_step(c, b, c.args.full)
    set_baud(c, 115200)


def cmd_ramp(c):
    """Offered load stepped up and down; duplex echo, host->DC and DC->host."""
    b = c.args.baud
    if b != c.baud and not set_baud(c, b):
        c.log.result(dict(name="baud", ok=False, baud=b, note="DC did not answer at this rate"))
        return
    eff = dc_effective(b)
    loads = [5, 10, 25, 50, 75, 90, 100]
    seq = loads + loads[-2::-1]
    ln = c.args.length
    for mode in c.args.modes.split(","):
        for load in seq:
            wire = eff / 10.0 * load / 100.0              # bytes/s per direction
            n = max(3, int(wire * c.args.step_seconds / (ln + 14)))
            iv = (ln + 14) / wire
            seed = c.rng.getrandbits(32)
            if mode == "echo":
                echo_run(c, "ramp-echo", n, ln, 0, seed, window=16, interval=iv, baud=b, load="%d%%" % load)
            elif mode == "sink":
                sink_run(c, "ramp-sink", n, ln, 0, seed, interval=iv, baud=b, load="%d%%" % load)
            else:
                src_run(c, "ramp-src", n, ln, 0, seed, interval_us=int(iv * 1e6), baud=b, load="%d%%" % load)


def cmd_hop(c):
    bauds = parse_bauds(c.args.bauds)
    for i in range(c.args.cycles):
        b = bauds[i % len(bauds)]
        if not set_baud(c, b):
            c.log.result(dict(name="hop", ok=False, baud=b, cycle=i, note="no answer"))
            continue
        echo_run(c, "hop", 20, 128, c.rng.randrange(8), c.rng.getrandbits(32), baud=b, cycle=i)
    set_baud(c, 115200)


def run_net_suite(c, quick):
    """c.link is a UDP link to the DC."""
    link = c.link
    h = hello(link, tries=10, timeout=0.5)
    if not h:
        c.log.result(dict(name="net-hello", ok=False, note="no answer over UDP"))
        return
    c.log.line("DC: " + h)
    c.tr = None
    rtts = []
    for i in range(100 if quick else 300):                     # latency: one frame at a time
        t = time.perf_counter()
        f = link.request(ECHO, os.urandom(64), timeout=0.5)
        if f:
            rtts.append((time.perf_counter() - t) * 1000)
    if rtts:
        rtts.sort()
        c.log.result(dict(name="udp-rtt", ok=len(rtts) >= len(rtts) * 0.99 and len(rtts) > (90 if quick else 290),
                          n=len(rtts), min_ms=round(rtts[0], 2), med_ms=round(rtts[len(rtts) // 2], 2),
                          p99_ms=round(rtts[int(len(rtts) * .99) - 1], 2), max_ms=round(rtts[-1], 2)))
    for ln in (1, 64, 512, 1000, 1400):
        echo_run(c, "udp-echo", 100 if quick else 400, ln, c.rng.randrange(8), c.rng.getrandbits(32), window=4)
    steps = [250, 500, 1000, 2000, 4000, 8000] if not quick else [500, 2000]    # kbit/s offered, up then down
    for kb in steps + steps[-2::-1]:
        ln = 1000
        iv = (ln + 14) * 8 / (kb * 1000.0)
        n = max(20, int(c.args.step_seconds / iv))
        sink_run(c, "udp-sink", n, ln, 0, c.rng.getrandbits(32), interval=iv, offered_kbit=kb)
        src_run(c, "udp-src", n, ln, 0, c.rng.getrandbits(32), interval_us=int(iv * 1e6), offered_kbit=kb)
    # TCP over the same frame protocol
    port = c.args.tcp_port
    r = link.request(TCPSTART, struct.pack("<H", port), timeout=1.0, tries=3)
    if not r:
        c.log.result(dict(name="tcp", ok=False, note="DC did not accept TCPSTART"))
        return
    try:
        tcp = Link(TcpChan(link.chan.addr, port))
    except OSError as e:
        c.log.result(dict(name="tcp", ok=False, note="connect failed: %s" % e))
        return
    tc = Ctx(tcp, c.log, c.args)
    tc.echo_seq = 0
    for ln in (64, 1000):
        echo_run(tc, "tcp-echo", 100 if quick else 400, ln, 0, c.rng.getrandbits(32), window=8)
    mb = 1 if quick else 8
    sink_run(tc, "tcp-sink", mb * 1000, 1000, 0, c.rng.getrandbits(32), size_MB=mb)
    src_run(tc, "tcp-src", mb * 1000, 1000, 0, c.rng.getrandbits(32), size_MB=mb)
    tcp.request(NETSTOP, timeout=1.0)
    tcp.chan.close()
    time.sleep(0.5)


def net_up(c, port, wait=60):
    """Serial -> W5500: returns a UDP link to the DC, or None."""
    r = c.link.request(NETSTART, struct.pack("<H", port), timeout=1.0, tries=3)
    if not r:
        c.log.result(dict(name="netstart", ok=False, note="DC did not acknowledge NETSTART"))
        return None
    udp = UdpChan(port, c.args.dc_ip)
    if not c.args.dc_ip:
        ip = udp.discover(wait)
        if not ip:
            c.log.result(dict(name="netstart", ok=False, note="no beacon from the DC in %ds (DHCP? use --dc-ip)" % wait))
            udp.close()
            return None
        c.log.line("DC found at " + ip)
    ul = Link(udp)
    if not hello(ul, tries=int(wait * 2), timeout=0.5):        # the DC listens only after its beacon phase
        c.log.result(dict(name="netstart", ok=False, note="DC does not answer over UDP"))
        udp.close()
        return None
    return ul


def net_down(c, ulink, baud):
    """W5500 -> serial: NETSTOP, then wait for the DC to answer on the cable again. Returns seconds or None."""
    t = time.perf_counter()
    ulink.request(NETSTOP, timeout=1.0, tries=3)
    ulink.chan.close()
    c.link.quiesce(0.2)
    for _ in range(60):
        if hello(c.link, tries=1, timeout=0.5):
            return time.perf_counter() - t
    return None


def cmd_net(c):
    if c.args.port:
        ul = net_up(c, c.args.udp_port)
        if not ul:
            return
    else:
        udp = UdpChan(c.args.udp_port, c.args.dc_ip)
        if not c.args.dc_ip:
            udp.discover(60)
        ul = Link(udp)
    nc = Ctx(ul, c.log, c.args)
    run_net_suite(nc, c.args.quick)
    if c.args.port:
        t = net_down(c, ul, c.baud)
        c.log.result(dict(name="back-to-serial", ok=t is not None, seconds=round(t, 2) if t else None))
        if t:
            echo_run(c, "echo-after", 50, 256, 0, c.rng.getrandbits(32), baud=c.baud)


def cmd_switch(c):
    """Serial <-> W5500 cycles at different baud rates."""
    bauds = parse_bauds(c.args.bauds)
    for i in range(c.args.cycles):
        b = bauds[i % len(bauds)]
        c.log.line("---- cycle %d: serial at %d, then W5500" % (i + 1, b))
        if not set_baud(c, b):
            c.log.result(dict(name="switch", ok=False, cycle=i, baud=b, note="no answer on serial"))
            continue
        echo_run(c, "echo-before", 50, 256, 0, c.rng.getrandbits(32), baud=b, cycle=i)
        ul = net_up(c, c.args.udp_port)
        if not ul:
            if not (hello(c.link, tries=20, timeout=0.5)):
                c.log.result(dict(name="switch", ok=False, cycle=i, note="DC lost"))
                return
            continue
        nc = Ctx(ul, c.log, c.args)
        echo_run(nc, "net-echo", 100, 1000, 0, c.rng.getrandbits(32), window=4, cycle=i)
        sink_run(nc, "net-sink", 200, 1000, 0, c.rng.getrandbits(32), interval=0.005, cycle=i)
        t = net_down(c, ul, b)
        c.log.result(dict(name="back-to-serial", ok=t is not None, seconds=round(t, 2) if t else None, cycle=i, baud=b))
        if t is None:
            return
        echo_run(c, "echo-after", 50, 256, 0, c.rng.getrandbits(32), baud=b, cycle=i)


def cmd_soak(c):
    end = time.time() + c.args.minutes * 60
    bauds = parse_bauds(c.args.bauds)
    n = 0
    while time.time() < end:
        n += 1
        r = c.rng
        if r.random() < 0.15:
            b = r.choice(bauds)
            if not set_baud(c, b):
                c.log.result(dict(name="baud", ok=False, baud=b))
                continue
        eff = dc_effective(c.baud)
        ln = r.choice([1, 2, 3, 17, 64, 255, 256, 700, 1024, 2048])
        pat = r.randrange(8)
        load = r.choice([0.1, 0.3, 0.6, 1.0, 1.0])
        wire = eff / 10.0 * load
        cnt = max(3, min(2000, int(wire * r.uniform(0.5, 4) / (ln + 14))))
        iv = (ln + 14) / wire
        seed = r.getrandbits(32)
        kind = r.choice(["echo", "echo", "sink", "src"])
        tag = dict(baud=c.baud, load="%d%%" % (load * 100), op=n)
        if kind == "echo": echo_run(c, "soak-echo", cnt, ln, pat, seed, window=r.choice([1, 4, 16]), interval=iv if load < 1 else 0, **tag)
        elif kind == "sink": sink_run(c, "soak-sink", cnt, ln, pat, seed, interval=iv if load < 1 else 0, **tag)
        else: src_run(c, "soak-src", cnt, ln, pat, seed, interval_us=int(iv * 1e6) if load < 1 else 0, **tag)
        if r.random() < 0.2:
            time.sleep(r.choice([0.05, 0.5, 3.0]))


# ---------------------------------------------------------------- selftest (against the host build of dctest.c)
def selftest(args, log):
    here = os.path.dirname(os.path.abspath(__file__))
    exe = os.path.join(args.tmp or here, "dctest-host")
    subprocess.check_call(["gcc", "-O1", "-Wall", "-DDCTEST_HOST", os.path.join(here, "dctest.c"), "-o", exe])
    args.step_seconds, args.full, args.seed = 0.3, False, 1
    args.dc_ip, args.udp_port, args.tcp_port, args.quick, args.port = "127.0.0.1", 17777, 17778, True, "x"
    args.start_baud = 115200

    def session(noise):
        ch = ProcChan(exe, noise)
        ch.baud = 115200
        c = Ctx(Link(ch), log, args, ch)
        c.baud = 115200
        if not hello(c.link):
            raise RuntimeError("selftest: DC process does not answer")
        return c

    log.line("selftest 1/2: clean link, everything must pass")
    c = session(0)
    args.bauds = "115200,1500000"
    cmd_serial(c)
    args.baud, args.modes, args.length = 115200, "echo,sink,src", 128
    cmd_ramp(c)
    args.cycles = 2
    cmd_switch(c)
    cmd_net(c)                                   # UDP latency/size/load ramps and TCP echo/sink/source
    c.link.chan.close()
    clean = log.summary()
    n_clean = len(log.results)
    log.line("selftest 2/2: 1 bit flip per 3000 bytes, the tool must report failures")
    log.results.clear()
    c = session(3000)
    args.bauds = "115200"
    try:
        cmd_serial(c)
    except RuntimeError:
        pass
    c.link.chan.close()
    failed = [r for r in log.results if not r["ok"]]
    log.line("noisy run: %d of %d tests reported errors" % (len(failed), len(log.results)))
    good = clean and n_clean > 20 and len(failed) > 0
    log.line("SELFTEST " + ("PASS" if good else "FAIL"))
    return good


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, serial=True):
        if serial:
            p.add_argument("--port", help="serial port of the cable (e.g. /dev/cu.usbmodemXXXX or /dev/dccable)")
            p.add_argument("--start-baud", type=int, default=115200, help="rate dctest starts at on the Dreamcast")
            p.add_argument("--rtscts", action="store_true", help="hardware flow control on the host port")
        p.add_argument("--log", help="log file (default dctest-<time>.log)")
        p.add_argument("--fw-log-cmd", help="command whose output joins the log, e.g. 'journalctl -fu dcsd -n0' "
                                            "or \"ssh pi@dreampi 'journalctl -fu dcsd -n0'\"")
        p.add_argument("--seed", type=int, default=int(time.time()) & 0xFFFFFF, help="random seed (printed; reuse to repeat a run)")
        p.add_argument("--step-seconds", type=float, default=6.0, help="time budget per baud / load step")

    sub.add_parser("ports")
    for name, fn in (("hello", None), ("serial", cmd_serial), ("ramp", cmd_ramp), ("hop", cmd_hop),
                     ("net", cmd_net), ("switch", cmd_switch), ("soak", cmd_soak)):
        p = sub.add_parser(name)
        common(p)
        if name == "serial":
            p.add_argument("--bauds", default="common", help="common | uncommon | all | comma list")
            p.add_argument("--full", action="store_true", help="all patterns, more sizes, longer idle gaps")
        if name == "ramp":
            p.add_argument("--baud", type=int, default=115200)
            p.add_argument("--length", type=int, default=512)
            p.add_argument("--modes", default="echo,sink,src")
        if name in ("hop", "switch"):
            p.add_argument("--bauds", default="115200,1500000,57600,921600")
            p.add_argument("--cycles", type=int, default=20)
        if name == "soak":
            p.add_argument("--minutes", type=float, default=30)
            p.add_argument("--bauds", default="common")
        if name in ("net", "switch"):
            p.add_argument("--dc-ip", help="DC address (default: learn it from the DC's beacon)")
            p.add_argument("--udp-port", type=int, default=7777)
            p.add_argument("--tcp-port", type=int, default=7778)
            p.add_argument("--quick", action="store_true")
    p = sub.add_parser("selftest")
    common(p, serial=False)
    p.add_argument("--tmp")
    args = ap.parse_args()

    if args.cmd == "ports":
        from serial.tools import list_ports
        for p in list_ports.comports():
            print(p.device, p.description)
        return 0
    log = Log(args.log or time.strftime("dctest-%Y%m%d-%H%M%S.log"))
    log.line("dctest.py %s  seed=%d  args=%s" % (args.cmd, args.seed, " ".join(sys.argv[1:])))
    fw = log.fw_watch(args.fw_log_cmd) if getattr(args, "fw_log_cmd", None) else None
    try:
        if args.cmd == "selftest":
            ok = selftest(args, log)
        else:
            if not args.port and not (args.cmd == "net" and args.dc_ip):
                ap.error("--port is required")
            chan = SerialChan(args.port, args.start_baud, args.rtscts) if args.port else None
            c = Ctx(Link(chan) if chan else None, log, args, chan)
            c.baud = args.start_baud
            if chan:
                h = hello(c.link, tries=20, timeout=0.5)
                if not h:
                    log.line("No answer from dctest on %s at %d baud. Start dctest.elf on the Dreamcast (and quit dc-tool)." % (args.port, args.start_baud))
                    return 2
                log.line("DC: " + h)
            if args.cmd != "hello":
                {"serial": cmd_serial, "ramp": cmd_ramp, "hop": cmd_hop, "net": cmd_net,
                 "switch": cmd_switch, "soak": cmd_soak}[args.cmd](c)
            ok = log.summary() if args.cmd != "hello" else True
    except KeyboardInterrupt:
        log.line("interrupted")
        log.summary()
        ok = False
    except RuntimeError as e:
        log.line("STOPPED: %s" % e)
        log.summary()
        ok = False
    finally:
        if fw:
            fw.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
