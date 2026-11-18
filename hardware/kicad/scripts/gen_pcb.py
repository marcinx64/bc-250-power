# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate bc250-power-controller.kicad_pcb (rev A) with the pcbnew API.

  flatpak run --command=kicad-cli org.kicad.KiCad sch export netlist --format kicadsexpr \
      --output hardware/kicad/bc250-power-controller.net hardware/kicad/bc250-power-controller.kicad_sch
  sed -i -E 's|\(source "[^"]*/([^/"]+)"\)|(source "\1")|' hardware/kicad/bc250-power-controller.net
  flatpak run --command=python3 org.kicad.KiCad hardware/kicad/scripts/gen_pcb.py

The sed line keeps the absolute path of your checkout out of the netlist.

Placement and routing are explicit (no autorouter): see the PLACE and ROUTES
tables. Net names and pad-to-net assignment come from the schematic netlist.
"""
import math
import os
import sys

import pcbnew
from pcbnew import FromMM, VECTOR2I

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design as D  # noqa: E402
from sexpr import find, find_all, parse, val  # noqa: E402

KICAD = os.path.dirname(HERE)
FPLIB = "/app/extensions/Library/footprints/"
BOARD_W, BOARD_H, CORNER = 60.0, 40.0, 3.0
SIG, PWR = 0.3, 0.8


def mm(x, y):
    return VECTOR2I(FromMM(x), FromMM(y))


# ---------------------------------------------------------------- netlist
def read_netlist(path):
    t = parse(open(path).read())[0]
    nets = find(t, "nets")
    pinnet = {}
    global COMP_UUID
    COMP_UUID = {}
    for c in find_all(find(t, "components"), "comp"):
        COMP_UUID[val(find(c, "ref")[1])] = val(find(c, "tstamps")[1])
    for n in find_all(nets, "net"):
        name = val(find(n, "name")[1])
        for node in find_all(n, "node"):
            pinnet[(val(find(node, "ref")[1]), val(find(node, "pin")[1]))] = name
    return pinnet


# ---------------------------------------------------------------- placement
# ref: (pad id used as anchor, (x, y) of that pad, rotation, reference text offset)
XIAO_P1 = (58.0, 14.38)
PLACE = {
    "U3": ("1", XIAO_P1, 270, (50.0, 21.0)),
    "BZ1": ("1", (18.8, 7.6), 180, None),
    "R1": ("1", (7.0, 13.7), 270, (4.9, 17.5)),
    "R2": ("1", (29.5, 13.7), 270, (27.5, 17.5)),
    "R3": ("1", (34.0, 13.7), 270, (31.9, 17.5)),
    "U1": ("1", (10.0, 31.42), 90, (14.5, 21.7)),
    "U2": ("1", (33.5, 23.8), 270, (22.0, 24.2)),
    "R4": ("1", (23.0, 31.42), 90, (21.8, 27.0)),
    "JP1": ("1", (20.6, 30.8), 90, None),
    "J1": ("1", (9.5, 35.6), 0, (8.0, 32.3)),
    "J2": ("1", (20.5, 35.6), 0, (19.0, 32.3)),
    "J3": ("1", (42.9, 35.6), 0, (41.4, 32.3)),
    "D1": ("1", (37.3, 37.8), 180, (35.6, 39.3)),
    "JP2": ("1", (35.35, 34.9), 0, (36.0, 33.1)),
    "MH1": (None, (3.0, 3.0), 0, None),
    "MH2": (None, (57.0, 3.0), 0, None),
    "MH3": (None, (3.0, 37.0), 0, None),
    "MH4": (None, (57.0, 37.0), 0, None),
}


def rot_pt(x, y, a):
    a = math.radians(a)
    c, s = round(math.cos(a)), round(math.sin(a))
    return x * c + y * s, -x * s + y * c


def load_fp(fpid):
    lib, name = fpid.split(":")
    path = KICAD + "/bc250.pretty" if lib == "bc250" else FPLIB + lib + ".pretty"
    fp = pcbnew.FootprintLoad(path, name)
    assert fp is not None, fpid
    return fp


def pad_pos(fp, num):
    for p in fp.Pads():
        if p.GetNumber() == num:
            return p.GetPosition()
    raise KeyError(num)


def build():
    pinnet = read_netlist(os.path.join(KICAD, D.PROJECT + ".net"))
    board = pcbnew.BOARD()
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)

    netinfo = {}

    def net(name):
        if name not in netinfo:
            n = pcbnew.NETINFO_ITEM(board, name)
            board.Add(n)
            netinfo[name] = n
        return netinfo[name]

    net("")  # keep code 0 for "no net"
    fps = {}
    for p in D.PARTS:
        ref = p["ref"]
        fp = load_fp(p["fp"])
        fp.SetReference(ref)
        fp.SetValue(p["value"])
        fp.SetFPID(pcbnew.LIB_ID(*p["fp"].split(":")))
        fp.SetPath(pcbnew.KIID_PATH("/" + COMP_UUID[ref]))
        fp.SetField("Description", p["desc"])
        anchor, pos, rot, reftxt = PLACE[ref]
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(mm(0, 0))
        if anchor:
            ap = pad_pos(fp, anchor)
            fp.SetPosition(VECTOR2I(FromMM(pos[0]) - ap.x, FromMM(pos[1]) - ap.y))
        else:
            fp.SetPosition(mm(*pos))
        for pad in fp.Pads():
            n = pinnet.get((ref, pad.GetNumber()))
            if n:
                pad.SetNet(net(n))
        if p.get("dnp"):
            fp.SetDNP(True)
        board.Add(fp)
        fps[ref] = fp
        # reference text on the silkscreen, small
        r = fp.Reference()
        r.SetTextSize(VECTOR2I(FromMM(0.8), FromMM(0.8)))
        r.SetTextThickness(FromMM(0.12))
        r.SetLayer(pcbnew.F_SilkS)
        r.SetVisible(not ref.startswith("MH"))
        if reftxt:
            r.SetPosition(mm(*reftxt))
        if ref.startswith("R"):
            r.SetTextAngleDegrees(90)
        if ref == "JP1":
            r.SetVisible(False)
        # drop the K/A text marks of the diode (the cathode band marks the polarity)
        for g in list(fp.GraphicalItems()):
            if isinstance(g, pcbnew.PCB_TEXT) and g.GetText() in ("K", "A"):
                fp.Remove(g)
        v = fp.Value()
        v.SetVisible(False)

    # ------------------------------------------------------------ outline
    def seg(a, b, layer=pcbnew.Edge_Cuts, w=0.1):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(mm(*a))
        s.SetEnd(mm(*b))
        s.SetLayer(layer)
        s.SetWidth(FromMM(w))
        board.Add(s)

    def arc(c, start, end, mid):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(mm(*start), mm(*mid), mm(*end))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(FromMM(0.1))
        board.Add(s)

    W, H, R = BOARD_W, BOARD_H, CORNER
    k = R * (1 - math.sqrt(0.5))
    seg((R, 0), (W - R, 0))
    seg((W, R), (W, H - R))
    seg((W - R, H), (R, H))
    seg((0, H - R), (0, R))
    arc((W - R, R), (W - R, 0), (W, R), (W - k, k))
    arc((W - R, H - R), (W, H - R), (W - R, H), (W - k, H - k))
    arc((R, H - R), (R, H), (0, H - R), (k, H - k))
    arc((R, R), (0, R), (R, 0), (k, k))

    # ------------------------------------------------------------ routing
    tracks = []   # (netname, layer, width, [(x,y),...])

    def route(netname, layer, width, pts):
        L = pcbnew.F_Cu if layer == "F" else pcbnew.B_Cu
        for a, b in zip(pts[:-1], pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(mm(*a))
            t.SetEnd(mm(*b))
            t.SetWidth(FromMM(width))
            t.SetLayer(L)
            t.SetNet(net(netname))
            board.Add(t)
        tracks.append((netname, layer, width, pts))

    def P(ref, num):
        p = pad_pos(fps[ref], num)
        return (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))

    def nm(ref, num):
        return pinnet[(ref, num)]

    # XIAO signal lanes (F.Cu): each pad goes north to its lane, then west
    lanes = {"2": 5.8, "3": 7.6, "4": 9.4, "5": 10.9, "6": 12.2}   # XIAO pad -> lane y
    d1, d2, d3, d4, d5 = P("U3", "2"), P("U3", "3"), P("U3", "4"), P("U3", "5"), P("U3", "6")
    route(nm("U3", "2"), "F", SIG, [d1, (d1[0], 5.8), (7.0, 5.8), P("R1", "1")])
    route(nm("U3", "3"), "F", SIG, [d2, (d2[0], 7.6), P("BZ1", "1")])
    u16 = P("U1", "6")
    route(nm("U3", "4"), "F", SIG, [d3, (d3[0], 9.4), (u16[0], 9.4), u16])
    route(nm("U3", "5"), "F", SIG, [d4, (d4[0], 10.9), (P("R2", "1")[0], 10.9), P("R2", "1")])
    route(nm("U3", "6"), "F", SIG, [d5, (d5[0], 12.2), (P("R3", "1")[0], 12.2), P("R3", "1")])
    # resistor to opto inputs
    r1b = P("R1", "2")
    u11 = P("U1", "1")
    route(nm("R1", "2"), "F", SIG, [r1b, (r1b[0], u11[1]), u11])
    route(nm("R2", "2"), "F", SIG, [P("R2", "2"), P("U2", "3")])
    route(nm("R3", "2"), "F", SIG, [P("R3", "2"), P("U2", "1")])
    # PS_ON#: U1 pin 8 to J1 pin 1 on the back
    u18 = P("U1", "8")
    j11 = P("J1", "1")
    route(nm("U1", "8"), "B", SIG, [u18, (8.5, u18[1] + 1.5), (8.5, 34.0), j11])
    # LED sense path
    route(nm("U1", "3"), "F", SIG, [P("U1", "3"), P("J2", "1")])
    u14, r4_2 = P("U1", "4"), P("R4", "2")
    route(nm("U1", "4"), "F", SIG, [u14, (u14[0], r4_2[1]), r4_2])
    j1p, r4_1 = P("JP1", "1"), P("R4", "1")
    jp2p = P("JP1", "2")
    route(nm("JP1", "2"), "F", SIG, [jp2p, (jp2p[0], r4_2[1])])
    route(nm("JP1", "1"), "F", SIG, [j1p, (j1p[0] + 0.9, r4_1[1]), r4_1])
    route(nm("R4", "1"), "F", SIG, [r4_1, P("J2", "2")])
    # opto outputs to J2
    u26, j23 = P("U2", "6"), P("J2", "3")
    route(nm("U2", "6"), "F", SIG, [u26, (u26[0], 32.9), (j23[0], 32.9), j23])
    u28, j24 = P("U2", "8"), P("J2", "4")
    route(nm("U2", "8"), "F", SIG, [u28, (u28[0], 34.0), (j24[0], 34.0), j24])
    # front switch
    d7, j31 = P("U3", "14"), P("J3", "1")
    route(nm("U3", "14"), "F", SIG, [d7, (d7[0], d7[1] + 1.4), (j31[0], j31[1] - 2.86), j31])
    d10, j33 = P("U3", "11"), P("J3", "3")
    route(nm("U3", "11"), "F", SIG, [d10, (d10[0], d10[1] + 2.4), (j33[0], j33[1] - 2.7), j33])
    # +5VSB: J1 pin 3 along the back to D1 anode; jumper JP2 across D1 on the front
    j13, d1a = P("J1", "3"), P("D1", "2")
    route(nm("J1", "3"), "B", PWR, [j13, (j13[0], d1a[1]), d1a])
    jp21 = P("JP2", "1")
    route(nm("JP2", "1"), "F", PWR, [jp21, (d1a[0], jp21[1]), d1a])
    jp22, d1k = P("JP2", "2"), P("D1", "1")
    route(nm("JP2", "2"), "F", PWR, [jp22, (d1k[0], jp22[1]), d1k])
    # +5V: D1 cathode to the XIAO 5V pad on the back
    x5 = P("U3", "8")
    route(nm("U3", "8"), "B", PWR, [d1k, (d1k[0] + 1.4, d1k[1]), (d1k[0] + 1.4, 33.0), (x5[0], 33.0), x5])

    # ------------------------------------------------------------ keep-out
    ko = pcbnew.ZONE(board)
    ko.SetIsRuleArea(True)
    ko.SetDoNotAllowZoneFills(True)
    ko.SetDoNotAllowTracks(True)
    ko.SetDoNotAllowVias(True)
    ko.SetDoNotAllowPads(False)
    ko.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET()
    ls.AddLayer(pcbnew.F_Cu)
    ls.AddLayer(pcbnew.B_Cu)
    ko.SetLayerSet(ls)
    ko.SetZoneName("XIAO antenna keep-out")
    ko_rect = (35.0, 12.6, 41.7, 31.5)
    ko.Outline().NewOutline()
    for x, y in [(ko_rect[0], ko_rect[1]), (ko_rect[2], ko_rect[1]), (ko_rect[2], ko_rect[3]), (ko_rect[0], ko_rect[3])]:
        ko.Outline().Append(FromMM(x), FromMM(y))
    board.Add(ko)

    # ------------------------------------------------------------ GND pours
    def pour(layer):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(net("GND"))
        z.SetMinThickness(FromMM(0.25))
        z.SetLocalClearance(FromMM(0.3))
        z.SetThermalReliefGap(FromMM(0.3))
        z.SetThermalReliefSpokeWidth(FromMM(0.5))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        z.Outline().NewOutline()
        m = 0.3
        for x, y in [(m, m), (W - m, m), (W - m, H - m), (m, H - m)]:
            z.Outline().Append(FromMM(x), FromMM(y))
        board.Add(z)
        return z
    pour(pcbnew.F_Cu)
    pour(pcbnew.B_Cu)

    # ------------------------------------------------------------ stitching vias
    pads = []
    for fp in fps.values():
        for p in fp.Pads():
            pos = p.GetPosition()
            sz = p.GetSize()
            pads.append((pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y), max(pcbnew.ToMM(sz.x), pcbnew.ToMM(sz.y)) / 2))

    def seg_dist(px, py, a, b):
        ax, ay = a
        bx, by = b
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0 if L2 == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L2))
        return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

    holes = [(3, 3), (57, 3), (3, 37), (57, 37)]
    nvia = 0
    gy = 2.0
    while gy < H - 1.5:
        gx = 2.0
        while gx < W - 1.5:
            ok = True
            if ko_rect[0] - 1.0 < gx < ko_rect[2] + 1.0 and ko_rect[1] - 1.0 < gy < ko_rect[3] + 1.0:
                ok = False
            # keep clear of the XIAO module footprint area on the antenna side
            for hx, hy in holes:
                if math.hypot(gx - hx, gy - hy) < 4.6:
                    ok = False
            for px, py, pr in pads:
                if math.hypot(gx - px, gy - py) < pr + 0.4 + 0.75:
                    ok = False
            for _, _, w, pts in tracks:
                for a, b in zip(pts[:-1], pts[1:]):
                    if seg_dist(gx, gy, a, b) < w / 2 + 0.4 + 0.75:
                        ok = False
            # keep off the silkscreen text bands
            if ok:
                v = pcbnew.PCB_VIA(board)
                v.SetPosition(mm(gx, gy))
                try:
                    v.SetWidth(FromMM(0.8))
                except TypeError:
                    v.SetWidth(pcbnew.F_Cu, FromMM(0.8))
                v.SetDrill(FromMM(0.4))
                v.SetNet(net("GND"))
                v.SetViaType(pcbnew.VIATYPE_THROUGH)
                board.Add(v)
                nvia += 1
            gx += 5.0
        gy += 5.0
    print("stitching vias:", nvia)

    # ------------------------------------------------------------ silkscreen text
    def text(s, x, y, size=1.0, thick=0.15, angle=0, just="c", bold=False, layer=pcbnew.F_SilkS):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetPosition(mm(x, y))
        t.SetLayer(layer)
        t.SetTextSize(VECTOR2I(FromMM(size), FromMM(size)))
        t.SetTextThickness(FromMM(thick))
        t.SetTextAngleDegrees(angle)
        t.SetHorizJustify({"c": pcbnew.GR_TEXT_H_ALIGN_CENTER, "l": pcbnew.GR_TEXT_H_ALIGN_LEFT,
                           "r": pcbnew.GR_TEXT_H_ALIGN_RIGHT}[just])
        if bold:
            t.SetBold(True)
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        board.Add(t)

    text("BC-250 Power Controller", 25.0, 2.0, 1.2, 0.18, just="l", bold=True)
    text("rev A   CERN-OHL-S-2.0", 25.0, 3.55, 0.8, 0.13, just="l")
    text("Marcin K.   github.com/marcinx64", 25.0, 4.7, 0.8, 0.13, just="l")
    # note in the free strip beside the XIAO antenna end (reads bottom to top)
    text("+5VSB & USB:", 36.0, 22.0, 0.8, 0.14, 90, "c", True)
    text("only with D1 fitted", 37.5, 22.0, 0.8, 0.14, 90, "c", True)
    # connector pin names, staggered under the pads
    labels = {"J1": ["PS_ON#", "GND", "+5VSB"], "J2": ["LED+", "LED-", "PWR", "RST"],
              "J3": ["SW", "COM", "LED+", "LED-"]}
    for ref, names in labels.items():
        for i, nme in enumerate(names):
            x, y = P(ref, str(i + 1))
            text(nme, x, y + 1.85, 0.55, 0.1)

    board.BuildListOfNets()
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    out = os.path.join(KICAD, D.PROJECT + ".kicad_pcb")
    pcbnew.SaveBoard(out, board)
    print("saved", out)


if __name__ == "__main__":
    build()
