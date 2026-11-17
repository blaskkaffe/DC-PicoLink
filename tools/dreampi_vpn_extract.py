#!/usr/bin/env python3
"""
Copy the Dreamcast Live VPN profiles out of a DreamPi 2.x SD card image, with nothing but
Python 3 (works in a-Shell on an iPhone or iPad, or on any computer).

    python3 dreampi_vpn_extract.py dreampi-2.1.img      (or the .zip, or a folder holding either)

It reads the image's Linux partition directly (ext4, read-only) and writes
dreampi-vpn.txt next to it: upload that file on DC-PicoLink's page (VPN profiles), or
attach it in the chat. It also writes the files themselves into a dreampi-vpn/ folder.
"""
import os
import struct
import sys
import zipfile

WANT = "/etc/openvpn/client"


class Ext4:
    def __init__(self, f, start):
        self.f, self.start = f, start
        sb = self.read(1024, 1024)
        if struct.unpack_from("<H", sb, 0x38)[0] != 0xEF53:
            raise ValueError("not an ext2/3/4 file system")
        self.bs = 1024 << struct.unpack_from("<I", sb, 0x18)[0]
        self.per_group = struct.unpack_from("<I", sb, 0x28)[0]
        self.inode_size = struct.unpack_from("<H", sb, 0x58)[0] if struct.unpack_from("<I", sb, 0x4C)[0] else 128
        incompat = struct.unpack_from("<I", sb, 0x60)[0]
        self.desc_size = struct.unpack_from("<H", sb, 0xFE)[0] if incompat & 0x80 else 32
        if self.desc_size < 32:
            self.desc_size = 32
        first = struct.unpack_from("<I", sb, 0x14)[0]
        self.gdt = (first + 1) * self.bs

    def read(self, off, n):
        self.f.seek(self.start + off)
        return self.f.read(n)

    def inode(self, num):
        g, i = divmod(num - 1, self.per_group)
        d = self.read(self.gdt + g * self.desc_size, self.desc_size)
        table = struct.unpack_from("<I", d, 8)[0]
        if self.desc_size >= 64:
            table |= struct.unpack_from("<I", d, 0x28)[0] << 32
        return self.read(table * self.bs + i * self.inode_size, self.inode_size)

    def blocks(self, ino):
        """the file's data blocks, in order (extents or the old block map)"""
        flags = struct.unpack_from("<I", ino, 0x20)[0]
        iblock = ino[0x28:0x28 + 60]
        if flags & 0x80000:                      # extents
            yield from self._extents(iblock)
        else:
            for k in range(12):
                b = struct.unpack_from("<I", iblock, 4 * k)[0]
                if b:
                    yield b
            per = self.bs // 4

            def ind(b, depth):
                if not b:
                    return
                data = self.read(b * self.bs, self.bs)
                for k in range(per):
                    x = struct.unpack_from("<I", data, 4 * k)[0]
                    if depth == 0:
                        if x:
                            yield x
                    else:
                        yield from ind(x, depth - 1)
            for depth, k in ((0, 12), (1, 13), (2, 14)):
                yield from ind(struct.unpack_from("<I", iblock, 4 * k)[0], depth)

    def _extents(self, node):
        magic, entries, _, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise ValueError("bad extent header")
        for k in range(entries):
            e = node[12 + 12 * k: 24 + 12 * k]
            if depth == 0:
                _, ln, hi, lo = struct.unpack("<IHHI", e)
                if ln > 32768:                   # not written yet: reads as zeros
                    ln -= 32768
                for b in range(ln):
                    yield (hi << 32 | lo) + b
            else:
                _, lo, hi, _ = struct.unpack("<IIHH", e)
                yield from self._extents(self.read((hi << 32 | lo) * self.bs, self.bs))

    def content(self, ino):
        size = struct.unpack_from("<I", ino, 4)[0] | struct.unpack_from("<I", ino, 0x6C)[0] << 32
        flags = struct.unpack_from("<I", ino, 0x20)[0]
        if flags & 0x10000000:                   # inline data (small files)
            return ino[0x28:0x28 + 60][:size]
        out = bytearray()
        for b in self.blocks(ino):
            out += self.read(b * self.bs, self.bs)
            if len(out) >= size:
                break
        return bytes(out[:size])

    def listdir(self, num):
        data, p, out = self.content(self.inode(num)), 0, {}
        while p + 8 <= len(data):
            inum, rec, nlen, ftype = struct.unpack_from("<IHBB", data, p)
            if rec < 8:
                break
            if inum:
                out[data[p + 8:p + 8 + nlen].decode("utf-8", "replace")] = (inum, ftype)
            p += rec
        return out

    def lookup(self, path):
        num = 2
        for part in [x for x in path.split("/") if x]:
            ent = self.listdir(num).get(part)
            if not ent:
                return None
            num = ent[0]
        return num


