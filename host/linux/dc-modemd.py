#!/usr/bin/env python3
"""
dc-modemd: a Dreamcast dial-up server for any Linux computer (stage 1 of "do what the ESP32 does").

A USB voice modem (Conexant-based, as for DreamPi) sits on the Dreamcast's phone cord. This
daemon does what esp32/main/usbmodem.c does:
  1. reset the modem, voice mode (AT+FCLASS=8, AT+VLS=1, AT+VSM=1,8000, AT+VTX), play a dial tone
  2. read the keys the Dreamcast dials (DLE + key); the number is over after 2 s without a key
  3. stop the tone, hang up, reset, wait 8 s (as DreamPi does), ATA, wait for CONNECT
  4. hand the line to pppd (192.168.98.1 <-> 192.168.98.2, NAT to the Internet) until NO CARRIER

Only the standard library and pppd are needed. Stage 1 covers PPP calls (the normal Dreamcast
Internet call, *70 call-waiting prefix, Capcom). Netlink, XBAND, DCNet (*69) and game-server numbers
are recognised and logged but not carried out yet (see host/linux/README.md).
Status lines are plain English, read in `journalctl -u dc-modemd`.
"""
import argparse, base64, glob, hashlib, pty, math, os, re, select, signal, socket, struct, subprocess, sys, termios, threading, time
import urllib.request

DLE, ETX = 0x10, 0x03
CAPCOM = "0642542154"
DCNET_HOST, DCNET_PORT, DCNET_PORT_ALT, DCNET_DISCO = "dcnet.flyca.st", 7654, 7656, 7655
DCNET_MAGIC = 0xDC15C001
DNS_FILE_URL = "https://dreamcast.online/dreampi/dreampi_dns.conf"
XBAND = ("18002071194", "19209492263", "0120717360", "0355703001")
DEFAULT_DNS = ("46.101.91.123", "209.50.50.129")   # the revival DNS servers DreamPi's dnsmasq forwards to


def say(msg):
    print(msg, flush=True)


# ---- the parsers (same rules as firmware/usbmodem_model.h) ----

class Rx:
    """One byte at a time from the modem: result lines, dialled keys, NO CARRIER in the data."""
    def __init__(self):
        self.line = b""
        self.last = ""
        self.dle = False
        self.nc = 0

    def line_byte(self, b):
        if b not in (13, 10):
            if len(self.line) < 63:
                self.line += bytes([b])
            return None
        if not self.line:
            return None
        l = self.line.decode("latin-1")
        self.line = b""
        self.last = l
        if l == "OK": return "OK"
        if l.startswith("CONNECT"): return "CONNECT"
        if l == "ERROR": return "ERROR"
        if l in ("NO CARRIER", "NO ANSWER", "NO DIALTONE", "NO DIAL TONE"): return "NO CARRIER"
        if l == "BUSY": return "BUSY"
        if l == "RING": return "RING"
        return None

    def voice_byte(self, b):
        if not self.dle:
            self.dle = b == DLE
            return None
        self.dle = False
        c = chr(b)
        if c in "0123456789*#ABCD":
            return c
        return None                                  # DLE DLE (data), busy, silence ...

    def data_byte(self, b):
        pat = b"\r\nNO CARRIER\r\n"
        if b == pat[self.nc]:
            self.nc += 1
            if self.nc == len(pat):
                self.nc = 0
                return True
        else:
            self.nc = 1 if b == 13 else 0
        return False


def classify(number):
    """What a dialled number asks for: ppp, capcom, idle, wait, dcnet_prefix, or an unsupported kind."""
    n = len(number)
    if not n or number == "*70": return "idle"
    if number in ("0", "00"): return "wait"
    if number == "*69": return "dcnet_prefix"
    if number in XBAND: return "xband"
    if number == CAPCOM: return "capcom"
    if n >= 2 and number[0] == "#" and number[-1] == "#": return "netlink"
    return "ppp"


def tone_block():
    """100 ms (800 samples) of 350 + 440 Hz, 8-bit unsigned; no sample is DLE."""
    out = bytearray()
    for i in range(800):
        v = 128 + int(40 * math.sin(2 * math.pi * 350 * i / 8000) + 40 * math.sin(2 * math.pi * 440 * i / 8000))
        out.append(DLE + 1 if v == DLE else v)
    return bytes(out)


# ---- the modem ----

class Gone(Exception):
    pass


