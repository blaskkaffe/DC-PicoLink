#!/usr/bin/env python3
"""dcsd - serve a disk image to the RP2040 Dreamcast cable as an emulated SD card.

The RP2040 exposes a vendor-specific USB interface ("DC SD Card Link"). It is
deliberately not a serial port, so modem probes (DreamPi's wvdialconf) can't open
it. dcsd talks to it directly through Linux usbfs. The Dreamcast sees an SDHC card
in the SD adapter on its serial port; every block it reads or writes comes from /
goes to the image file here.

Protocol (little-endian):
  device -> Pi : 'H'                       hello
                 'R' lba:u32 count:u16     read blocks
                 'W' lba:u32 data[512]     write block
                 'S' len:u8 text           status line (logged)
                 'L' len:u16 data          logic capture chunk; len 0 ends it (+ rate_khz:u32)
  Pi -> device : 'I' blocks:u32 flags:u8   image info (bit0 read-only)
                 'M' mode:u8               force a bus mode (see MODES; set with dcsd-mode)
                 'C' 0:u8                  arm a logic capture (dcsd-capture)
                 'D' lba:u32 data[512] crc:u16be   (CRC16-XMODEM, what SD sends)
                 'A' lba:u32 status:u8     write done (0 ok)

Only Python 3 standard library. Run as a service (see dcsd.service).
"""
import argparse
import binascii
import ctypes
import fcntl
import glob
import errno
import os
import select
import struct
import subprocess
import sys
import termios
import threading
import queue
import time
import tty

BLOCK = 512
PART_START = 2048          # first partition at 1 MiB, like a real card


MODES = {"auto": 0, "serial": 1, "spi": 2, "w5500": 3, "sd": 4}


def read_mode(path):
    try:
        with open(path) as f:
            return MODES.get(f.read().strip().lower(), 0)
    except OSError:
        return 0


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def parse_size(s):
    s = s.strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if s[-1:] in mult:
        return int(float(s[:-1]) * mult[s[-1]])
    return int(s)


def create_image(path, size):
    """Sparse image with an MBR and one FAT32 partition (what KOS/DreamShell expect)."""
    blocks = (size // BLOCK) & ~1023            # whole 512 KiB units (SD CSD granularity)
    if blocks < 128 * 1024:
        sys.exit("image size must be at least 64M")
    with open(path, "wb") as f:
        f.truncate(blocks * BLOCK)
        mbr = bytearray(BLOCK)
        part_len = blocks - PART_START
        # status, CHS first (dummy), type 0x0C = FAT32 LBA, CHS last (dummy), LBA start, length
        mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff",
                                   PART_START, part_len)
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
    mkfs = None
    for cand in ("mkfs.fat", "mkfs.vfat", "/sbin/mkfs.fat", "/usr/sbin/mkfs.fat"):
        if subprocess.call(["sh", "-c", "command -v %s >/dev/null" % cand]) == 0:
            mkfs = cand
            break
    if not mkfs:
        os.unlink(path)
        sys.exit("mkfs.fat not found: sudo apt install dosfstools")
    kib = (blocks - PART_START) * BLOCK // 1024
    subprocess.check_call([mkfs, "-F", "32", "-n", "DREAMCAST", "-h", str(PART_START),
                           "--offset", str(PART_START), path, str(kib)])
    log("created %s: %d MiB, FAT32 partition at sector %d" % (path, blocks * BLOCK >> 20, PART_START))


def image_mounted(path):
    """True if a loop device on this machine is backed by the image (it's mounted/edited)."""
    real = os.path.realpath(path)
    try:
        for d in os.listdir("/sys/block"):
            bf = "/sys/block/%s/loop/backing_file" % d
            if os.path.exists(bf):
                with open(bf) as f:
                    if os.path.realpath(f.read().strip()) == real:
                        return True
    except OSError:
        pass
    return False


# ---- logic capture decoding (dcsd-capture) ---------------------------------
CLK, MOSI, MISO, CS = 1, 2, 4, 8          # bits of each 4-bit sample (GPIO2..5)


