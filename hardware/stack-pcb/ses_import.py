#!/usr/bin/env python3
"""Put Freerouting's routes (a Specctra .ses file) onto a KiCad 7 board without the GUI.

pcbnew's own ImportSpecctraSES only works inside the editor, so this reads the session's
(wire (path layer width x y ...)) and (via padstack x y) entries itself.
Usage: ses_import.py in.kicad_pcb board.ses out.kicad_pcb
"""
import re
import sys
import pcbnew


def tokens(text):
    return re.findall(r'\(|\)|"[^"]*"|[^\s()]+', text)


def parse(toks):
    stack, cur = [], []
    for t in toks:
        if t == "(":
            stack.append(cur); cur = []
        elif t == ")":
            done = cur; cur = stack.pop(); cur.append(done)
        else:
            cur.append(t.strip('"'))
    return cur[0]


def find(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def main(inp, ses, out):
    tree = parse(tokens(open(ses).read()))
    res = find(find(tree, "placement")[0], "resolution")[0]
    unit = {"um": 1000.0, "mm": 1e6, "mil": 25400.0}[res[1]] / float(res[2])   # nm per session unit
    b = pcbnew.LoadBoard(inp)
    layers = {b.GetLayerName(l): l for l in (pcbnew.F_Cu, pcbnew.B_Cu)}
    have = {(v.GetPosition().x, v.GetPosition().y) for v in b.GetTracks() if v.Type() == pcbnew.PCB_VIA_T}
    have_t = {(t.GetStart().x, t.GetStart().y, t.GetEnd().x, t.GetEnd().y) for t in b.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T}
    routes = find(tree, "routes")[0]
    netout = find(routes, "network_out")[0]
    nw = nv = 0
    for n in find(netout, "net"):
        net = b.FindNet(n[1])
        if net is None:
            print("no net", n[1]); continue
        for w in find(n, "wire"):
            path = find(w, "path")[0]
            layer = layers[path[1]]
            width = int(float(path[2]) * unit)
            xy = [float(v) for v in path[3:]]
            pts = [pcbnew.VECTOR2I(int(xy[i] * unit), int(-xy[i + 1] * unit)) for i in range(0, len(xy), 2)]
            for a, c in zip(pts, pts[1:]):
                if (a.x, a.y, c.x, c.y) in have_t or (c.x, c.y, a.x, a.y) in have_t:
                    continue                      # one of the board's own fixed tracks
                t = pcbnew.PCB_TRACK(b)
                t.SetStart(a); t.SetEnd(c); t.SetWidth(width); t.SetLayer(layer); t.SetNet(net)
                b.Add(t); nw += 1
        for v in find(n, "via"):
            x, y = float(v[2]), float(v[3])
            if (int(x * unit), int(-y * unit)) in have:
                continue                          # one of the board's own fixed vias
            via = pcbnew.PCB_VIA(b)
            via.SetPosition(pcbnew.VECTOR2I(int(x * unit), int(-y * unit)))
            m = re.search(r'(\d+(?:\.\d+)?):(\d+(?:\.\d+)?)_um', v[1])
            dia, drill = (float(m.group(1)), float(m.group(2))) if m else (700, 350)
            via.SetWidth(pcbnew.FromMM(dia / 1000)); via.SetDrill(pcbnew.FromMM(drill / 1000))
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); via.SetNet(net)
            b.Add(via); nv += 1
    b.Save(out)
    print(f"{nw} track segments, {nv} vias -> {out}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
