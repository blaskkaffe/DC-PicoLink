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
import argparse, glob, math, os, re, select, signal, subprocess, sys, termios, time

DLE, ETX = 0x10, 0x03
CAPCOM = "0642542154"
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


def run_ppp(modem, args, why):
    """pppd on the modem's tty until it exits or NO CARRIER shows (the modem is in data mode)."""
    cmd = [args.pppd, modem.path, str(args.ppp_speed), "%s:%s" % (args.our_ip, args.dc_ip),
           "noauth", "nodetach", "noccp", "nodeflate", "nobsdcomp", "novj", "proxyarp",
           "local", "nocrtscts", "lock", "mtu", "1500", "mru", "1500", "lcp-echo-interval", "30",
           "lcp-echo-failure", "4"]
    for d in args.dns:
        cmd += ["ms-dns", d]
    say("PPP: %s starts for %s" % (os.path.basename(args.pppd), why))
    # pppd takes the tty over: close our descriptor's reading (we reopen after), keep the device
    modem.close()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    t0 = time.monotonic()
    up = False
    try:
        for line in p.stdout:
            l = line.decode("latin-1").rstrip()
            if "local  IP address" in l or "remote IP address" in l:
                up = True
            if args.verbose or "address" in l or "Exit" in l or "Connection terminated" in l:
                say("pppd: " + l)
        p.wait()
    finally:
        if p.poll() is None:
            p.terminate()
            try: p.wait(5)
            except subprocess.TimeoutExpired: p.kill()
    modem.reopen()
    say("PPP is over after %d s%s" % (time.monotonic() - t0, "" if up else " (it never came up)"))
    return up


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
                prefix_t = now; say("*69 heard (DCNet prefix; DCNet is not in this stage)"); dial = ""; continue
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
    if time.monotonic() - prefix_t < 10:
        say("the Dreamcast dialled %s after *69 (DCNet), not carried out yet; hanging up" % dial)
        modem.end_tone(); modem.reset()
        return
    say("the Dreamcast dialled %s; answering%s" % (dial, " (Capcom matching: PPP)" if kind == "capcom" else " in %g s" % args.answer_delay))
    st("the Dreamcast is dialling")
    modem.end_tone()
    modem.reset()
    if kind != "capcom":
        while time.monotonic() - heard < args.answer_delay:
            time.sleep(0.05)
    st("answering")
    if not modem.cmd("ATA", "CONNECT", 60000):
        say("the two modems did not connect (no CONNECT after ATA)")
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
    ap.add_argument("--pppd", default="pppd")
    ap.add_argument("--our-ip", default="192.168.98.1")
    ap.add_argument("--dc-ip", default="192.168.98.2")
    ap.add_argument("--dns", action="append", help="DNS server for the Dreamcast (repeat; default the revival servers)")
    ap.add_argument("--ppp-speed", type=int, default=115200)
    ap.add_argument("--key-timeout", type=float, default=2.0, help="seconds without a key that end the number")
    ap.add_argument("--answer-delay", type=float, default=8.0, help="seconds after the number before ATA")
    ap.add_argument("--no-nat", action="store_true", help="do not add the MASQUERADE rule")
    ap.add_argument("--once", action="store_true", help="stop after one call (for tests)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    args.dns = args.dns or list(DEFAULT_DNS)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
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
