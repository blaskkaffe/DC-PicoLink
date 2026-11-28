#!/usr/bin/env python3
"""Generate the DC-PicoLink stack PCB (KiCad 7, pcbnew Python API).

The board is the middle layer of a 3-board stack: RP2040-Zero on the top face (face up), ESP32-S3
SuperMini on the bottom face (face down), both USB ports at the board's square end. The other end
is a full-size SD card that goes into the slot of the DC SD adapter on the Dreamcast's serial port;
a 7-hole row behind it takes a ribbon cable instead, for anyone who cuts the card end off.
The slot card's push-push microSD socket sits between the card end and the stack.

Run:  python3 gen_board.py out.kicad_pcb       (then route: see make.sh)
Coordinates in mm, KiCad's y axis points down. Viewed from the top (the Zero's face = the card's
label side): the SD contact edge is at the top (y = 0), the USB end at the bottom.
"""
import sys
import pcbnew

FP = "/usr/share/kicad/footprints"
OUT = sys.argv[1] if len(sys.argv) > 1 else "dc-picolink-stack.kicad_pcb"

P = 2.54
WC = 24.0           # SD card width (card end)
WB = 18.0           # body width = the modules' width
XB0, XB1 = (WC - WB) / 2, (WC + WB) / 2     # body edges, 3 .. 21
XL, XR = WC / 2 - 3 * P, WC / 2 + 3 * P     # the two pin columns, 15.24 mm apart (4.38 / 19.62)
CARD = 22.0         # card end: how deep the adapter takes a card (32 mm card - ~12 mm sticking out) + 2 mm
YH = CARD + 2.0     # ribbon-cable row
YW = YH + P         # W5500 row, under the ribbon row
YS0, YS1 = YW + 1.9, YW + 1.9 + 17.4        # microSD socket zone
Y9 = YS1 + 1.9      # the Zero's bottom row (GP8 .. GP14) = position 9 of the side columns
L = Y9 + 8 * P + 1.3                        # USB end: 1.3 mm past position 1


def ypos(n):        # side position n: 1 at the USB end, 9 = the bottom row's corners
    return Y9 + P * (9 - n)


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


board = pcbnew.BOARD()
ds = board.GetDesignSettings()
ds.SetBoardThickness(mm(1.6))
ds.m_TrackMinWidth = mm(0.2)
ds.m_MinClearance = mm(0.15)          # the socket maker's own land pattern has 0.15 mm in one spot
ds.m_ViasMinSize = mm(0.6)
ds.m_MinThroughDrill = mm(0.3)
ds.m_HoleToHoleMin = mm(0.25)
ds.m_CopperEdgeClearance = mm(0.3)
ds.m_MinSilkTextHeight = mm(0.6)      # small labels: 18 mm is narrow (fabs print 0.6 mm, a little soft)
dflt = ds.m_NetSettings.m_DefaultNetClass
dflt.SetTrackWidth(mm(0.25)); dflt.SetClearance(mm(0.2)); dflt.SetViaDiameter(mm(0.7)); dflt.SetViaDrill(mm(0.35))

nets = {}


def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        nets[name] = n
    return nets[name]


def add_text(s, x, y, size=0.8, layer=pcbnew.F_SilkS, rot=0, bold=False, mirror=False):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(pt(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * (0.2 if bold else 0.15)))
    if rot:
        t.SetTextAngle(pcbnew.EDA_ANGLE(rot, pcbnew.DEGREES_T))
    if mirror:
        t.SetMirrored(True)
    board.Add(t)


def add_line(x1, y1, x2, y2, layer=pcbnew.Edge_Cuts, w=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pt(x1, y1)); s.SetEnd(pt(x2, y2))
    s.SetLayer(layer); s.SetWidth(mm(w))
    board.Add(s)


def track(points, netname, width=0.3, layer=pcbnew.F_Cu):
    for (a, b) in zip(points, points[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pt(*a)); t.SetEnd(pt(*b)); t.SetWidth(mm(width)); t.SetLayer(layer)
        t.SetNet(net(netname)); t.SetLocked(True); board.Add(t)


def via(x, y, netname):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pt(x, y)); v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(net(netname)); v.SetLocked(True); board.Add(v)