def collect(fs, path, depth=0):
    """every file under path (by name and by full path), for inline()"""
    out = {}
    num = fs.lookup(path)
    if not num or depth > 3:
        return out
    for name, (n, ftype) in fs.listdir(num).items():
        if name in (".", ".."):
            continue
        p = path + "/" + name
        if ftype == 1:
            data = fs.content(fs.inode(n))
            out[p] = data
            out.setdefault(name, data)
        elif ftype == 2:
            out.update(collect(fs, p, depth + 1))
    return out


def inline(conf, keys):
    """'ca ca.crt' style lines become <ca>...</ca> blocks (DC-PicoLink reads inline blocks only)"""
    out = []
    for line in conf.decode("utf-8", "replace").splitlines():
        w = line.split()
        if len(w) >= 2 and w[0] in ("ca", "cert", "key", "tls-auth", "tls-crypt"):
            data = keys.get(w[1]) or keys.get(os.path.basename(w[1]))
            if data is not None:
                out.append("<%s>\n%s\n</%s>" % (w[0], data.decode("utf-8", "replace").strip(), w[0]))
                if w[0] == "tls-auth" and len(w) >= 3:
                    out.append("key-direction " + w[2])
                continue
        out.append(line)
    return ("\n".join(out) + "\n").encode()


def partitions(f):
    f.seek(0)
    mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        return [0]                               # a bare file system
    out = []
    for k in range(4):
        e = mbr[446 + 16 * k: 462 + 16 * k]
        ptype, lba = e[4], struct.unpack_from("<I", e, 8)[0]
        if ptype == 0x83 and lba:
            out.append(lba * 512)
    return out


def open_image(path):
    if os.path.isdir(path):
        for n in sorted(os.listdir(path)):
            if n.lower().endswith((".img", ".zip")) and "dreampi" in n.lower():
                return open_image(os.path.join(path, n))
        sys.exit("no dreampi .img or .zip in " + path)
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path)
        imgs = [i for i in z.infolist() if i.filename.lower().endswith(".img")]
        if not imgs:
            sys.exit("no .img inside " + path)
        out = os.path.join(os.path.dirname(os.path.abspath(path)), os.path.basename(imgs[0].filename))
        if not os.path.exists(out) or os.path.getsize(out) != imgs[0].file_size:
            print("unpacking %s (%.1f GB, a few minutes)..." % (os.path.basename(out), imgs[0].file_size / 1e9))
            with z.open(imgs[0]) as src, open(out, "wb") as dst:
                done = 0
                while True:
                    b = src.read(4 << 20)
                    if not b:
                        break
                    dst.write(b)
                    done += len(b)
                    if done % (256 << 20) < (4 << 20):
                        print("  %d MB" % (done >> 20))
        return out
    return path


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    img = open_image(sys.argv[1])
    with open(img, "rb") as f:
        for start in partitions(f):
            try:
                fs = Ext4(f, start)
            except ValueError:
                continue
            d = fs.lookup(WANT)
            if not d:
                continue
            files = {}
            for name, (num, ftype) in sorted(fs.listdir(d).items()):
                if name in (".", "..") or ftype != 1:
                    continue
                files[name] = fs.content(fs.inode(num))
            if not files:
                continue
            keys = collect(fs, "/etc/openvpn")      # certificates a profile may name as separate files
            for name in list(files):
                if name.endswith((".conf", ".ovpn")):
                    files[name] = inline(files[name], keys)
            base = os.path.dirname(os.path.abspath(img))
            folder = os.path.join(base, "dreampi-vpn")
            os.makedirs(folder, exist_ok=True)
            txt = b""
            if "server.list" in files:
                txt += b"@@SERVERS@@\n" + files["server.list"] + b"\n"
            for name, data in files.items():
                with open(os.path.join(folder, name), "wb") as o:
                    o.write(data)
                if name.endswith((".conf", ".ovpn")):
                    txt += b"@@PROFILE@@\n" + data + b"\n"
            with open(os.path.join(base, "dreampi-vpn.txt"), "wb") as o:
                o.write(txt)
            print("found %d files in %s:" % (len(files), WANT))
            for name, data in files.items():
                print("  %-28s %6d bytes" % (name, len(data)))
            print("wrote dreampi-vpn.txt (upload it on the DC-PicoLink page, or attach it in the chat)")
            return
    sys.exit("no %s in this image (is it DreamPi 2.x?)" % WANT)


if __name__ == "__main__":
    main()
