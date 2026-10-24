#!/usr/bin/env python3
"""Cycle-level check of w5500_spi.pio against Dreamcast-like SPI waveforms.

Decodes the program from the pioasm-generated header (the real opcodes), runs it
with RP2040 semantics (2-cycle input synchroniser, stalls, FIFOs, TXSTALL) and a
simple model of the CPU answering RX words after a fixed latency.

usage: pio_sim.py build/w5500_spi.pio.h [sys_mhz]
"""
import re, sys

SCLK, MOSI, MISO, CS = 2, 3, 4, 5
IN_BASE, OUT_BASE, JMP_PIN = 3, 4, 5

def load(path):
    txt = open(path).read()
    body = txt.split("w5500_spi_program_instructions[] = {")[1].split("};")[0]
    prog = [int(m, 16) for m in re.findall(r"0x([0-9a-f]{4}),", body)]
    d = {k: int(v) for k, v in re.findall(r"#define w5500_spi_(\w+) (\d+)", txt)}
    return prog, d

class SM:
    def __init__(s, prog, d, y=3):
        s.p, s.d = prog, d
        s.pc = d["offset_start"]
        s.x = 0; s.y = y
        s.isr = s.isc = 0
        s.osr = s.osc = 0
        s.rx, s.tx = [], []
        s.delay = 0
        s.txstall = False
        s.pins_out = {MISO: 1}
        s.hist = []          # delayed inputs (synchroniser)

    def rd(self, gpio): return (self.inp >> gpio) & 1

    def step(s, gpio_now):
        s.hist.append(gpio_now)
        s.inp = s.hist[-3] if len(s.hist) >= 3 else gpio_now
        if s.delay: s.delay -= 1; return
        ins = s.p[s.pc]
        op, dly = ins >> 13, (ins >> 8) & 31
        nxt = s.pc + 1
        if s.pc == s.d["wrap"]: nxt = s.d["wrap_target"]
        stall = False
        rot = ((s.inp >> IN_BASE) | (s.inp << (32 - IN_BASE))) & 0xFFFFFFFF
        def src(v):
            return {0: rot, 1: s.x, 2: s.y, 3: 0, 6: s.isr, 7: s.osr}[v]
        if op == 0:                                   # JMP
            c, a = (ins >> 5) & 7, ins & 31
            take = {0: True, 1: s.x == 0, 2: s.x != 0, 3: s.y == 0, 4: s.y != 0,
                    5: s.x != s.y, 6: bool(s.rd(JMP_PIN)), 7: s.osc < 32}[c]
            if c == 2: s.x = (s.x - 1) & 0xFFFFFFFF
            if c == 4: s.y = (s.y - 1) & 0xFFFFFFFF
            if take: nxt = a
        elif op == 1:                                 # WAIT gpio
            pol, idx = (ins >> 7) & 1, ins & 31
            assert (ins >> 5) & 3 == 0
            if s.rd(idx) != pol: stall = True
        elif op == 2:                                 # IN
            v, n = src((ins >> 5) & 7), (ins & 31) or 32
            v &= (1 << n) - 1
            s.isr = ((s.isr << n) | v) & 0xFFFFFFFF if n < 32 else v
            s.isc = min(32, s.isc + n)
        elif op == 3:                                 # OUT
            dst, n = (ins >> 5) & 7, (ins & 31) or 32
            v = s.osr >> (32 - n)
            s.osr = (s.osr << n) & 0xFFFFFFFF
            s.osc = min(32, s.osc + n)
            assert dst == 0 and n == 1
            s.pins_out[MISO] = v & 1
        elif op == 4:                                 # PUSH / PULL
            pull, blk = (ins >> 7) & 1, (ins >> 5) & 1
            if not pull:
                if len(s.rx) >= 4:
                    if blk: stall = True
                    else: s.lost = getattr(s, "lost", 0) + 1; s.isr = s.isc = 0
                else:
                    s.rx.append(s.isr); s.isr = s.isc = 0
            else:
                if s.tx: s.osr = s.tx.pop(0); s.osc = 0
                elif blk: stall = True; s.txstall = True
                else: s.osr = s.x; s.osc = 0
        elif op == 5:                                 # MOV
            dst, o, sv = (ins >> 5) & 7, (ins >> 3) & 3, ins & 7
            v = src(sv)
            if o == 1: v = ~v & 0xFFFFFFFF
            if o == 2: v = int(f"{v:032b}"[::-1], 2)
            if dst == 1: s.x = v
            elif dst == 2: s.y = v
            elif dst == 6: s.isr = v; s.isc = 0
            elif dst == 7: s.osr = v; s.osc = 0
            else: raise ValueError(dst)
        elif op == 7:                                 # SET
            dst, v = (ins >> 5) & 7, ins & 31
            if dst == 0: s.pins_out[MISO] = v & 1
            elif dst == 1: s.x = v
            elif dst == 2: s.y = v
            else: raise ValueError(dst)
        else:
            raise ValueError(hex(ins))
        if stall: return
        s.pc = nxt
        s.delay = dly