class Modem:
    def __init__(self, path):
        self.path = path
        self.open()

    def open(self):
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            a = termios.tcgetattr(self.fd)
            a[0] = 0; a[1] = 0; a[3] = 0                       # raw
            a[2] = (a[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)) | termios.CS8 | termios.CREAD | termios.CLOCAL
            a[4] = a[5] = termios.B115200
            a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        except termios.error:
            pass                                               # a pty or CDC port without line settings
        self.rx = Rx()

    def close(self):
        try: os.close(self.fd)
        except OSError: pass
        self.fd = -1

    def reopen(self):
        """pppd had the port: take it back (the modem may take a moment to show up again)."""
        for _ in range(50):
            try:
                self.open()
                return
            except OSError:
                time.sleep(0.1)
        raise Gone()

    def read(self, wait):
        try:
            r, _, _ = select.select([self.fd], [], [], wait)
            if not r:
                return b""
            d = os.read(self.fd, 4096)
        except (OSError, ValueError):
            raise Gone()
        if d == b"":
            raise Gone()
        return d

    def write(self, data):
        try:
            while data:
                try:
                    n = os.write(self.fd, data)
                    data = data[n:]
                except BlockingIOError:
                    select.select([], [self.fd], [], 0.2)
        except OSError:
            raise Gone()

    def cmd(self, text, want="OK", ms=2000):
        """Send one AT command, wait for its result (OK, CONNECT ...)."""
        self.rx = Rx()
        self.write(text.encode() + b"\r")
        end = time.monotonic() + ms / 1000
        while time.monotonic() < end:
            for b in self.read(0.05):
                ev = self.rx.line_byte(b)
                if ev == want:
                    return True
                if ev in ("ERROR", "NO CARRIER", "BUSY"):
                    return False
        return False

    def reset(self):
        if not self.cmd("ATZ0", "OK", 3000):
            return False
        time.sleep(1.0)
        self.cmd("AT&F0", "OK", 2000)
        if not self.cmd("ATE0W2", "OK", 2000):
            self.cmd("ATE0", "OK", 2000)
        return True

    def hang_up(self):
        self.rx = Rx()
        time.sleep(1.1)
        self.write(b"+++")
        time.sleep(1.1)
        self.cmd("ATH0", "OK", 3000)

    def end_tone(self):
        self.rx = Rx()
        self.write(b"\0\x10\x03\r\n")
        self.hang_up()


# ---- finding the modem ----

def usb_ids(tty):
    """(vendor, product) of the USB device a tty belongs to, from sysfs, or None."""
    p = os.path.realpath("/sys/class/tty/%s/device" % os.path.basename(tty))
    while p and p != "/" and p != "/sys":
        v, d = os.path.join(p, "idVendor"), os.path.join(p, "idProduct")
        if os.path.exists(v) and os.path.exists(d):
            try:
                return open(v).read().strip(), open(d).read().strip()
            except OSError:
                return None
        p = os.path.dirname(p)
    return None


def find_modem(spec):
    if spec != "auto":
        return spec if os.path.exists(spec) else None
    for tty in sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")):
        ids = usb_ids(tty)
        if ids == ("cafe", "4dc5"):                            # our own cable: not a modem
            continue
        return tty
    return None


# ---- DNS: replaces DreamPi's dnsmasq (rules as firmware/dns_model.h) ----

class DnsConf:
    """Upstream servers plus the lines of dreampi_dns.conf in dnsmasq syntax (address=/name/ip, server=/name/ip, server=ip)."""
    def __init__(self, upstream):
        self.up = list(upstream)
        self.ovr = {}      # name -> ip
        self.dom = {}      # name -> upstream ip

    def add_text(self, text):
        took = 0
        for l in text.splitlines():
            l = l.strip()
            m = re.match(r"^(address|server)=(?:/([^/]+)/)?([0-9.]+)(?:#\d+)?$", l)
            if not m or l.startswith("#"):
                continue
            kind, name, ip = m.groups()
            if name: name = name.lower()
            if kind == "address" and name: self.ovr[name] = ip
            elif kind == "server" and name: self.dom[name] = ip
            elif kind == "server": self.up.append(ip)
            else: continue
            took += 1
        return took

    @staticmethod
    def _match(table, name):
        best = None
        for d in table:
            if (name == d or name.endswith("." + d)) and (best is None or len(d) > len(best)):
                best = d
        return table[best] if best else None

    def override(self, name): return self._match(self.ovr, name)
    def upstream(self, name):
        ip = self._match(self.dom, name)
        return [ip] if ip else self.up


def dns_parse(p):
    """(id, flags, name, qtype, question_end) of a standard one-question query, else None."""
    if len(p) < 17: return None
    qid, flags, qd = struct.unpack(">HHH", p[:6])
    if flags & 0x8000 or (flags >> 11) & 15 or qd != 1: return None
    i, labels = 12, []
    while True:
        if i >= len(p): return None
        l = p[i]; i += 1
        if l == 0: break
        if l & 0xC0 or i + l > len(p): return None
        labels.append(p[i:i + l].decode("latin-1").lower()); i += l
    if i + 4 > len(p): return None
    return qid, flags, ".".join(labels), struct.unpack(">H", p[i:i + 2])[0], i + 4