def capture_samples(raw):
    out = bytearray(len(raw) * 2)
    for i, b in enumerate(raw):
        out[2 * i] = b & 15
        out[2 * i + 1] = b >> 4
    return out


def write_vcd(path, smp, khz):
    ns = 1e6 / khz
    names = (("clk", CLK), ("mosi", MOSI), ("miso", MISO), ("cs", CS))
    with open(path, "w") as f:
        f.write("$timescale 1ns $end\n$scope module dc $end\n")
        for i, (n, _) in enumerate(names):
            f.write("$var wire 1 %s %s $end\n" % (chr(33 + i), n))
        f.write("$upscope $end\n$enddefinitions $end\n")
        prev = None
        for i, v in enumerate(smp):
            if v != prev:
                f.write("#%d\n" % int(i * ns))
                for j, (_, m) in enumerate(names):
                    if prev is None or (v ^ prev) & m:
                        f.write("%d%s\n" % (1 if v & m else 0, chr(33 + j)))
                prev = v


def decode_capture(raw, khz, max_frames=40):
    """SPI mode 0 frames between CS falling and rising, as the W5500 sees them."""
    smp = capture_samples(raw)
    ns = 1e6 / khz
    lines = []
    # narrowest pulses on CLK and CS (glitch check)
    def runs(mask):
        hi = lo = 1 << 30
        start, level = 0, smp[0] & mask
        for i in range(1, len(smp)):
            l = smp[i] & mask
            if l != level:
                n = i - start
                if level: hi = min(hi, n)
                else: lo = min(lo, n)
                start, level = i, l
        return hi, lo
    def fmt(v):
        return "-" if v >= 1 << 30 else "%.0f ns" % (v * ns)
    ch, cl = runs(CLK)
    sh, sl = runs(CS)
    lines.append("capture: %d samples at %.1f MHz (%.2f ms); narrowest CLK high %s, low %s; "
                 "narrowest CS high %s, low %s; first sample CLK=%d MOSI=%d MISO=%d CS=%d" % (
                 len(smp), khz / 1000.0, len(smp) * ns / 1e6, fmt(ch), fmt(cl), fmt(sh), fmt(sl),
                 smp[0] & 1, smp[0] >> 1 & 1, smp[0] >> 2 & 1, smp[0] >> 3 & 1))
    frames = []
    i, n = 0, len(smp)
    # frame in progress at the trigger (we start at a clock, CS may already be low)
    while i < n:
        if smp[i] & CS:
            i += 1
            continue
        start = i
        bits_o, bits_i, clocks = [], [], 0
        prev = smp[i]
        i += 1
        while i < n and not (smp[i] & CS):
            v = smp[i]
            if (v & CLK) and not (prev & CLK):
                clocks += 1
                bits_o.append(1 if v & MOSI else 0)
                bits_i.append(1 if v & MISO else 0)
            prev = v
            i += 1
        end = i
        frames.append((start, end, clocks, bits_o, bits_i, end >= n))
    def to_bytes(bits):
        return " ".join("%02x" % int("".join(map(str, bits[k:k + 8])), 2) for k in range(0, len(bits) - 7, 8))
    prev_end = None
    for k, (st, en, clocks, bo, bi, cut) in enumerate(frames[:max_frames]):
        gap = "" if prev_end is None else " gap %.1fus" % ((st - prev_end) * ns / 1000)
        extra = " +%d bits" % (clocks % 8) if clocks % 8 else ""
        mo = to_bytes(bo)[:3 * 24]
        mi = to_bytes(bi)[:3 * 24]
        lines.append("frame %d @%.1fus len %.1fus%s: %d clocks%s%s  MOSI %s  MISO %s" % (
            k, st * ns / 1000, (en - st) * ns / 1000, gap, clocks, extra, " (cut off)" if cut else "", mo, mi))
        prev_end = en
    if len(frames) > max_frames:
        lines.append("(%d more frames)" % (len(frames) - max_frames))
    return lines, smp


VID, PID = "cafe", "4dc5"


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("U") << 8) | nr