# ---- outline: the SD card end (corner chamfer on the pin-9 side: top right seen from the label side),
# then the body at the modules' width
CH = 4.0
outline = [(0, 0), (WC - CH, 0), (WC, CH), (WC, CARD), (XB1, CARD), (XB1, L), (XB0, L), (XB0, CARD),
           (0, CARD), (0, 0)]
for (a, b) in zip(outline, outline[1:]):
    add_line(a[0], a[1], b[0], b[1])


# ---- footprints -------------------------------------------------------------------------------
def load(lib, name, ref, x, y, rot=0, value=None, back=False):
    fp = pcbnew.FootprintLoad(f"{FP}/{lib}.pretty", name)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    fp.SetReference(ref)
    if value:
        fp.SetValue(value)
    fp.SetPosition(pt(x, y))
    if rot:
        fp.SetOrientationDegrees(rot)
    fp.Value().SetVisible(False)
    fp.Reference().SetLayer(pcbnew.F_Fab)
    board.Add(fp)
    if back:
        fp.Flip(fp.GetPosition(), False)
    return fp


def setnets(fp, m):
    for pad in fp.Pads():
        n = m.get(pad.GetNumber())
        if n:
            pad.SetNet(net(n))


def new_fp(ref, value, x, y):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetReference(ref); fp.SetValue(value)
    fp.SetPosition(pt(x, y))
    fp.Reference().SetVisible(False); fp.Value().SetVisible(False)
    board.Add(fp)
    return fp


def place_pad(fp, p, x, y):
    fp.Add(p)
    o = fp.GetPosition()
    p.SetPos0(pcbnew.VECTOR2I(mm(x) - o.x, mm(y) - o.y)); p.SetPosition(pt(x, y))


