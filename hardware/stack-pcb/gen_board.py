#!/usr/bin/env python3
"""Generate the DC-PicoLink stack interposer PCB (KiCad 7, pcbnew Python API).

The board sits between the RP2040-Zero (on top, face up) and the ESP32-S3 SuperMini (underneath,
face down), both USB ports at the board's square end. Its other end is shaped like a full-size
SD card and plugs into the slot of the DC SD adapter on the Dreamcast's serial port.

Run:  python3 gen_board.py out.kicad_pcb       (then route: see make.sh)
Coordinates in mm, KiCad's y axis points down. Viewed from the top (the Zero's side, the card's
label side): the SD contact edge is at the top (y = 0), the USB end at the bottom.
"""
import sys
import pcbnew

FP = "/usr/share/kicad/footprints"
OUT = sys.argv[1] if len(sys.argv) > 1 else "dc-picolink-stack.kicad_pcb"

W = 24.0            # SD card width
CARD = 34.0         # card part: 32 mm card length + 2 mm for the slot's mouth (no parts here)
P = 2.54
XL, XR = W / 2 - 3 * P, W / 2 + 3 * P      # the two 9-pin rows, 15.24 mm apart (4.38 / 19.62)
Y10 = 43.6          # the row of holes beside the Zero's bottom-edge pads (GP9-GP13)
L = Y10 + P * 9 + 1.3                       # board end at the USB side: 1.3 mm past position 1, clear of the USB plugs

def mm(v):
    return pcbnew.FromMM(v)

def pt(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))

board = pcbnew.BOARD()
ds = board.GetDesignSettings()
ds.SetBoardThickness(mm(1.6))
ds.m_TrackMinWidth = mm(0.2)
ds.m_MinClearance = mm(0.2)
ds.m_ViasMinSize = mm(0.6)
ds.m_MinThroughDrill = mm(0.3)
ds.m_HoleToHoleMin = mm(0.25)
ds.m_CopperEdgeClearance = mm(0.3)

nets = {}
def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        nets[name] = n
    return nets[name]

# Net classes: power wider
nc = ds.m_NetSettings
pwr = pcbnew.NETCLASS("Power")
pwr.SetTrackWidth(mm(0.6)); pwr.SetClearance(mm(0.25)); pwr.SetViaDiameter(mm(0.8)); pwr.SetViaDrill(mm(0.4))
nc.m_NetClasses["Power"] = pwr
dflt = nc.m_DefaultNetClass
dflt.SetTrackWidth(mm(0.3)); dflt.SetClearance(mm(0.2)); dflt.SetViaDiameter(mm(0.7)); dflt.SetViaDrill(mm(0.35))
POWER_NETS = ("+5V", "GND", "+3V3", "VIN")     # set to the Power class after the nets exist (end of file)

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
    return t

def add_line(x1, y1, x2, y2, layer=pcbnew.Edge_Cuts, w=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pt(x1, y1)); s.SetEnd(pt(x2, y2))
    s.SetLayer(layer); s.SetWidth(mm(w))
    board.Add(s)

def add_rect(x1, y1, x2, y2, layer, w=0.12):
    for a, b in (((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))):
        add_line(a[0], a[1], b[0], b[1], layer, w)

# ---- outline: SD card end with its corner chamfer on the pin-9 side (top right, seen from the label side)
CH = 4.0
outline = [(0, 0), (W - CH, 0), (W, CH), (W, L), (0, L), (0, 0)]
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
    fp.Reference().SetLayer(pcbnew.F_Fab)           # labels on the silkscreen are placed by hand below
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

