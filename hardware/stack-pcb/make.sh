#!/bin/sh
# Build the stack PCB: generate, autoroute (Freerouting 1.9, headless via xvfb-run), import, DRC.
# Needs KiCad 7 (pcbnew Python), Java 17+, xvfb-run, FREEROUTING=/path/to/freerouting-1.9.0.jar.
set -e
cd "$(dirname "$0")"
B=${1:-build}
mkdir -p "$B"
python3 gen_board.py "$B/unrouted.kicad_pcb"
python3 -c "import sys,pcbnew; pcbnew.ExportSpecctraDSN(pcbnew.LoadBoard(sys.argv[1]), sys.argv[2])" "$B/unrouted.kicad_pcb" "$B/board.dsn"
# the 5 V nets in their own class for the router: 0.4 mm tracks, 0.8 mm vias (the rest 0.25 mm)
python3 - "$B/board.dsn" <<'PY'
import re, sys
f = sys.argv[1]; s = open(f).read()
power = ["+5V", "VIN"]          # the 5 V path from the ribbon hole (the stack can draw ~1 A)
m = re.search(r'\(class kicad_default ""(.*?)\(circuit', s, re.S)
names = m.group(1).split()
for n in power:
    if n in names:
        names.remove(n)
s = s[:m.start(1)] + " " + " ".join(names) + "\n      " + s[m.end(1):]
cls = ('    (class Power ' + " ".join(power) + '\n      (circuit\n        (use_via Via[0-1]_800:400_um)\n      )\n'
       '      (rule\n        (width 400)\n        (clearance 200.1)\n      )\n    )\n')
i = s.index("    (class kicad_default")
s = s[:i] + cls + s[i:]
open(f, "w").write(s)
PY
rm -f "$B/board.ses"
timeout 600 xvfb-run -a java -jar "${FREEROUTING:?set FREEROUTING to freerouting-1.9.0.jar}" -de "$B/board.dsn" -do "$B/board.ses" -mp 60 >"$B/freerouting.log" 2>&1
python3 ses_import.py "$B/unrouted.kicad_pcb" "$B/board.ses" dc-picolink-stack.kicad_pcb
# board rules live in the project file in KiCad 7: 0.15 mm minimum (the socket's own pattern), 0.6 mm text
python3 - <<'PY'
import json, os
f = "dc-picolink-stack.kicad_pro"
d = json.load(open(f)) if os.path.exists(f) else {"board": {"design_settings": {"rules": {}}}, "meta": {"filename": f, "version": 1}}
r = d.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
r.update({"min_clearance": 0.15, "min_text_height": 0.6, "min_copper_edge_clearance": 0.3,
          "min_track_width": 0.2, "min_via_diameter": 0.6, "min_through_hole_diameter": 0.3})
json.dump(d, open(f, "w"), indent=2)
PY
# nothing routed on the card end (it may be cut off for a ribbon cable): only the fixed tracks
python3 - "$B" <<'PY'
import pcbnew, sys
b = pcbnew.LoadBoard("dc-picolink-stack.kicad_pcb")
fixed = pcbnew.LoadBoard(sys.argv[1] + "/unrouted.kicad_pcb")
key = lambda t: (t.Type(), t.GetStart().x, t.GetStart().y, t.GetEnd().x, t.GetEnd().y)
own = {key(t) for t in fixed.GetTracks()}
card = pcbnew.FromMM(22.0)
bad = [t for t in b.GetTracks() if key(t) not in own and min(t.GetStart().y, t.GetEnd().y) < card]
if bad:
    print(f"routed {len(bad)} items on the card end: change the layout"); sys.exit(1)
PY
python3 -c "import sys,pcbnew; pcbnew.WriteDRCReport(pcbnew.LoadBoard(sys.argv[1]), sys.argv[2], pcbnew.EDA_UNITS_MILLIMETRES, True)" dc-picolink-stack.kicad_pcb "$B/drc.rpt"
grep -E '^\*\* Found' "$B/drc.rpt"
# fab files: Gerbers + drill (zip for JLCPCB/PCBWay etc.), pictures
F="$B/fab"; rm -rf "$F"; mkdir -p "$F"
kicad-cli pcb export gerbers -o "$F/" --layers F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,F.Paste,B.Paste,Edge.Cuts --no-x2 --subtract-soldermask dc-picolink-stack.kicad_pcb >/dev/null
kicad-cli pcb export drill -o "$F/" --format excellon --excellon-separate-th --generate-map --map-format gerberx2 dc-picolink-stack.kicad_pcb >/dev/null
(cd "$F" && zip -q ../dc-picolink-stack-gerbers.zip *)
for side in F B; do
  m=""; [ $side = B ] && m=--mirror
  kicad-cli pcb export svg --layers $side.Cu,$side.Mask,$side.SilkS,Edge.Cuts --exclude-drawing-sheet --page-size-mode 2 $m -o "$B/$side.svg" dc-picolink-stack.kicad_pcb >/dev/null
done
echo "fab: $B/dc-picolink-stack-gerbers.zip"