def dns_rcode(p, qend, rcode):
    return p[:2] + bytes([0x80 | (p[2] & 1), 0x80 | rcode]) + b"\0\1\0\0\0\0\0\0" + p[12:qend]


def dns_answer_a(p, qend, ip, ttl=60):
    r = dns_rcode(p, qend, 0)
    r = r[:6] + b"\0\1" + r[8:]
    return r + b"\xc0\x0c\0\1\0\1" + struct.pack(">I", ttl) + b"\0\4" + socket.inet_aton(ip)


def dns_local_only(name, qtype):
    """domain-needed and bogus-priv: plain names and reverse lookups of private ranges are never forwarded."""
    if qtype != 12 and "." not in name: return True
    if qtype == 12 and name.endswith(".in-addr.arpa"):
        parts = name[:-13].split(".")
        if len(parts) == 4 and all(x.isdigit() for x in parts):
            a, b = int(parts[3]), int(parts[2])
            if a in (10, 127, 0) or (a == 192 and b == 168) or (a == 172 and 16 <= b <= 31) or (a == 169 and b == 254):
                return True
    return False


class DnsServer(threading.Thread):
    def __init__(self, ip, port, conf_get, log=None):
        super().__init__(daemon=True)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.IPPROTO_IP, 15, 1)               # IP_FREEBIND: the ppp address does not exist yet
        self.s.bind((ip, port))
        self.s.settimeout(0.5)
        self.conf_get, self.log, self.stop_now = conf_get, log, False
        self.up_port = 53
        self.stats = {"queries": 0, "overridden": 0, "forwarded": 0, "refused": 0}
        self.port = self.s.getsockname()[1]

    def stop(self):
        self.stop_now = True
        self.join(2)
        self.s.close()

    def run(self):
        while not self.stop_now:
            try:
                p, who = self.s.recvfrom(1500)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                r = self.handle(p)
            except Exception as e:
                r = None
                if self.log: self.log("DNS: %s" % e)
            if r:
                try: self.s.sendto(r, who)
                except OSError: pass

    def handle(self, p):
        q = dns_parse(p)
        if not q: return None
        qid, flags, name, qtype, qend = q
        conf = self.conf_get()
        self.stats["queries"] += 1
        ip = conf.override(name) if qtype in (1, 28) else None
        if ip:
            self.stats["overridden"] += 1
            return dns_answer_a(p, qend, ip) if qtype == 1 else dns_rcode(p, qend, 0)
        if dns_local_only(name, qtype):
            self.stats["refused"] += 1
            return dns_rcode(p, qend, 3)
        for up in conf.upstream(name):
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.settimeout(2.0)
            try:
                u.sendto(p, (up, self.up_port))
                while True:
                    r, frm = u.recvfrom(4096)
                    if frm[0] == up and r[:2] == p[:2]:
                        self.stats["forwarded"] += 1
                        return r
            except OSError:
                continue
            finally:
                u.close()
        return dns_rcode(p, qend, 2)


class DnsFile(threading.Thread):
    """Fetches dreampi_dns.conf now and every 6 hours; no file on the server = ask the servers directly (as DreamPi does)."""
    def __init__(self, upstream, url, log):
        super().__init__(daemon=True)
        self.upstream, self.url, self.log = upstream, url, log
        self.conf = DnsConf(upstream)

    def get(self):
        return self.conf

    def run(self):
        while True:
            try:
                with urllib.request.urlopen(self.url, timeout=10) as r:
                    text = r.read(1 << 20).decode("latin-1")
                c = DnsConf(self.upstream)
                n = c.add_text(text)
                self.conf = c
                self.log("DNS: dreampi_dns.conf loaded (%d lines used)" % n)
            except Exception as e:
                self.log("DNS: no dreampi_dns.conf (%s); asking the DNS servers directly" % getattr(e, "reason", e))
                self.conf = DnsConf(self.upstream)
            time.sleep(6 * 3600)


# ---- DCNet (rules as firmware/dcnet_model.h, esp32/main/dcnet.c) ----

def dcnet_wanted(mode, dial, prefix_recent):
    pre = dial.startswith("*69")
    number = "".join(c for c in (dial[3:] if pre else dial) if c in "0123456789*#")
    if mode == "off": return False, number
    if mode == "always": return True, number
    return pre or prefix_recent, number


def dcnet_port_for(number):
    if re.fullmatch(r"0053600100(0[1-9]|10|13|14|15|16)", number):
        return DCNET_PORT_ALT
    return DCNET_PORT


def dcnet_fix_to_dc(state, data):
    """A '~' followed by anything but '~' becomes '~~' on the way to the Dreamcast; state[0] carries a last '~'."""
    out = bytearray()
    for c in data:
        if state[0]:
            state[0] = False
            if c != 0x7E: out.append(0x7E)
            out.append(c)
            continue
        if c == 0x7E: state[0] = True
        out.append(c)
    return bytes(out)