def tht_pad(fp, num, x, y, netname, drill=1.0, size=1.7, square=False):
    p = pcbnew.PAD(fp)
    p.SetNumber(num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    p.SetShape(pcbnew.PAD_SHAPE_RECT if square else pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    p.SetDrillSize(pcbnew.VECTOR2I(mm(drill), mm(drill)))
    p.SetLayerSet(pcbnew.PAD.PTHMask())
    fp.Add(p)
    o = fp.GetPosition()
    p.SetPos0(pcbnew.VECTOR2I(mm(x) - o.x, mm(y) - o.y)); p.SetPosition(pt(x, y))
    if netname:
        p.SetNet(net(netname))
    return p

# Stack holes. Position n counts from the USB end (n = 1 at the bottom of this view).
# Seen from the top with USB at the bottom, the Zero's 5V..GP14 column is on the right.
ZR = ["+5V", "GND", "+3V3", "GP29", "GP28", "GP27", "GP26", "GP15", "GP14"]           # Zero pads 1-9
ZL = ["GP0", "GP1", "GP2", "GP3_HS", "GP4_MISO", "GP5_CS", "GP6_SCK", "GP7_MOSI", "GP8_DR"]  # pads 23-15
# how each position is fitted: F = one long pin through all three boards, Z = Zero to this board only
# (cut off underneath before the ESP32 goes on), O = optional (ESP32 console), - = no pin
FIT_R = ["F", "F", "Z", "Z", "Z", "-", "Z", "Z", "Z"]
FIT_L = ["O", "O", "-", "F", "F", "F", "F", "F", "F"]
ESP_R = ["5V", "GND", "3V3", "IO13", "IO12", "IO11", "IO10", "IO9", "IO8"]
ESP_L = ["IO43", "IO44", "IO1", "IO2", "IO3", "IO4", "IO5", "IO6", "IO7"]

stack = new_fp("J1", "Stack: RP2040-Zero above, ESP32-S3 SuperMini below", W / 2, L - 12)
def ypos(n):                                  # position 9 is one pitch from the bottom-pad row, 1 at the USB end
    return Y10 + P * (10 - n)
for i in range(9):
    n = i + 1
    y = ypos(n)
    tht_pad(stack, f"R{n}", XR, y, ZR[i], square=(n == 1))
    tht_pad(stack, f"L{n}", XL, y, ZL[i])
# bottom-edge pads of the Zero (seen from the top, USB at the bottom: GP9 on the left ... GP13 on the right)
BOT = {"GP9": XL + 1 * P, "GP10": XL + 2 * P, "GP11": XL + 3 * P, "GP12": XL + 4 * P, "GP13": XL + 5 * P}
for k, x in BOT.items():
    tht_pad(stack, k, x, Y10, k)

# the link pins (left row) are joined by the long pins themselves: their nets have no other pad, no tracks
# SD contact edge: pads on the bottom copper (the card's contact face), no paste, no mask.
# Contact centres from the card's centre line (as in KiCad's SD socket footprints, TE 2041021).
SDX = {"1": 6.875, "2": 4.375, "3": 1.875, "4": -0.625, "5": -3.125, "6": -5.625, "7": -8.055}
SDN = {"1": "GP12", "2": "GP10", "3": "GND", "4": "DC_3V3", "5": "GP9", "6": "GND", "7": "GP11"}
edge = new_fp("J2", "SD card edge to the DC SD adapter", W / 2, 3.5)
for num, dx in SDX.items():
    p = pcbnew.PAD(edge)
    p.SetNumber(num)
    p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
    p.SetShape(pcbnew.PAD_SHAPE_RECT)
    p.SetSize(pcbnew.VECTOR2I(mm(1.6), mm(5.6)))
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.B_Cu); ls.AddLayer(pcbnew.B_Mask)
    p.SetLayerSet(ls)
    edge.Add(p)
    o = edge.GetPosition()
    p.SetPos0(pcbnew.VECTOR2I(mm(W / 2 + dx) - o.x, mm(0.8 + 2.8) - o.y)); p.SetPosition(pt(W / 2 + dx, 0.8 + 2.8))
    p.SetNet(net(SDN[num]))

# ---- under the ESP32 (bottom side, between the two pin rows): the slot card's socket and its parts.
# Everything here is at most 1.42 mm high: it fits in the header spacer's 2.5 mm gap. The card goes in
# from the USB end, between this board and the ESP32's back.
sd = load("Connector_Card", "microSD_HC_Molex_104031-0811", "J4", W / 2, L - 6.55, rot=180, back=True,
          value="Molex 104031-0811 microSD")
setnets(sd, {"1": "SD_DAT2", "2": "GP14", "3": "GP15", "4": "+3V3", "5": "GP26", "6": "GND", "7": "GP28",
             "8": "SD_DAT1", "11": "GND"})
R0805 = ("Resistor_SMD", "R_0805_2012Metric_Pad1.20x1.40mm_HandSolder")
C0805 = ("Capacitor_SMD", "C_0805_2012Metric_Pad1.18x1.45mm_HandSolder")
# 10k pull-ups to 3.3 V: CS (card deselected while the RP2040 boots), MOSI, MISO, and the two data
# lines SPI mode leaves unused (they must not float)
PULL = [("R3", "GP14"), ("R4", "GP15"), ("R5", "GP28"), ("R6", "SD_DAT1"), ("R7", "SD_DAT2")]
for i, (ref, n) in enumerate(PULL):
    r = load(*R0805, ref, 7.2 + 2.4 * i, Y10 + 3.6, rot=90, value="10k", back=True)
    setnets(r, {"1": n, "2": "+3V3"})
c1 = load(*C0805, "C1", 9.6, Y10 + 8.1, rot=90, value="10uF", back=True)
setnets(c1, {"1": "+3V3", "2": "GND"})
c2 = load(*C0805, "C2", 14.4, Y10 + 8.1, rot=90, value="100nF", back=True)
setnets(c2, {"1": "+3V3", "2": "GND"})

# ---- between the card end and the stack (top side) ----------------------------------------------------
# 5 V in (from the Dreamcast's serial pin 1 or a charger): JST XH 2-pin at the left edge
j3 = load("Connector_JST", "JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical", "J3", 3.6, CARD + 2.6, rot=270,
          value="5V in")
setnets(j3, {"1": "VIN", "2": "GND"})
# SS14 (SMA): anode on VIN, cathode (pad 1, the band) toward the stack's 5V
d1 = load("Diode_SMD", "D_SMA_Handsoldering", "D1", 11.0, CARD + 2.3, rot=180, value="SS14")
setnets(d1, {"1": "+5V", "2": "VIN"})
# R2 10k: GP29 (power sense) to the Dreamcast's 3.3 V on SD contact 4
r2 = load(*R0805, "R2", 18.2, CARD + 2.3, rot=0, value="10k")
setnets(r2, {"1": "GP29", "2": "DC_3V3"})
# R1 1k: GP13 (the cable's RTS out) to GP9 (DC CTS / clock sense), right by their holes
r1 = load(*R0805, "R1", W / 2, Y10 - 3.3, rot=0, value="1k")
setnets(r1, {"1": "GP9", "2": "GP13"})

# ---- keep the card part free of parts on both sides; no copper but the contacts on the bottom
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
# bottom copper in the card part: only the contacts and the stubs leaving them
# Each contact gets a fixed stub straight back to its own via; everything else runs on the top copper.
# No tracks between or in front of the contacts (the slot's springs slide there), no vias near them.
VIA_Y = 8.4
for num, dx in SDX.items():
    x = W / 2 + dx
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pt(x, 6.3)); t.SetEnd(pt(x, VIA_Y)); t.SetWidth(mm(0.5)); t.SetLayer(pcbnew.B_Cu)
    t.SetNet(net(SDN[num])); board.Add(t)
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pt(x, VIA_Y)); v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(net(SDN[num])); board.Add(v)
keepout(0, 0, W, 5.6, [pcbnew.B_Cu], tracks=True, vias=True)
keepout(0, 0, W, 7.4, [pcbnew.F_Cu], tracks=False, vias=True)
keepout(0, 9.0, W, CARD, [pcbnew.B_Cu], tracks=True, vias=False)