# ---- W5500 register model for the CPU (just enough to answer) -------------
REGS = {(0, 0x00): 0x00, (0, 0x39): 0x04}

class CPU:
    """Reads one RX word per poll; answers after 'lat' cycles."""
    def __init__(c, sm, lat, end_cycles=6):
        c.sm, c.lat, c.end_cycles = sm, lat, end_cycles
        c.busy_until = 0
        c.pending = []
        c.words = []
        c.frames = []
        c.cur = []
        c.phase = 0
    def step(c, t):
        for p in [p for p in c.pending if p[0] <= t]:
            c.pending.remove(p); p[1]()
        if t < c.busy_until or not c.sm.rx: return
        v = c.sm.rx.pop(0)
        c.words.append(v)
        c.busy_until = t + 6
        sm = c.sm
        def push(b): (sm.tx.append(b << 24) if len(sm.tx) < 4 else None)
        if v == 0xFFFFFFFF:
            c.frames.append((c.cur, sm.txstall)); c.cur = []; sm.txstall = False
            sm.tx.clear()                  # exec'd pulls
            c.phase = 0
            c.busy_until = t + c.end_cycles  # frame-end bookkeeping
            return
        c.cur.append(v & 0xFF if c.phase == 3 else v)   # data words: low byte (upper bits = other pins)
        if c.phase == 0:
            c.addr = v
            c.phase = 1
        elif c.phase == 1:
            c.bsb = v
            b = REGS.get((c.bsb, c.addr), 0x5A)
            c.pending.append((t + c.lat, lambda: push(b)))
            c.phase = 2
        elif c.phase == 2:
            c.write = (v >> 2) & 1
            c.pending.append((t + 10, lambda: [push(0 if c.write else 0xEE) for _ in range(3)]))
            c.phase = 3
        else:
            c.pending.append((t + 10, lambda: push(0 if c.write else 0xEE)))

# ---- Dreamcast waveform --------------------------------------------------
def dc_frames(frames, ns, bit_ns=130, gap_ns=300, lead_ns=200, sample_ns=-30, spikes=(), byte_gap_ns=0):
    # sample_ns: where the DC effectively samples MISO, relative to the rising
    # edge. Measured on a real DC: before it (KOS reads RXD through the SCIF
    # port register, which lags), so MISO must change right after the edge.
    """frames: list of byte lists. Returns gpio(t) samples (per cycle) and the
    MISO sample times per frame."""
    ev = []          # (time_ns, pin, value)
    samples = []
    t = lead_ns
    ev.append((0, CS, 1)); ev.append((0, SCLK, 0)); ev.append((0, MOSI, 1))
    for fr in frames:
        ev.append((t, CS, 0)); t += 60
        bits = [(b >> (7 - i)) & 1 for b in fr for i in range(8)]
        ss = []
        for i, b in enumerate(bits):
            if i and i % 8 == 0: t += byte_gap_ns
            ev.append((t, MOSI, b)); t += bit_ns / 2
            ev.append((t, SCLK, 1)); ss.append(t + sample_ns); t += bit_ns / 2
            ev.append((t, SCLK, 0))
        t += 40
        ev.append((t, CS, 1)); t += gap_ns
        samples.append(ss)
    for (a, w) in spikes:
        ev.append((a, CS, 1)); ev.append((a + w, CS, 0))
    ev.sort(key=lambda e: e[0])
    return ev, samples, t + 500

def run(path, mhz, frames, lat_cycles, end_cycles=6, **kw):
    prog, d = load(path)
    sm = SM(prog, d)
    cpu = CPU(sm, lat_cycles, end_cycles)
    ev, samples, end = dc_frames(frames, 1, **kw)
    cyc = 1000.0 / mhz
    lv = {SCLK: 0, MOSI: 1, CS: 1}
    ei = 0
    miso_trace = []
    n = int(end / cyc)
    for k in range(n):
        tn = k * cyc
        while ei < len(ev) and ev[ei][0] <= tn:
            lv[ev[ei][1]] = ev[ei][2]; ei += 1
        g = sum(v << p for p, v in lv.items())
        sm.step(g)
        cpu.step(k)
        miso_trace.append((tn, sm.pins_out[MISO]))
    # MISO as the DC reads it
    def miso_at(tt):
        i = min(int(tt / cyc), len(miso_trace) - 1)
        return miso_trace[i][1]
    got = []
    for ss in samples:
        bits = [miso_at(x) for x in ss]
        got.append([int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)])
    return cpu, got, sm

def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    return cond