def dcnet_best_ap(log):
    """The closest access point (lowest of up to 3 pings each within 1 s): (ip, name, ms) or None."""
    try:
        disco = socket.gethostbyname(DCNET_HOST)
    except OSError:
        return None
    magic = struct.pack("<I", DCNET_MAGIC)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.1)
    ping = lambda: magic + b"\x01" + struct.pack("<Q", int(time.monotonic() * 1000))
    s.sendto(magic + b"\x03", (disco, DCNET_DISCO))
    aps, tot, got = [], {}, {}
    t0 = time.monotonic()
    while time.monotonic() - t0 < 1.0:
        try:
            b, frm = s.recvfrom(300)
        except socket.timeout:
            continue
        except OSError:
            break
        if len(b) < 5 or b[:4] != magic: continue
        if b[4] == 3 and not aps:
            p = 5
            while p + 5 <= len(b) and len(aps) < 8:
                ip = socket.inet_ntoa(b[p:p + 4]); l = b[p + 4]; p += 5
                if p + l > len(b): break
                aps.append((ip, b[p:p + l].decode("latin-1")[:31])); p += l
            if len(aps) == 1: break
            for ip, _ in aps:
                s.sendto(ping(), (ip, DCNET_DISCO))
        elif b[4] == 2 and len(b) == 13:
            sent = struct.unpack("<Q", b[5:])[0]
            ms = int(time.monotonic() * 1000) - sent
            tot[frm[0]] = tot.get(frm[0], 0) + ms; got[frm[0]] = got.get(frm[0], 0) + 1
            if got[frm[0]] >= 3: break
            s.sendto(ping(), frm)
    s.close()
    best = None
    for ip, name in aps:
        if got.get(ip) and (best is None or tot[ip] / got[ip] < best[2]):
            best = (ip, name, int(tot[ip] / got[ip]))
    if not best and len(aps) == 1:
        best = (aps[0][0], aps[0][1], None)
    return best


def dcnet_open(number, say_):
    """A TCP connection to the closest DCNet access point for this number, or None."""
    ap = dcnet_best_ap(say_)
    if ap:
        say_("DCNet: access point %s%s" % (ap[1], " (%d ms)" % ap[2] if ap[2] is not None else ""))
        host = ap[0]
    else:
        host = DCNET_HOST
    port = dcnet_port_for(number)
    try:
        sock = socket.create_connection((host, port), 10)
    except OSError as e:
        say_("DCNet: could not connect to the access point (%s)" % e)
        return None
    say_("DCNet: connected to %s port %d" % (host, port))
    return sock


def dcnet_relay(modem, number, say_):
    """The Dreamcast's call (after ATA/CONNECT) passed unchanged to a DCNet access point over TCP."""
    sock = dcnet_open(number, say_)
    if not sock:
        return False
    rx, tilde = Rx(), [False]
    try:
        while True:
            r, _, _ = select.select([modem.fd, sock], [], [], 0.5)
            if modem.fd in r:
                try:
                    d = os.read(modem.fd, 4096)
                except OSError:
                    raise Gone()
                if not d: raise Gone()
                sock.sendall(d)
                if any(rx.data_byte(b) for b in d):
                    say_("DCNet: the line dropped (NO CARRIER)")
                    return True
            if sock in r:
                d = sock.recv(4096)
                if not d:
                    say_("DCNet: the access point ended the call")
                    return True
                modem.write(dcnet_fix_to_dc(tilde, d))
    except OSError as e:
        say_("DCNet: connection error (%s)" % e)
        return True
    finally:
        sock.close()


# ---- PPP and NAT ----

class Nat:
    """MASQUERADE for the Dreamcast, in our own iptables chain (never touching anyone else's rules)."""
    CHAIN = "DCMODEM"

    def __init__(self, dc_ip, enabled):
        self.dc, self.on = dc_ip, enabled and bool(self._ipt())
        self.up = False

    @staticmethod
    def _ipt():
        for d in os.environ.get("PATH", "").split(":") + ["/usr/sbin", "/sbin"]:
            if os.access(os.path.join(d, "iptables"), os.X_OK):
                return os.path.join(d, "iptables")
        return None

    def _run(self, *a):
        return subprocess.call([self._ipt(), "-t", "nat"] + list(a), stderr=subprocess.DEVNULL) == 0

    def start(self):
        if not self.on:
            return
        self._run("-N", self.CHAIN)
        self._run("-F", self.CHAIN)
        self._run("-A", self.CHAIN, "-s", self.dc, "-j", "MASQUERADE")
        if not self._run("-C", "POSTROUTING", "-j", self.CHAIN):
            self._run("-I", "POSTROUTING", "-j", self.CHAIN)
        try:
            open("/proc/sys/net/ipv4/ip_forward", "w").write("1")
        except OSError:
            pass
        self.up = True

    def stop(self):
        if not self.up:
            return
        self._run("-D", "POSTROUTING", "-j", self.CHAIN)
        self._run("-F", self.CHAIN)
        self._run("-X", self.CHAIN)
        self.up = False


