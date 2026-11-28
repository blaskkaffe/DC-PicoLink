#!/bin/sh
# Build the stack PCB: generate, autoroute (Freerouting 1.9, headless via xvfb-run), import, DRC.
# Needs KiCad 7 (pcbnew Python), Java 17+, xvfb-run, FREEROUTING=/path/to/freerouting-1.9.0.jar.
set -e
cd "$(dirname "$0")"
B=${1:-build}
mkdir -p "$B"
python3 gen_board.py "$B/unrouted.kicad_pcb"
python3 -c "import sys,pcbnew; pcbnew.ExportSpecctraDSN(pcbnew.LoadBoard(sys.argv[1]), sys.argv[2])" "$B/unrouted.kicad_pcb" "$B/board.dsn"
# power nets into the Power class (0.5 mm), the rest stays 0.3 mm
python3 - "$B/board.dsn" <<'PY'
import re, sys
f = sys.argv[1]; s = open(f).read()
power = ["+3V3", "+5V", "GND", "VIN"]
for n in power:
    s = re.sub(r'(\(class kicad_default ""[^(]*?)\s' + re.escape(n) + r'(?=\s)', r'\1', s, count=1)
s = s.replace("(class Power\n", "(class Power " + " ".join(power) + "\n", 1)
s = s.replace("(width 600)\n        (clearance 250.1)", "(width 500)\n        (clearance 200.1)")
open(f, "w").write(s)
PY
rm -f "$B/board.ses"
xvfb-run -a java -jar "${FREEROUTING:?set FREEROUTING to freerouting-1.9.0.jar}" -de "$B/board.dsn" -do "$B/board.ses" -mp 40 >"$B/freerouting.log" 2>&1
python3 ses_import.py "$B/unrouted.kicad_pcb" "$B/board.ses" dc-picolink-stack.kicad_pcb
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