if __name__ == "__main__":
    path = sys.argv[1]
    ok = True
    probe = [[0x00, 0x00, 0x04, 0x80], [0x00, 0x00, 0x00, 0xFF], [0x00, 0x39, 0x00, 0xFF]]
    for mhz in (200, 250):
        cyc = 1000 / mhz
        # the CPU has ~3.5 bit times after the 21st bit: try a realistic and a slow answer
        for lat_ns in (120, 380):
            lat = int(lat_ns / cyc)
            for gap in (300, 120, 70):
                cpu, got, sm = run(path, mhz, probe, lat, gap_ns=gap)
                tag = f"{mhz}MHz lat{lat_ns}ns gap{gap}ns"
                ok &= check(f"{tag}: 3 frames seen (no merge)", len(cpu.frames) == 3)
                if len(cpu.frames) == 3:
                    f = [fr for fr, _ in cpu.frames]
                    ok &= check(f"{tag}: headers", f[0][:4] == [0, 0, 4, 0x80] and f[1][:3] == [0, 0, 0] and f[2][:2] == [0x39, 0])
                    ok &= check(f"{tag}: MISO of version read = {got[2][3]:02x}", got[2][3] == 0x04)
                    ok &= check(f"{tag}: no TXSTALL", not any(st for _, st in cpu.frames))
        # CS spikes inside a frame (between bytes and mid-byte) must not end it
        b = 130
        cpu, got, sm = run(path, mhz, [[0x00, 0x39, 0x00, 0xFF, 0xFF]], int(120 / cyc), lead_ns=200,
                           spikes=[(200 + 60 + 16 * b + 70, 12), (200 + 60 + 26 * b + 70, 12),
                                   (200 + 60 + 32 * b + 15, 12)])          # the last one hits the between-bytes check
        ok &= check(f"{mhz}MHz: 12 ns CS spikes ignored", len(cpu.frames) == 1 and len(cpu.frames[0][0]) == 5
                    and got[0][3:] == [0x04, 0xEE])
        # long burst read, e.g. 8 data bytes
        cpu, got, sm = run(path, mhz, [[0x00, 0x09, 0x00] + [0xFF] * 8, [0x00, 0x39, 0x00, 0xFF]], int(120 / cyc), gap_ns=60)
        ok &= check(f"{mhz}MHz: burst + next frame", len(cpu.frames) == 2 and len(cpu.frames[0][0]) == 11 and got[1][3] == 4)
    # how late may the answer be? Uniform 130 ns bits, and KOS-like bytes of
    # 80 ns bits with 400 ns between bytes (same ~4.2 us per frame)
    for bit, bg in ((130, 0), (80, 400)):
        for mhz in (200, 250):
            cyc = 1000 / mhz
            last = 0
            for lat in range(2, 200, 2):
                cpu, got, sm = run(path, mhz, probe, lat, gap_ns=300, bit_ns=bit, byte_gap_ns=bg)
                if len(cpu.frames) == 3 and got[2][3] == 4 and not any(st for _, st in cpu.frames):
                    last = lat
                else:
                    break
            print(f"info {mhz}MHz, {bit} ns bits, {bg} ns between bytes: the answer may take up to "
                  f"{last} cycles ({last * cyc:.0f} ns) after the block-select word is read")
    # KOS hw_init: back-to-back register writes (scif_spi_write_byte: ~85 ns
    # bits) with only a mutex unlock/lock between them (~200 ns of CS high).
    # How long may the CPU's frame-end work take before the next frame breaks?
    wr = [[0x00, 0x2E, 0x04, 0x00], [0x00, 0x2E, 0x04, 0xB8], [0x00, 0x39, 0x00, 0xFF]]
    for mhz in (200, 250):
        cyc = 1000 / mhz
        last = 0
        for endc in range(0, 1200, 20):
            cpu, got, sm = run(path, mhz, wr, 20, end_cycles=endc, gap_ns=200, bit_ns=85, byte_gap_ns=150)
            f = [fr for fr, _ in cpu.frames]
            good = (len(f) == 3 and f[0][3] == 0x00 and f[1][3] == 0xB8 and got[2][3] == 4
                    and not any(st for _, st in cpu.frames))
            if not good: break
            last = endc
        print(f"info {mhz}MHz, back-to-back 85 ns-bit writes: frame-end work may take up to {last} cycles ({last * cyc:.0f} ns)")
    # With the non-stalling pull, a late CPU must never cost alignment:
    # back-to-back writes stay correct however slow the answer is, and a late
    # read only gets 0xFF in its first byte.
    for mhz in (200, 250):
        cpu, got, sm = run(path, mhz, wr, 2000, end_cycles=20, gap_ns=200, bit_ns=85, byte_gap_ns=150)
        f = [fr for fr, _ in cpu.frames]
        ok &= check(f"{mhz}MHz: very late CPU, writes still aligned",
                    len(f) == 3 and f[0][3] == 0x00 and f[1][3] == 0xB8 and f[2][:2] == [0x39, 0])
        ok &= check(f"{mhz}MHz: very late CPU, late read answers ff", got[2][3] == 0xFF)
    sys.exit(0 if ok else 1)