def ppp_start(args, dev, why):
    """pppd on a tty (a modem's or a pty), with our DNS server in front. Returns (process, dns server or None)."""
    cmd = [args.pppd, dev, str(args.ppp_speed), "%s:%s" % (args.our_ip, args.dc_ip),
           "noauth", "nodetach", "noccp", "nodeflate", "nobsdcomp", "novj", "proxyarp",
           "local", "nocrtscts", "lock", "mtu", "1500", "mru", "1500", "lcp-echo-interval", "30",
           "lcp-echo-failure", "4"]
    dns = None
    if not args.no_dns_server:
        try:
            dns = DnsServer(args.our_ip, args.dns_port, args.dns_conf, say)
            dns.start()
        except OSError as e:
            say("DNS: cannot listen on %s:%d (%s); the Dreamcast gets the upstream servers directly" % (args.our_ip, args.dns_port, e))
            dns = None
    for d in ([args.our_ip] if dns else args.dns):
        cmd += ["ms-dns", d]
    say("PPP: %s starts for %s" % (os.path.basename(args.pppd), why))
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT), dns


def ppp_log(p, args, seen):
    """Log pppd's output until it ends; seen['up'] is set when the addresses are agreed."""
    for line in p.stdout:
        l = line.decode("latin-1").rstrip()
        if "local  IP address" in l or "remote IP address" in l:
            seen["up"] = True
        if args.verbose or "address" in l or "Exit" in l or "Connection terminated" in l:
            say("pppd: " + l)


def ppp_finish(p, dns, t0, seen):
    if dns:
        say("DNS: %(queries)d queries, %(overridden)d overridden, %(forwarded)d forwarded, %(refused)d refused" % dns.stats)
        dns.stop()
    if p.poll() is None:
        p.terminate()
        try: p.wait(5)
        except subprocess.TimeoutExpired: p.kill()
    say("PPP is over after %d s%s" % (time.monotonic() - t0, "" if seen.get("up") else " (it never came up)"))


def run_ppp(modem, args, why):
    """pppd on the modem's tty until it exits (the modem is in data mode)."""
    modem.close()                       # pppd takes the tty over; we reopen after
    p, dns = ppp_start(args, modem.path, why)
    t0, seen = time.monotonic(), {}
    try:
        ppp_log(p, args, seen)
        p.wait()
    finally:
        ppp_finish(p, dns, t0, seen)
        modem.reopen()
    return bool(seen.get("up"))

# ---- the browser page: Web Serial in Chrome talks to the modem, this relay does the PPP/DCNet part ----

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WEB_DIRS = (os.path.join(os.path.dirname(os.path.abspath(__file__)), "web"), "/usr/local/share/dc-modemd/web")
relay_lock = threading.BoundedSemaphore(1)   # replaced in relay_main


class Ws:
    """The least of RFC 6455 a browser needs: masked frames in, plain frames out."""
    def __init__(self, sock):
        self.s, self.buf, self.part = sock, b"", b""

    def send(self, op, data=b""):
        n = len(data)
        h = bytes([0x80 | op]) + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n) if n < 65536 else b"\x7f" + struct.pack(">Q", n))
        self.s.sendall(h + data)

    def text(self, t): self.send(1, t.encode())

    def feed(self):
        """Read what is there; the list of whole messages (op, payload); EOFError when the browser is gone."""
        d = self.s.recv(65536)
        if not d: raise EOFError()
        self.buf += d
        out = []
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            n, p = b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4: break
                n, p = struct.unpack(">H", self.buf[2:4])[0], 4
            elif n == 127:
                if len(self.buf) < 10: break
                n, p = struct.unpack(">Q", self.buf[2:10])[0], 10
            if n > (1 << 20): raise EOFError()
            key = b""
            if b1 & 0x80:
                if len(self.buf) < p + 4: break
                key, p = self.buf[p:p + 4], p + 4
            if len(self.buf) < p + n: break
            data, self.buf = self.buf[p:p + n], self.buf[p + n:]
            if key: data = bytes(x ^ key[i & 3] for i, x in enumerate(data))
            op = b0 & 15
            if op == 0:
                self.part += data
                if b0 & 0x80: out.append((self.pop_op, self.part)); self.part = b""
                continue
            if not b0 & 0x80:
                self.part, self.pop_op = data, op
                continue
            if op == 8: raise EOFError()
            if op == 9: self.send(10, data); continue
            if op == 10: continue
            out.append((op, data))
        return out


