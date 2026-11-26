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
                    "--once", "--key-timeout", "0.5", "--answer-delay", "0.3", "--dns", "9.9.9.9"],
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
    check("pppd gets the addresses and DNS", "192.168.98.1:192.168.98.2" in a and "ms-dns 9.9.9.9" in a and "noauth" in a)
check("PPP over is reported", "PPP is over" in out)

sys.exit(1 if fails else 0)