# ---- silkscreen -----------------------------------------------------------------------------------
add_text("DC-PicoLink stack", W / 2, 13.5, 1.2, bold=True)
add_text("SD adapter slot end", W / 2, 15.6, 0.8)
add_text("this side up = card label side", W / 2, 17.3, 0.8)
add_text("ENIG, 1.6 mm", W / 2, 19.0, 0.8)
add_text("RP2040-Zero on top, face up", W / 2, 22.6, 0.9, bold=True)
add_text("ESP32-S3 SuperMini under, face down", W / 2, 24.2, 0.8)
add_text("both USB ports at the square end", W / 2, 25.6, 0.8)
add_text("USB", W / 2, L - 2.0, 0.9, bold=True)
# fitting marks beside each stack hole: F full pin, Z Zero only (cut under), O optional, - none
for i in range(9):
    n = i + 1
    y = ypos(n)
    add_text(FIT_R[i], XR + 1.9, y, 0.8, bold=True)
    add_text(FIT_L[i], XL - 1.9, y, 0.8, bold=True)
    add_text(ZR[i].replace("+", ""), XR - 3.0, y, 0.8)
    add_text(ZL[i].split("_")[0], XL + 2.9, y, 0.8)
for k, x in BOT.items():
    add_text(k[2:], x, Y10 - 1.55, 0.8)
