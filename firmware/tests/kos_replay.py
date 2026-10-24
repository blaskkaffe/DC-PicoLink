"""Replay KOS scif-spi.c register writes (SCSPTR2) as pin events into pio_sim."""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pio_sim as P
SCLK, MOSI, CS = P.SCLK, P.MOSI, P.CS
RTSIO, RTSDT, CTSIO, CTSDT, SPB2IO, SPB2DT = 0x80, 0x40, 0x20, 0x10, 0x02, 0x01

class KOS:
    def __init__(k, W=40, R=60, call=60, mutex=150):
        k.t = 200.0; k.ev = [(0, CS, 1), (0, SCLK, 0), (0, MOSI, 1)]
        k.W, k.R, k.call, k.mutex = W, R, call, mutex
        k.scsptr2 = RTSIO | RTSDT | CTSIO | SPB2IO
    def wr(k, v):
        k.ev += [(k.t, CS, 1 if v & RTSDT else 0), (k.t, SCLK, 1 if v & CTSDT else 0), (k.t, MOSI, v & SPB2DT)]
        k.t += k.W
    def rd(k): k.t += k.R
    def set_cs(k, enabled):            # scif_set_cs_wrapper(enabled) -> scif_spi_set_cs(!enabled)
        if enabled: k.t += k.mutex
        if not enabled: k.scsptr2 |= RTSDT
        else: k.scsptr2 &= ~RTSDT
        k.wr(k.scsptr2)
        if not enabled: k.t += k.mutex
    def write_byte(k, b):
        tmp = k.scsptr2 & ~CTSDT & ~SPB2DT
        k.t += k.call
        for i in range(7, -1, -1):
            bit = (b >> i) & 1
            k.wr(tmp | bit); k.wr(tmp | bit | CTSDT)
        k.wr(tmp)
    def read_byte(k):
        tmp = (k.scsptr2 & ~CTSDT) | SPB2DT
        k.t += k.call
        for i in range(8):
            k.wr(tmp); k.wr(tmp | CTSDT); k.rd()
    def read_reg(k, block, addr):
        k.t += k.call
        k.set_cs(True)
        for b in (addr >> 8, addr & 255, block << 3): k.write_byte(b)
        k.read_byte()
        k.set_cs(False)
    def write_reg(k, block, addr, v):
        k.t += k.call
        k.set_cs(True)
        for b in (addr >> 8, addr & 255, (block << 3) | 4, v): k.write_byte(b)
        k.set_cs(False)

def run(mhz, **kw):
    k = KOS(**kw)
    k.read_reg(0, 0x0000)          # soft reset poll: MR
    k.write_reg(0, 0x002E, 0x00)   # PHY reset
    k.write_reg(0, 0x002E, 0xB8)
    k.read_reg(0, 0x0039)          # version
    ev = sorted(k.ev, key=lambda e: e[0])
    prog, d = P.load(sys.argv[1])
    sm = P.SM(prog, d); cpu = P.CPU(sm, 20)
    cyc = 1000.0 / mhz; lv = {SCLK: 0, MOSI: 1, CS: 1}; ei = 0
    for n in range(int((k.t + 1000) / cyc)):
        tn = n * cyc
        while ei < len(ev) and ev[ei][0] <= tn:
            lv[ev[ei][1]] = ev[ei][2]; ei += 1
        sm.step(sum(v << p for p, v in lv.items())); cpu.step(n)
    return [[hex(w) for w in f] for f, _ in cpu.frames], [hex(w) for w in cpu.cur], hex(sm.pc)

for mhz in (200, 250):
    for W in (20, 40, 80):
        print(mhz, W, run(mhz, W=W))