def read_http(c):
    c.settimeout(5)
    d = b""
    while b"\r\n\r\n" not in d and len(d) < 8192:
        x = c.recv(2048)
        if not x: return None
        d += x
    head = d.split(b"\r\n\r\n")[0].decode("latin-1").split("\r\n")
    hd = {}
    for l in head[1:]:
        if ":" in l:
            k, v = l.split(":", 1); hd[k.strip().lower()] = v.strip()
    return head[0].split(" ")[:2], hd


def relay_decide(args, text):
    """'dial <number> [prefix]' -> (reply, kind, number)."""
    f = text.split()
    if len(f) < 2 or f[0] != "dial":
        return "error bad request", None, ""
    number, prefix = f[1], len(f) > 2 and f[2] == "prefix"
    if number == "-":                       # the serial cable's Dreamcast: PPP without a number
        if args.dcnet_only: return "unsupported ppp (this relay only does DCNet)", None, ""
        return "ok ppp 0", "ppp", ""
    kind = classify(number)
    if kind == "capcom" and not args.dcnet_only: return "ok capcom 0", "capcom", number
    if kind == "ppp":
        via, num = dcnet_wanted("always" if args.dcnet_only else args.dcnet, number, prefix)
        if via: return "ok dcnet 0", "dcnet", num
        return "ok ppp %g" % args.answer_delay, "ppp", number
    return "unsupported %s" % kind, None, number


def relay_session(ws, args, first):
    reply, kind, number = relay_decide(args, first)
    ws.text(reply)
    if not kind:
        say("browser: %s dialled, %s" % (number, reply))
        return
    say("browser: the Dreamcast dialled %s (%s)" % (number, kind))
    if kind == "dcnet":
        sock = dcnet_open(number, say)
        if not sock:
            ws.text("over")
            return
        tilde = [False]
        try:
            while True:
                r, _, _ = select.select([ws.s, sock], [], [], 1)
                if ws.s in r:
                    for op, data in ws.feed():
                        if op == 2: sock.sendall(data)
                if sock in r:
                    d = sock.recv(4096)
                    if not d:
                        say("DCNet: the access point ended the call")
                        return
                    ws.send(2, dcnet_fix_to_dc(tilde, d))
        except (OSError, EOFError):
            return
        finally:
            sock.close()
    master, slave = pty.openpty()
    nat = Nat(args.dc_ip, not args.no_nat)
    nat.start()
    p, dns = ppp_start(args, os.ttyname(slave), "the browser")
    t0, seen = time.monotonic(), {}
    threading.Thread(target=ppp_log, args=(p, args, seen), daemon=True).start()
    try:
        while p.poll() is None:
            r, _, _ = select.select([ws.s, master], [], [], 0.5)
            if master in r:
                try: d = os.read(master, 4096)
                except OSError: break
                ws.send(2, d)
            if ws.s in r:
                for op, data in ws.feed():
                    if op == 2: os.write(master, data)
    except (OSError, EOFError):
        pass
    finally:
        ppp_finish(p, dns, t0, seen)
        nat.stop()
        os.close(master); os.close(slave)
        try: ws.text("over")
        except OSError: pass


def relay_client(c, args):
    try:
        req = read_http(c)
        if not req: return
        (method, path), hd = req
        if hd.get("upgrade", "").lower() != "websocket" or path != "/ws":
            name = "index.html" if path in ("/", "/index.html") else None
            for d in WEB_DIRS:
                if name and os.path.exists(os.path.join(d, name)):
                    body = open(os.path.join(d, name), "rb").read()
                    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-store\r\n\r\n" % len(body) + body)
                    return
            c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            return
        # only this program's own page may drive pppd: another site's script must not
        org, host = hd.get("origin", ""), hd.get("host", "")
        if org and re.sub(r"^https?://", "", org) != host and org not in args.relay_origin:
            c.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            say("browser: refused a page from %s" % org)
            return
        acc = base64.b64encode(hashlib.sha1((hd.get("sec-websocket-key", "") + WS_GUID).encode()).digest()).decode()
        c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        c.settimeout(None)
        ws = Ws(c)
        if not relay_lock.acquire(False):
            ws.text("busy")
            return
        try:
            say("browser: connected")
            first = None
            t0 = time.monotonic()
            while first is None and time.monotonic() - t0 < 120:
                r, _, _ = select.select([c], [], [], 1)
                if r:
                    for op, data in ws.feed():
                        if op == 1: first = data.decode("latin-1")
            if first: relay_session(ws, args, first)
            say("browser: call over")
        finally:
            relay_lock.release()
    except (OSError, EOFError):
        pass
    finally:
        try: c.close()
        except OSError: pass


def relay_main(args):
    global relay_lock
    relay_lock = threading.BoundedSemaphore(args.max_sessions or (20 if args.dcnet_only else 1))
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.relay_host, args.relay_port))
    s.listen(4)
    say("web page: http://%s:%d/ (open it in Chrome or Edge; the modem plugs into that computer)" % (args.relay_host, args.relay_port))
    while True:
        c, _ = s.accept()
        threading.Thread(target=relay_client, args=(c, args), daemon=True).start()