def tht_pad(fp, num, x, y, netname, drill=1.0, size=1.7, square=False):
    p = pcbnew.PAD(fp)
    p.SetNumber(num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    p.SetShape(pcbnew.PAD_SHAPE_RECT if square else pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    p.SetDrillSize(pcbnew.VECTOR2I(mm(drill), mm(drill)))
    p.SetLayerSet(pcbnew.PAD.PTHMask())
    place_pad(fp, p, x, y)
    if netname:
        p.SetNet(net(netname))
    return p


def smd_pad(fp, num, x, y, w, h, netname, back):
    p = pcbnew.PAD(fp)
    p.SetNumber(num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
    p.SetShape(pcbnew.PAD_SHAPE_RECT)
    p.SetSize(pcbnew.VECTOR2I(mm(w), mm(h)))
    ls = pcbnew.LSET()
    for l in ((pcbnew.B_Cu, pcbnew.B_Mask, pcbnew.B_Paste) if back else (pcbnew.F_Cu, pcbnew.F_Mask, pcbnew.F_Paste)):
        ls.AddLayer(l)
    p.SetLayerSet(ls)
    place_pad(fp, p, x, y)
    if netname:
        p.SetNet(net(netname))
    return p


def npth(fp, x, y, d):
    p = pcbnew.PAD(fp)
    p.SetNumber("")
    p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    p.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(pcbnew.VECTOR2I(mm(d), mm(d)))
    p.SetDrillSize(pcbnew.VECTOR2I(mm(d), mm(d)))
    p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    place_pad(fp, p, x, y)


# ---- the stack holes. Seen from the top, USB at the bottom: the Zero's 5V..GP15 side is on the right,
# GP0..GP7 on the left, and its bottom edge (GP8 GP9 GP10 GP11 GP12 GP13 GP14, left to right in this
# view) is one row whose two ends are the side columns' position 9.
ZR = ["+5V", "GND", "+3V3", "GP29", "GP28", "W_MOSI", "GP26", "GP15", "GP14"]
ZL = ["W_CS", "W_MISO", "W_SCLK", "GP3_HS", "GP4_MISO", "GP5_CS", "GP6_SCK", "GP7_MOSI", "GP8_DR"]
# The W5500 runs on four ESP32 pins that reach this board where the Zero has nothing: IO43 (CS),
# IO44 (MISO), IO1 (SCLK) under the Zero's unused GP0-GP2, IO11 (MOSI) under its unused GP27.
TOP_LABEL = {"W_CS": "W5500", "W_MISO": "W5500", "W_SCLK": "W5500", "W_MOSI": "W5500"}
BOT = ["GP9", "GP10", "GP11", "GP12", "GP13"]                 # x = XL + 1..5 pitches
# F = one long pin through all three boards, Z = Zero to this board only (cut flush underneath),
# E = ESP32 to this board only (cut flush on top; no Zero pin). The bottom row GP9-GP13 is all Z.
FIT_R = ["F", "F", "Z", "Z", "Z", "E", "Z", "Z", "Z"]
FIT_L = ["E", "E", "E", "F", "F", "F", "F", "F", "F"]
ESP_R = ["5V", "GND", "3V3", "IO13", "IO12", "IO11", "IO10", "IO9", "IO8"]
ESP_L = ["IO43", "IO44", "IO1", "IO2", "IO3", "IO4", "IO5", "IO6", "IO7"]

stack = new_fp("J1", "Stack: RP2040-Zero above, ESP32-S3 SuperMini below", WC / 2, Y9 + 10)
for i in range(9):
    n = i + 1
    tht_pad(stack, f"R{n}", XR, ypos(n), ZR[i], square=(n == 1))
    tht_pad(stack, f"L{n}", XL, ypos(n), ZL[i])       # link pins: joined by the long pins, no tracks
for k, n in enumerate(BOT):
    tht_pad(stack, n, XL + (k + 1) * P, Y9, n)

# ---- SD card contacts: bottom copper (the card's contact face), no paste. Contact centres from the
# card's centre line as in KiCad's SD socket footprints (TE 2041021, Kyocera 145638). SD contacts 8 and
# 9 (DAT1/DAT2) are not wired in the adapter and get no pad.
SDX = {"1": 6.875, "2": 4.375, "3": 1.875, "4": -0.625, "5": -3.125, "6": -5.625, "7": -8.055}
SDN = {"1": "GP12", "2": "GP10", "3": "GND", "4": "DC_3V3", "5": "GP9", "6": "GND", "7": "GP11"}
edge = new_fp("J2", "SD card edge to the DC SD adapter", WC / 2, 3.5)
for num, dx in SDX.items():
    p = pcbnew.PAD(edge)
    p.SetNumber(num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
    p.SetShape(pcbnew.PAD_SHAPE_RECT)
    p.SetSize(pcbnew.VECTOR2I(mm(1.6), mm(5.6)))
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.B_Cu); ls.AddLayer(pcbnew.B_Mask)
    p.SetLayerSet(ls)
    place_pad(edge, p, WC / 2 + dx, 3.6)
    p.SetNet(net(SDN[num]))

# ---- ribbon-cable row right behind the card end: the same signals in the contacts' order, so the card
# end is wired straight into it (fixed tracks) and can be cut off.
# power in the middle, ground between 3.3 V and 5 V
RIB = [("GP11", "RXD", "4"), ("GP9", "CTS", "7"), ("DC_3V3", "3V3", "10"), ("GND", "GND", "3"),
       ("VIN", "5V", "1"), ("GP10", "TXD", "5"), ("GP12", "RTS", "6")]
rib = new_fp("J5", "Dreamcast serial: ribbon cable / 5 V in", WC / 2, YH)
for k, (n, lab, dcpin) in enumerate(RIB):
    tht_pad(rib, str(k + 1), XL + k * P, YH, n, square=(k == 0))
# W5500 row: wire a module with six jumper wires (the common blue module has its own 3.3 V regulator
# and is fed from 5 V; its RST, INT and 3V3 pins stay unconnected: the firmware polls)
WROW = [("W_CS", "CS"), ("W_SCLK", "SCK"), ("W_MISO", "MISO"), ("GND", "GND"), ("+5V", "5V"), ("W_MOSI", "MOSI")]
wrow = new_fp("J6", "W5500 module (jumper wires)", WC / 2, YW)
for k, (n, lab) in enumerate(WROW):
    tht_pad(wrow, str(k + 1), XL + k * P, YW, n, square=(k == 0))
VIA_Y = 8.4
for num, dx in SDX.items():
    x = WC / 2 + dx
    track([(x, 6.3), (x, VIA_Y)], SDN[num], 0.5, pcbnew.B_Cu)
    if num != "6":                                   # contact 6 stays underneath (joined to 3 below)
        via(x, VIA_Y, SDN[num])
HOLE_FOR = {"7": 0, "5": 1, "4": 2, "3": 3, "2": 5, "1": 6}   # ribbon hole 5 (5 V) has no contact
for num, k in HOLE_FOR.items():
    x, xh = WC / 2 + SDX[num], XL + k * P
    track([(x, VIA_Y), (x, YH - 4.6), (xh, YH - 2.2), (xh, YH)], SDN[num], 0.5 if SDN[num] == "GND" else 0.3)
# the second ground contact (6) joins the first (3) underneath
x3, x6 = WC / 2 + SDX["3"], WC / 2 + SDX["6"]
track([(x3, VIA_Y), (x3, 10.4), (x6, 10.4), (x6, VIA_Y)], "GND", 0.5, pcbnew.B_Cu)

# ---- microSD socket: TF-01A push-push (Korean Hroparts, LCSC C91145, about 0.10-0.20 USD), on the
# bottom face, its opening at the left edge (its pins face the Zero's GP14-GP29 side). Land pattern as in Bus Pirate 5's TF-01A_C91145. Pins 1-8
# are the microSD contacts (DAT2, CS, CMD, VDD, CLK, VSS, DAT0, DAT1), 9 the card switch (not used),
# 11 the shell.
TF_PADS = [(str(i + 1), 2.185 - 1.1 * i, -14.85, 0.7, 1.3) for i in range(9)] + [
    ("11", -7.715, -14.0, 1.2, 1.4), ("11", 6.965, -14.0, 1.2, 1.4),
    ("11", -7.76, -4.3, 1.2, 2.0), ("11", 7.76, -4.3, 1.2, 2.0)]
TF_HOLES = [(-4.95, -4.01), (3.05, -4.01)]
TF_CY = -9.575                                   # midway between the contact row and the front pads
TF_NETS = {"1": "SD_DAT2", "2": "GP14", "3": "GP15", "4": "+3V3", "5": "GP26", "6": "GND", "7": "GP28",
           "8": "SD_DAT1", "11": "GND"}
SX = XB0 + 0.4 + (-4.3 - TF_CY + 1.0)            # front pads' outer edge 0.4 mm inside the left edge
SY = (YS0 + YS1) / 2


def tf_xy(lx, ly):
    # The part's own top view (looking at it, as mounted, from below the board) has its opening at +y.
    # Seen from the top through the board, that view is mirrored; turned so the opening points to -x.
    return SX - (ly - TF_CY), SY - lx


sd = new_fp("J4", "TF-01A microSD (LCSC C91145)", SX, SY)
for (num, lx, ly, w, h) in TF_PADS:
    x, y = tf_xy(lx, ly)
    smd_pad(sd, num, x, y, h, w, TF_NETS.get(num), back=True).SetLocalClearance(mm(0.15))  # maker's pattern
for (lx, ly) in TF_HOLES:
    npth(sd, *tf_xy(lx, ly), 1.0)
bx1 = SX + 6.6
for (a, b) in (((XB0 + 0.2, SY - 8.7), (bx1, SY - 8.7)), ((XB0 + 0.2, SY + 8.7), (bx1, SY + 8.7)),
               ((bx1, SY - 8.7), (bx1, SY + 8.7))):
    add_line(a[0], a[1], b[0], b[1], pcbnew.B_SilkS, 0.12)

# ---- the small parts, on the top face of the socket zone (no module above them)
R0805 = ("Resistor_SMD", "R_0805_2012Metric_Pad1.20x1.40mm_HandSolder")
C0805 = ("Capacitor_SMD", "C_0805_2012Metric_Pad1.18x1.45mm_HandSolder")
# 10k pull-ups to 3.3 V on the card: CS (card deselected while the RP2040 boots), MOSI, MISO, and the
# two data lines SPI mode leaves unused (they must not float)
PULL = [("R3", "GP14"), ("R4", "GP15"), ("R5", "GP28"), ("R6", "SD_DAT1"), ("R7", "SD_DAT2")]
for i, (ref, n) in enumerate(PULL):
    r = load(*R0805, ref, 10.6 + 2.2 * i, YS0 + 2.9, rot=90, value="10k")
    setnets(r, {"1": n, "2": "+3V3"})
c1 = load(*C0805, "C1", 12.8, YS0 + 8.3, rot=90, value="10uF")
setnets(c1, {"1": "+3V3", "2": "GND"})
c2 = load(*C0805, "C2", 15.0, YS0 + 8.3, rot=90, value="100nF")
setnets(c2, {"1": "+3V3", "2": "GND"})
# R1 1k: GP13 (the cable's RTS out) to GP9 (DC CTS / clock sense)
r1 = load(*R0805, "R1", 17.2, YS0 + 8.3, rot=90, value="1k")
setnets(r1, {"1": "GP9", "2": "GP13"})
# R2 10k: GP29 (power sense) to the Dreamcast's 3.3 V (SD contact 4)
r2 = load(*R0805, "R2", 19.4, YS0 + 8.3, rot=90, value="10k")
setnets(r2, {"1": "GP29", "2": "DC_3V3"})
# D1 B5819W (SOD-123, 1 A Schottky): 5 V in (ribbon hole 5) -> the stack's 5V; pad 1 = cathode (band)
d1 = load("Diode_SMD", "D_SOD-123", "D1", 16.6, YS0 + 13.6, rot=0, value="B5819W")
setnets(d1, {"1": "+5V", "2": "VIN"})


def keepout(x1, y1, x2, y2, layers, tracks=True, vias=True):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(tracks); z.SetDoNotAllowVias(vias); z.SetDoNotAllowPads(False)
    z.SetDoNotAllowCopperPour(True); z.SetDoNotAllowFootprints(True)
    ls = pcbnew.LSET()
    for l in layers:
        ls.AddLayer(l)
    z.SetLayerSet(ls)
    o = z.Outline(); o.NewOutline()
    for (x, y) in ((x1, y1), (x2, y1), (x2, y2), (x1, y2)):
        o.Append(mm(x), mm(y))
    board.Add(z)


# The card end: on the contact face only the contacts and their stubs, no vias where the slot's springs
# slide; the rest of the card end carries only the fixed tracks to the ribbon row (the router keeps out).
keepout(0, 0, WC, 5.6, [pcbnew.B_Cu], tracks=True, vias=True)
keepout(0, 0, WC, 7.4, [pcbnew.F_Cu], tracks=False, vias=True)
keepout(0, 11.2, WC, CARD, [pcbnew.B_Cu], tracks=True, vias=True)
# for the autorouter only (make.sh removes it after routing): nothing else on the card end's top face
ko = pcbnew.ZONE(board)
ko.SetIsRuleArea(True); ko.SetZoneName("router_only")
ko.SetDoNotAllowTracks(True); ko.SetDoNotAllowVias(True); ko.SetDoNotAllowPads(False)
ko.SetDoNotAllowCopperPour(True); ko.SetDoNotAllowFootprints(False)
_ls = pcbnew.LSET(); _ls.AddLayer(pcbnew.F_Cu); ko.SetLayerSet(_ls)
_o = ko.Outline(); _o.NewOutline()
for (x, y) in ((0, 9.4), (WC, 9.4), (WC, CARD + 0.6), (0, CARD + 0.6)):
    _o.Append(mm(x), mm(y))
board.Add(ko)
# (the router is kept off the card end by make.sh's check instead: a keepout here would also flag the
# fixed tracks)
# a 0.5 mm band along the body's edges: the autorouter does not keep the board-edge clearance itself
for (x1, x2) in ((XB0, XB0 + 0.55), (XB1 - 0.55, XB1)):
    keepout(x1, CARD, x2, L, [pcbnew.F_Cu, pcbnew.B_Cu], tracks=True, vias=True)
keepout(XB0, L - 0.55, XB1, L, [pcbnew.F_Cu, pcbnew.B_Cu], tracks=True, vias=True)

# ---- silkscreen -------------------------------------------------------------------------------------
add_text("DC-PicoLink", WC / 2, 12.6, 1.2, bold=True)
add_text("SD adapter end", WC / 2, 14.4, 0.8)
add_text("1.6 mm ENIG", WC / 2, 15.9, 0.8)
add_text("cut here for a ribbon cable", WC / 2, CARD - 1.2, 0.7)
add_line(0.6, CARD - 0.4, WC - 0.6, CARD - 0.4, pcbnew.F_SilkS, 0.15)
for k, (n, lab, dcpin) in enumerate(RIB):
    add_text(dcpin, XL + k * P, YH - 1.45, 0.6, layer=pcbnew.B_SilkS, mirror=True)
add_text("DC serial pin no.", WC / 2, YH - 2.6, 0.6, layer=pcbnew.B_SilkS, mirror=True)
# names between the rows: Dreamcast row above, W5500 row below; on the bottom face the W5500 names
for k, ((n, lab, dcpin), wl) in enumerate(zip(RIB, [w[1] for w in WROW] + [""])):
    add_text(lab, XL + k * P, YH + P / 2, 0.6)
    if wl:
        add_text(wl, XL + k * P, YW + 1.45, 0.6, layer=pcbnew.B_SilkS, mirror=True)
for k, (n, wl) in enumerate(WROW):                   # W5500 names under its row on the top face too
    add_text(wl, XL + k * P, YW + 1.45, 0.6)
add_text("R3-R7 10k", 15.0, YS0 + 0.55, 0.6)
add_text("C1  C2  R1  R2", 16.1, YS0 + 10.7, 0.6)
add_text("D1", 13.4, YS0 + 13.6, 0.7)
for k, line in enumerate(("Zero", "face up", "", "z cut under", "e ESP32 pin,", "cut on top")):
    if line:
        add_text(line, WC / 2, ypos(8) + 1.27 + k * 1.6, 0.8 if k < 2 else 0.6, bold=(k == 0))
add_text("USB end", WC / 2, L - 1.4, 0.7)
for i in range(8):                                   # position 9 is the bottom row, numbered below
    add_text(TOP_LABEL.get(ZR[i], ZR[i].replace("+", "")), XR - 2.9, ypos(i + 1), 0.6)
    add_text(TOP_LABEL.get(ZL[i], ZL[i].split("_")[0]), XL + 2.9, ypos(i + 1), 0.6)
for k, n in enumerate(["GP8"] + BOT + ["GP14"]):
    add_text(n[2:], XL + k * P, Y9 - 1.45, 0.6)
# bottom face (mirrored text)
for k, line in enumerate(("ESP32", "face down")):
    add_text(line, WC / 2, ypos(7) + k * 1.6, 0.8, layer=pcbnew.B_SilkS, mirror=True, bold=(k == 0))
add_text("microSD", SX + 1.2, SY, 0.9, layer=pcbnew.B_SilkS, mirror=True, rot=90)
add_text("< card in from here", SX - 2.4, SY, 0.6, layer=pcbnew.B_SilkS, mirror=True, rot=90)
for i in range(8):
    add_text(ESP_R[i], XR - 3.0, ypos(i + 1), 0.6, layer=pcbnew.B_SilkS, mirror=True)
    add_text(ESP_L[i], XL + 3.0, ypos(i + 1), 0.6, layer=pcbnew.B_SilkS, mirror=True)
for num, dx in SDX.items():
    add_text(num, WC / 2 + dx + 0.9, 7.2, 0.6, layer=pcbnew.B_SilkS, mirror=True)
# a small mark beside every hole that is not a full pin: z = cut under, x = no pin, o = optional
for fits, x in ((FIT_R, XR), (FIT_L, XL)):
    for i, f in enumerate(fits[:8]):                 # position 9 is marked with the bottom row
        if f in "ZE":
            xm = x - 1.27 if x > WC / 2 else x + 1.27
            for layer, mir in ((pcbnew.F_SilkS, False), (pcbnew.B_SilkS, True)):
                add_text({"Z": "z", "E": "e"}[f], xm, ypos(i + 1), 0.6, layer=layer, mirror=mir)
for k in range(1, 7):                                # GP9-GP14 of the bottom row: all Zero-only
    for layer, mir in ((pcbnew.F_SilkS, False), (pcbnew.B_SilkS, True)):
        add_text("z", XL + k * P - 1.1, Y9 + 1.25, 0.6, layer=layer, mirror=mir)

board.Save(OUT)
print("wrote", OUT, "nets:", len(nets))