class _Bulk(ctypes.Structure):
    _fields_ = [("ep", ctypes.c_uint), ("len", ctypes.c_uint),
                ("timeout", ctypes.c_uint), ("data", ctypes.c_void_p)]


USBDEVFS_BULK = _ioc(3, 2, ctypes.sizeof(_Bulk))
USBDEVFS_CLAIMINTERFACE = _ioc(2, 15, 4)
USBDEVFS_RELEASEINTERFACE = _ioc(2, 16, 4)


def _rd(path):
    with open(path) as f:
        return f.read().strip()


class UsbLink:
    """Bulk endpoints of the cable's vendor interface via /dev/bus/usb (stdlib only)."""

    @staticmethod
    def find():
        for d in glob.glob("/sys/bus/usb/devices/*"):
            try:
                if _rd(d + "/idVendor") != VID or _rd(d + "/idProduct") != PID:
                    continue
                for itf in glob.glob(d + ":*"):
                    if _rd(itf + "/bInterfaceClass") != "ff":
                        continue
                    ep_in = ep_out = None
                    for ep in glob.glob(itf + "/ep_*"):
                        if _rd(ep + "/type") != "Bulk":
                            continue
                        addr = int(_rd(ep + "/bEndpointAddress"), 16)
                        if _rd(ep + "/direction") == "in":
                            ep_in = addr
                        else:
                            ep_out = addr
                    if ep_in is not None and ep_out is not None:
                        node = "/dev/bus/usb/%03d/%03d" % (int(_rd(d + "/busnum")), int(_rd(d + "/devnum")))
                        return node, int(_rd(itf + "/bInterfaceNumber"), 16), ep_in, ep_out
            except (OSError, ValueError):
                continue
        return None

    def __init__(self, node, ifnum, ep_in, ep_out):
        self.name = "USB %s interface %d" % (node, ifnum)
        self.fd = os.open(node, os.O_RDWR)
        self.ifnum = ifnum
        self.ep_in, self.ep_out = ep_in, ep_out
        fcntl.ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", ifnum))
        self.reader = None
        self.closed = False

    def _bulk(self, ep, buf, length, timeout_ms):
        b = _Bulk(ep, length, timeout_ms, ctypes.cast(buf, ctypes.c_void_p).value)
        return fcntl.ioctl(self.fd, USBDEVFS_BULK, b, True)

    def _reader(self):
        """Bulk reads in a thread, one packet at a time. A usbfs bulk read that
        times out throws away whatever part of the transfer had already arrived,
        which loses data when the cable pauses in the middle of a long stream
        (seen with logic captures). So reads run in this thread and the
        main loop waits on a queue instead. Each read asks for one packet (64
        bytes), so a transfer always completes at the next packet and a timeout
        can only ever cut off an empty read."""
        buf = ctypes.create_string_buffer(64)
        while not self.closed:
            try:
                n = self._bulk(self.ep_in, buf, len(buf), 1000)
            except OSError as e:
                if e.errno == errno.ETIMEDOUT:      # nothing arrived for 1 s
                    continue
                self.rq.put(e)
                return
            if n:
                self.rq.put(buf.raw[:n])

    def read(self, timeout):
        if self.reader is None:
            self.rq = queue.Queue()
            self.reader = threading.Thread(target=self._reader, daemon=True)
            self.reader.start()
        try:
            item = self.rq.get(timeout=timeout)
        except queue.Empty:
            return b""
        out = bytearray()
        while True:
            if isinstance(item, Exception):
                if out:                         # deliver what we have, raise next time
                    self.rq.put(item)
                    return bytes(out)
                raise item
            out += item
            try:
                item = self.rq.get_nowait()
            except queue.Empty:
                return bytes(out)

    def write(self, data):
        view = bytes(data)
        while view:
            chunk = view[:4096]
            buf = ctypes.create_string_buffer(chunk, len(chunk))
            n = self._bulk(self.ep_out, buf, len(chunk), 2000)
            view = view[n:]

    def close(self):
        self.closed = True
        if self.reader is not None:
            self.reader.join(4)
        try:
            fcntl.ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", self.ifnum))
        except OSError:
            pass
        os.close(self.fd)