# ---- one call ----

def one_call(modem, args, state):
    def st(s):
        if state.get("s") != s:
            say(s)
        state["s"] = s

    st("setting up the modem")
    if not modem.reset():
        modem.write(b"\0\x10\x03\r\n")
        modem.hang_up()
        if not modem.reset():
            st("the modem does not answer AT commands")
            time.sleep(5)
            return
    if not (modem.cmd("AT+FCLASS=8") and modem.cmd("AT+VLS=1") and modem.cmd("AT+VSM=1,8000") and modem.cmd("AT+VTX", "CONNECT", 3000)):
        st("this modem has no voice mode (needed to hear the Dreamcast dial); a Conexant-based modem as for DreamPi works")
        modem.cmd("ATH0")
        time.sleep(10)
        return
    st("dial tone on, waiting for the Dreamcast to dial")
    modem.rx = Rx()
    tone = tone_block()
    dial, last_key, due = "", 0.0, time.monotonic()
    prefix_t = -1e9
    while True:
        now = time.monotonic()
        if now >= due:
            modem.write(tone)
            due += 0.1
            if now - due > 0.5:
                due = now
        if dial and now - last_key > args.key_timeout:
            kind = classify(dial)
            if kind == "dcnet_prefix":
                prefix_t = now; say("*69 heard (DCNet prefix)"); dial = ""; continue
            if kind == "idle":
                dial = ""; continue
            break
        for b in modem.read(0.01):
            k = modem.rx.voice_byte(b)
            if k and len(dial) < 23:
                dial += k; last_key = time.monotonic()
    heard = time.monotonic()
    kind = classify(dial)
    state["calls"] = state.get("calls", 0) + 1
    if kind not in ("ppp", "capcom"):
        say("%s dialled: %s, which is not carried out by this host program yet; hanging up" % (dial, kind))
        modem.end_tone(); modem.reset()
        return
    via_dcnet, number = False, ""
    if kind == "ppp":
        via_dcnet, number = dcnet_wanted(args.dcnet, dial, time.monotonic() - prefix_t < 10)
    say("the Dreamcast dialled %s; answering%s" % (dial, " (DCNet)" if via_dcnet else " (Capcom matching: PPP)" if kind == "capcom" else " in %g s" % args.answer_delay))
    st("the Dreamcast is dialling")
    modem.end_tone()
    modem.reset()
    if kind != "capcom" and not via_dcnet:
        while time.monotonic() - heard < args.answer_delay:
            time.sleep(0.05)
    st("answering")
    if not modem.cmd("ATA", "CONNECT", 60000):
        say("the two modems did not connect (no CONNECT after ATA)")
        modem.hang_up()
        return
    if via_dcnet:
        say("answered, %s; passing the call to DCNet" % modem.rx.last)
        dcnet_relay(modem, number, say)
        modem.hang_up()
        return
    say("answered, %s; starting PPP" % modem.rx.last)
    nat = Nat(args.dc_ip, not args.no_nat)
    nat.start()
    try:
        run_ppp(modem, args, dial)
    finally:
        nat.stop()
    st("hanging up")


# ---- the serial cable: the Dreamcast talks AT to us as to a modem (firmware/hayes_model.h) ----

class Hayes:
    def __init__(self):
        self.line = b""
        self.echo = self.verbose = True
        self.quiet = False
        self.baud = 33600

    def _res(self, code):
        if self.quiet: return b""
        if self.verbose:
            w = ["OK", "CONNECT %d" % self.baud, "RING", "NO CARRIER", "ERROR"][code]
            return b"\r\n" + w.encode() + b"\r\n"
        return b"%d\r" % code

    def byte(self, c):
        """(reply bytes, event): event is 'dial' after ATD/ATA (CONNECT sent), 'ppp' for a bare PPP flag."""
        if c == 0x7E and not self.line:
            return b"", "ppp"
        out = b""
        if self.echo and c >= 8 and c not in (0x7E, 0x7F, 10):
            out += bytes([c])
        ev = None
        if c == 13:
            l = self.line.decode("latin-1").upper()
            if l.startswith("AT"):
                r, ev = self._run(l)
                out += r
            self.line = b""
        elif c in (8, 0x7F):
            self.line = self.line[:-1]
        elif 0x20 <= c < 0x7F and len(self.line) < 96:
            self.line += bytes([c])
        return out, ev

    def _run(self, l):
        i = 2
        while i < len(l):
            c = l[i]; i += 1
            if c == " ": continue
            if c in "DA":
                return self._res(1), "dial"
            if c in "+%\\": break
            if c == "&": i += 1
            j = i
            while i < len(l) and l[i].isdigit(): i += 1
            val = int(l[j:i]) if i > j else -1
            if c == "S":
                if i < len(l) and l[i] in "=?":
                    i += 1
                    while i < len(l) and l[i].isdigit(): i += 1
                continue
            if c == "E": self.echo = val != 0
            elif c == "V": self.verbose = val != 0
            elif c == "Q": self.quiet = val > 0
            elif c == "Z": self.echo, self.verbose, self.quiet = True, True, False
        return self._res(0), None