add_text("F = one pin through all 3 boards", W / 2, 27.6, 0.8)
add_text("Z = Zero to this board, cut below", W / 2, 28.9, 0.8)
add_text("O = optional   - = no pin", W / 2, 30.2, 0.8)
add_text("5V IN 1:+", 3.0, 32.6, 0.8)
add_text("SS14", 11.0, 32.6, 0.8)
add_text("R2 10k", 18.2, 32.6, 0.8)
add_text("R1 1k", W / 2 + 4.6, Y10 - 3.3, 0.8)
# bottom: the ESP32's pin names (seen from below, mirrored text)
add_text("ESP32 side", W / 2, 22.0, 1.0, layer=pcbnew.B_SilkS, mirror=True, bold=True)
add_text("Kapton on the ESP32's back:", W / 2, 24.0, 0.8, layer=pcbnew.B_SilkS, mirror=True)
add_text("the socket and parts are under it", W / 2, 25.4, 0.8, layer=pcbnew.B_SilkS, mirror=True)
add_text("microSD in from the USB end", W / 2, 26.8, 0.8, layer=pcbnew.B_SilkS, mirror=True)
for i in range(9):                                   # the same fitting marks, seen from below
    y = ypos(i + 1)
    add_text(FIT_R[i], XR + 1.9, y, 0.8, layer=pcbnew.B_SilkS, mirror=True, bold=True)
    add_text(FIT_L[i], XL - 1.9, y, 0.8, layer=pcbnew.B_SilkS, mirror=True, bold=True)
add_text("R3-R7: 10k pull-ups (left to right)", W / 2, 28.2, 0.8, layer=pcbnew.B_SilkS, mirror=True)
add_text("C1 10u", 6.4, Y10 + 8.1, 0.8, layer=pcbnew.B_SilkS, mirror=True, rot=90)
add_text("C2 100n", 17.6, Y10 + 8.1, 0.8, layer=pcbnew.B_SilkS, mirror=True, rot=90)
for num, dx in SDX.items():
    add_text(num, W / 2 + dx + 1.0, 9.8, 0.8, layer=pcbnew.B_SilkS, mirror=True)

for n in POWER_NETS:
    if n in nets:
        nets[n].SetNetClass(pwr)
board.Save(OUT)
print("wrote", OUT, "nets:", len(nets))