class TtyLink:
    """Serial/pty transport (used by the test harness: --device /dev/pts/N)."""

    def __init__(self, dev):
        self.name = dev
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        if os.isatty(self.fd):
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def read(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        data = os.read(self.fd, 65536)
        if not data:
            raise OSError("device closed")
        return data

    def write(self, data):
        view = memoryview(bytes(data))
        while view:
            try:
                n = os.write(self.fd, view)
                view = view[n:]
            except BlockingIOError:
                select.select([], [self.fd], [], 1.0)

    def close(self):
        os.close(self.fd)


class Server:
    def __init__(self, args):
        self.args = args
        self.link = None
        self.img = None
        self.blocks = 0
        self.dirty = False
        self.last_sync = time.monotonic()
        self.inbuf = bytearray()
        self.capbuf = bytearray()
        self.stats = [0, 0]
        self.mounted = False
        self.last_mcheck = 0.0

    def open_image(self):
        flags = os.O_RDONLY if self.args.readonly else os.O_RDWR
        self.img = os.open(self.args.image, flags)
        self.blocks = (os.fstat(self.img).st_size // BLOCK) & ~1023

    def info(self):
        m = image_mounted(self.args.image)
        if m and not self.mounted:
            log("image is mounted on the Pi - the Dreamcast sees no card until it is unmounted")
        elif self.mounted and not m:
            log("image unmounted - card available again")
        self.mounted = m
        blocks = 0 if self.mounted else self.blocks
        flags = 1 if self.args.readonly else 0
        return b"I" + struct.pack("<IB", blocks, flags)

    def send(self, data):
        self.link.write(data)

    def handle(self):
        buf = self.inbuf
        while buf:
            t = buf[0]
            if t == ord("H"):
                del buf[:1]
                self.send(self.info())
            elif t == ord("R"):
                if len(buf) < 7:
                    return
                lba, count = struct.unpack_from("<IH", buf, 1)
                del buf[:7]
                out = bytearray()
                for i in range(count):
                    b = lba + i
                    if b < self.blocks:
                        data = os.pread(self.img, BLOCK, b * BLOCK)
                        data = data.ljust(BLOCK, b"\0")
                    else:
                        data = bytes(BLOCK)
                    out += b"D" + struct.pack("<I", b) + data + struct.pack(">H", binascii.crc_hqx(data, 0))
                self.send(out)
                self.stats[0] += count
            elif t == ord("S"):                 # status line from the cable firmware
                if len(buf) < 2 or len(buf) < 2 + buf[1]:
                    return
                n = buf[1]
                log("cable: " + bytes(buf[2:2 + n]).decode("ascii", "replace"))
                del buf[:2 + n]
            elif t == ord("L"):                 # logic capture chunk
                if len(buf) < 3:
                    return
                n = buf[1] | (buf[2] << 8)
                if n == 0:
                    if len(buf) < 7:
                        return
                    khz = struct.unpack_from("<I", buf, 3)[0]
                    del buf[:7]
                    self.capture_done(bytes(self.capbuf), khz)
                    self.capbuf = bytearray()
                    continue
                if len(buf) < 3 + n:
                    return
                self.capbuf += buf[3:3 + n]
                del buf[:3 + n]
            elif t == ord("W"):
                if len(buf) < 517:
                    return
                lba = struct.unpack_from("<I", buf, 1)[0]
                data = bytes(buf[5:517])
                del buf[:517]
                status = 0
                if self.args.readonly or self.mounted or lba >= self.blocks:
                    status = 1
                else:
                    os.pwrite(self.img, data, lba * BLOCK)
                    self.dirty = True
                    self.stats[1] += 1
                self.send(b"A" + struct.pack("<IB", lba, status))
            else:
                log("protocol resync: dropping byte 0x%02x" % t)
                del buf[:1]

    def open_link(self):
        if self.args.device:
            return TtyLink(self.args.device)
        found = UsbLink.find()
        if not found:
            raise FileNotFoundError("cable not found")
        return UsbLink(*found)

    def serve(self):
        waiting_logged = False
        while True:
            try:
                self.link = self.open_link()
            except OSError as e:
                if not waiting_logged:
                    log("waiting for the cable (%s)" % e)
                    waiting_logged = True
                time.sleep(1)
                continue
            waiting_logged = False
            try:
                log("connected to %s (%d MiB image%s)" % (self.link.name, self.blocks * BLOCK >> 20,
                                                         ", read-only" if self.args.readonly else ""))
                self.inbuf.clear()
                self.send(self.info())
                self.loop()
            except OSError as e:
                log("link lost: %s" % e)
            finally:
                self.sync()
                try:
                    self.link.close()
                except OSError:
                    pass
                self.link = None
            time.sleep(1)

    def sync(self):
        if self.dirty and self.img is not None:
            os.fsync(self.img)
            self.dirty = False
        self.last_sync = time.monotonic()

    def capture_done(self, raw, khz):
        d = os.path.dirname(getattr(self.args, "mode_file", "/var/lib/dcsd/mode")) or "."
        stamp = time.strftime("%Y%m%d-%H%M%S")
        base = os.path.join(d, "capture-" + stamp)
        try:
            with open(base + ".bin", "wb") as f:
                f.write(raw)
            lines, smp = decode_capture(raw, khz)
            write_vcd(base + ".vcd", smp, khz)
            with open(base + ".txt", "w") as f:
                f.write("\n".join(lines) + "\n")
            for l in lines:
                log(l)
            log("capture saved: %s.txt / .vcd (PulseView) / .bin" % base)
        except Exception as e:                    # never let diagnostics kill the daemon
            log("capture decode failed: %s" % e)

    def check_capture_request(self):
        req = os.path.join(os.path.dirname(getattr(self.args, "mode_file", "/var/lib/dcsd/mode")) or ".",
                           "capture-request")
        if os.path.exists(req):
            try:
                os.unlink(req)
            except OSError:
                pass
            log("arming logic capture")
            self.send(b"C\0")

    def send_mode(self, force=False):
        m = read_mode(getattr(self.args, "mode_file", "/var/lib/dcsd/mode"))
        if force or m != getattr(self, "mode", None):
            if not force or m:
                name = [k for k, v in MODES.items() if v == m][0]
                log("bus mode: %s" % name)
            self.mode = m
            self.send(b"M" + bytes([m]))

    def loop(self):
        last_stats = last_hb = last_mode = time.monotonic()
        self.send_mode(force=True)
        while True:
            data = self.link.read(0.05)
            if data:
                self.inbuf += data
                self.handle()
            now = time.monotonic()
            if now - last_mode > 0.3:
                last_mode = now
                self.send_mode()
                self.check_capture_request()
            if now - last_hb > 2.0:                   # heartbeat; also reports mount changes
                last_hb = now
                self.send(self.info())
            if self.dirty and now - self.last_sync > 1.0:
                self.sync()
            if now - last_stats > 60 and any(self.stats):
                log("last minute: %d blocks read, %d written" % tuple(self.stats))
                self.stats = [0, 0]
                last_stats = now


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="/var/lib/dcsd/dcsd.img", help="disk image (created if missing)")
    ap.add_argument("--size", default="2G", help="size when creating a new image (default 2G)")
    ap.add_argument("--device", default=None,
                    help="use a serial/pty device instead of the USB link (testing)")
    ap.add_argument("--readonly", action="store_true", help="refuse writes from the Dreamcast")
    ap.add_argument("--mode-file", default="/var/lib/dcsd/mode",
                    help="bus mode override (auto/serial/spi/w5500/sd), written by dcsd-mode")
    args = ap.parse_args()

    if not os.path.exists(args.image):
        os.makedirs(os.path.dirname(os.path.abspath(args.image)), exist_ok=True)
        create_image(args.image, parse_size(args.size))
    srv = Server(args)
    srv.open_image()
    srv.serve()


if __name__ == "__main__":
    main()