def cable_loop(args):
    """The RP2040 cable's serial port: Hayes until ATD/ATA or a PPP flag, then pppd."""
    state = {}
    while True:
        if not os.path.exists(args.cable):
            if state.get("s") != "none":
                say("waiting for the cable (%s)" % args.cable)
            state["s"] = "none"; time.sleep(3); continue
        try:
            modem = Modem(args.cable)
        except OSError as e:
            say("cannot open %s: %s" % (args.cable, e)); time.sleep(5); continue
        say("serial cable on %s: waiting for the Dreamcast's AT commands" % args.cable)
        h = Hayes()
        try:
            while True:
                ev, first = None, b""
                for b in modem.read(0.5):
                    out, e = h.byte(b)
                    if out: modem.write(out)
                    if e: ev = e; first = bytes([b]) if e == "ppp" else b""
                if ev:
                    say("Dreamcast on the serial cable: %s" % ("PPP without a dial" if ev == "ppp" else "dialled, PPP starts"))
                    nat = Nat(args.dc_ip, not args.no_nat); nat.start()
                    try:
                        run_ppp(modem, args, "the serial cable")
                    finally:
                        nat.stop()
                    h = Hayes()
                    if args.once: return 0
        except Gone:
            say("the cable was unplugged")
        finally:
            modem.close()
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--device", default="auto", help="the modem's tty (default: the first USB serial port that is not our cable)")
    ap.add_argument("--cable", help="serve the RP2040 cable's serial port (e.g. /dev/dccable) instead of a USB modem: Hayes stub, then PPP")
    ap.add_argument("--relay", action="store_true", help="serve the web page and the PPP/DCNet relay for it (the modem is then used by Chrome's Web Serial)")
    ap.add_argument("--relay-host", default="127.0.0.1", help="address to listen on (default this computer only)")
    ap.add_argument("--relay-port", type=int, default=8099)
    ap.add_argument("--relay-origin", action="append", default=[], help="another page address allowed to use the relay (repeat)")
    ap.add_argument("--dcnet-only", action="store_true", help="relay: carry DCNet calls only (no pppd, NAT or DNS), for a public bridge: the only place it connects to is DCNet's own servers")
    ap.add_argument("--max-sessions", type=int, default=0, help="relay: calls at once (default 1; 20 with --dcnet-only)")
    ap.add_argument("--pppd", default="pppd")
    ap.add_argument("--our-ip", default="192.168.98.1")
    ap.add_argument("--dc-ip", default="192.168.98.2")
    ap.add_argument("--dns", action="append", help="upstream DNS server (repeat; default the revival servers)")
    ap.add_argument("--dcnet", choices=("off", "prefix", "always"), default="prefix", help="DCNet: only after *69 (default), every call, or never")
    ap.add_argument("--no-dns-server", action="store_true", help="no DNS server of our own: give the Dreamcast the --dns servers")
    ap.add_argument("--dns-port", type=int, default=53)
    ap.add_argument("--dns-file", default=DNS_FILE_URL, help="dreampi_dns.conf URL ('' = none)")
    ap.add_argument("--ppp-speed", type=int, default=115200)
    ap.add_argument("--key-timeout", type=float, default=2.0, help="seconds without a key that end the number")
    ap.add_argument("--answer-delay", type=float, default=8.0, help="seconds after the number before ATA")
    ap.add_argument("--no-nat", action="store_true", help="do not add the MASQUERADE rule")
    ap.add_argument("--once", action="store_true", help="stop after one call (for tests)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    args.dns = args.dns or list(DEFAULT_DNS)
    if args.dns_file and not args.no_dns_server:
        f = DnsFile(args.dns, args.dns_file, say)
        f.start()
        args.dns_conf = f.get
    else:
        c = DnsConf(args.dns)
        args.dns_conf = lambda: c
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    if args.relay:
        relay_main(args)
    if args.cable:
        return cable_loop(args)
    state = {}
    while True:
        path = find_modem(args.device)
        if not path:
            if state.get("s") != "no modem":
                say("no USB modem found; waiting (plug one in)")
            state["s"] = "no modem"
            time.sleep(3)
            continue
        try:
            modem = Modem(path)
        except OSError as e:
            say("cannot open %s: %s" % (path, e)); time.sleep(5); continue
        say("using the modem on %s" % path)
        try:
            while True:
                one_call(modem, args, state)
                if args.once:
                    return 0
        except Gone:
            say("the modem was unplugged")
        finally:
            modem.close()
        state["s"] = ""
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
