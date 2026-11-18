# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate bc250-power-controller.kicad_sch from design.py.

  flatpak run --command=python3 org.kicad.KiCad hardware/kicad/scripts/gen_sch.py

Every pin is wired to a short stub with a global net label, so the nets read
the same everywhere. Power flags are added on +5V, +5VSB and GND for ERC.
"""
import math
import os
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design as D  # noqa: E402
from sexpr import extract_symbol_text, find, find_all, parse, val  # noqa: E402

KICAD = os.path.dirname(HERE)
LIB = "/app/extensions/Library/symbols/"
ROOT = str(uuid.uuid5(uuid.NAMESPACE_URL, "bc250-power-controller-root"))
_n = [0]


def uid(tag=""):
    _n[0] += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "bc250/%s/%d" % (tag, _n[0])))


def fmt(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ---------------------------------------------------------------- symbols
SYMS = {}  # lib_id -> dict(text, pins{num:(x,y,ang,unit)})


def load_symbol(lib_id):
    if lib_id in SYMS:
        return SYMS[lib_id]
    lib, name = lib_id.split(":")
    path = KICAD + "/bc250.kicad_sym" if lib == "bc250" else LIB + lib + ".kicad_sym"
    t = open(path).read()
    raw = extract_symbol_text(t, name)
    s = parse(raw)[0]
    pins = {}
    for sub in find_all(s, "symbol"):
        nm = val(sub[1])
        unit = int(nm.rsplit("_", 2)[1])
        for p in find_all(sub, "pin"):
            at = find(p, "at")
            num = val(find(p, "number")[1])
            pins[num] = (float(at[1]), float(at[2]), float(at[3]), unit)
    text = raw.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
    SYMS[lib_id] = dict(text=text, pins=pins)
    return SYMS[lib_id]


def rot(dx, dy, a):
    a = math.radians(a)
    c, s = round(math.cos(a)), round(math.sin(a))
    return dx * c + dy * s, -dx * s + dy * c


class Sch:
    def __init__(self):
        self.items = []
        self.libs = []
        self.pinpos = {}  # (ref, pin) -> (x, y, outdir)

    def lib(self, lib_id):
        if lib_id not in [l for l in self.libs]:
            self.libs.append(lib_id)
        return load_symbol(lib_id)

    # primitives
    def wire(self, a, b):
        self.items.append('\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n\t\t(stroke (width 0) (type default))\n\t\t(uuid "%s")\n\t)'
                          % (fmt(a[0]), fmt(a[1]), fmt(b[0]), fmt(b[1]), uid("w")))

    def label(self, net, pos, ang):
        just = "left" if ang in (0, 90) else "right"
        self.items.append('\t(global_label "%s"\n\t\t(shape passive)\n\t\t(at %s %s %d)\n\t\t(effects (font (size 1.27 1.27)) (justify %s))\n\t\t(uuid "%s")\n\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify %s) hide))\n\t)'
                          % (net, fmt(pos[0]), fmt(pos[1]), ang, just, uid("l"), fmt(pos[0]), fmt(pos[1]), just))

    def noconnect(self, pos):
        self.items.append('\t(no_connect (at %s %s) (uuid "%s"))' % (fmt(pos[0]), fmt(pos[1]), uid("nc")))

    def text(self, s, pos, size=1.27, bold=False):
        s = s.replace('"', "'")
        self.items.append('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at %s %s 0)\n\t\t(effects (font (size %s %s)%s) (justify left bottom))\n\t\t(uuid "%s")\n\t)'
                          % (s, fmt(pos[0]), fmt(pos[1]), size, size, " bold" if bold else "", uid("t")))

    def rect(self, a, b):
        self.items.append('\t(rectangle (start %s %s) (end %s %s) (stroke (width 0.2) (type dash)) (fill (type none)) (uuid "%s"))'
                          % (fmt(a[0]), fmt(a[1]), fmt(b[0]), fmt(b[1]), uid("r")))

    # symbol instance
    def symbol(self, ref, lib_id, value, fp, x, y, angle=0, unit=1, pins_used=None,
               refpos=None, valpos=None, desc="", dnp=False, hide_ref=False, extra=None):
        sd = self.lib(lib_id)
        props = []
        rp = refpos or (x + 2.54, y - 5.08)
        vp = valpos or (x + 2.54, y + 5.08)
        eff = '(effects (font (size 1.27 1.27)) (justify left)%s)'
        props.append('\t\t(property "Reference" "%s" (at %s %s 0) %s)' % (ref, fmt(rp[0]), fmt(rp[1]), eff % (" hide" if hide_ref else "")))
        props.append('\t\t(property "Value" "%s" (at %s %s 0) %s)' % (value, fmt(vp[0]), fmt(vp[1]), eff % ""))
        props.append('\t\t(property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (fp, fmt(x), fmt(y)))
        props.append('\t\t(property "Datasheet" "" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (fmt(x), fmt(y)))
        props.append('\t\t(property "Description" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (desc, fmt(x), fmt(y)))
        for k, v in (extra or {}).items():
            props.append('\t\t(property "%s" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) hide))' % (k, v, fmt(x), fmt(y)))
        pinlines = []
        res = {}
        for num, (px, py, pa, pu) in sd["pins"].items():
            if pu not in (0, unit):
                continue
            dx, dy = rot(px, -py, angle)
            # outward direction is opposite of the lib pin angle
            ox, oy = rot(math.cos(math.radians(pa + 180)), -math.sin(math.radians(pa + 180)), angle)
            res[num] = (round(x + dx, 4), round(y + dy, 4), (round(ox), round(oy)))
            self.pinpos[(ref, num)] = res[num]
            pinlines.append('\t\t(pin "%s" (uuid "%s"))' % (num, uid("p")))
        self.items.append('''\t(symbol
\t\t(lib_id "%s")
\t\t(at %s %s %d)
\t\t(unit %d)
\t\t(exclude_from_sim no)
\t\t(in_bom %s)
\t\t(on_board yes)
\t\t(dnp %s)
\t\t(uuid "%s")
%s
%s
\t\t(instances
\t\t\t(project "%s"
\t\t\t\t(path "/%s" (reference "%s") (unit %d))
\t\t\t)
\t\t)
\t)''' % (lib_id, fmt(x), fmt(y), angle, unit, "no" if (ref.startswith(("MH", "JP", "#"))) else "yes", "yes" if dnp else "no", uid("s"),
           "\n".join(props), "\n".join(pinlines), D.PROJECT, ROOT, ref, unit))
        return res

    def stub(self, ref, pin, net, length=5.08, nc=False):
        x, y, (ox, oy) = self.pinpos[(ref, pin)]
        if nc:
            self.noconnect((x, y))
            return (x, y)
        end = (x + ox * length, y + oy * length)
        self.wire((x, y), end)
        ang = {(1, 0): 0, (0, -1): 90, (-1, 0): 180, (0, 1): 270}[(ox, oy)]
        self.label(net, end, ang)
        return end

    def power(self, kind, pos, angle=0):
        """power symbol with its pin at pos (GND hangs down, PWR_FLAG goes up)"""
        lib_id = {"GND": "power:GND", "FLAG": "power:PWR_FLAG", "+5V": "power:+5V"}[kind]
        self.lib(lib_id)
        self.pwr_n = getattr(self, "pwr_n", 0) + 1
        ref = "#FLG0%d" % self.pwr_n if kind == "FLAG" else "#PWR0%d" % self.pwr_n
        self.symbol(ref, lib_id, {"GND": "GND", "FLAG": "PWR_FLAG", "+5V": "+5V"}[kind], "",
                    pos[0], pos[1], angle, hide_ref=True, refpos=(pos[0] + 2, pos[1]),
                    valpos=(pos[0] + 2, pos[1] + 2.54))

    def tap(self, end, net_dir, down=False):
        """from a label end, add a short wire to a PWR_FLAG (and optionally a GND symbol)"""
        x, y = end
        ox, oy = net_dir
        # flag goes on the opposite side of the label, perpendicular to the stub
        fx, fy = (0, 1 if down else -1) if ox != 0 else (1, 0)
        p = (x + fx * 5.08, y + fy * 5.08)
        self.wire((x, y), p)
        self.power("FLAG", p, 180 if fy == 1 else 0)


def build():
    s = Sch()
    # ---------------- layout (x, y, rotation, unit)
    L = {}

    # XIAO
    x, y = 88.9, 101.6
    s.symbol("U3", "bc250:XIAO_ESP32C3", "XIAO_ESP32C3", "bc250:XIAO_ESP32C3_THT", x, y, 0, 1,
             refpos=(x - 12.7, y - 14.5), valpos=(x - 12.7, y + 14.5), desc=D.PART["U3"]["desc"])
    p = D.PART["U3"]
    for pin in ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14"]:
        net = p["pins"].get(pin)
        if net is None:
            s.stub("U3", pin, None, nc=True)
        else:
            end = s.stub("U3", pin, net)
            if net == "+5V":
                s.tap(end, (1, 0))

    # resistors and optos
    def res(ref, y, net1, net2, uref, uy):
        pr = D.PART[ref]
        s.symbol(ref, "Device:R", pr["value"], pr["fp"], 160.02, y, 90, refpos=(157.48, y - 3.81),
                 valpos=(157.48, y + 5.08), desc=pr["desc"])
        s.stub(ref, "1", net1)
        a = s.pinpos[(ref, "2")]
        b = s.pinpos[(uref, "1" if uref == "U1" else ("1" if net2 == "RST_DRV" else "3"))]
        s.wire((a[0], a[1]), (b[0], b[1]))
        s.label(net2, ((a[0] + b[0]) / 2 - 2.54, a[1]), 0)

    def opto(ref, unit, y, nets, title):
        pr = D.PART[ref]
        s.symbol(ref, "bc250:TLP521-2", pr["value"], pr["fp"], 215.9, y, 0, unit,
                 refpos=(217.2, y - 8.89), valpos=(217.2, y + 8.89), desc=pr["desc"])
        s.text(title, (190, y - 6.5), 1.27)
        return nets

    # U1 unit 1: PS_ON#  (pins 1,2,7,8) ; unit 2: LED sense (3,4,5,6)
    opto("U1", 1, 40.64, None, "U1A: PS_ON# to ATX")
    opto("U1", 2, 63.5, None, "U1B: BC-250 power LED sense")
    opto("U2", 1, 88.9, None, "U2A: reset button")
    opto("U2", 2, 111.76, None, "U2B: power button")
    # resistors wired straight to the opto LED anodes
    for ref, y, n1, n2, uref, upin in [("R1", 38.1, "PSON", "PSON_DRV", "U1", "1"),
                                       ("R3", 86.36, "RST", "RST_DRV", "U2", "1"),
                                       ("R2", 109.22, "PWR", "PWR_DRV", "U2", "3")]:
        pr = D.PART[ref]
        s.symbol(ref, "Device:R", pr["value"], pr["fp"], 160.02, y, 90, refpos=(157.48, y - 3.81),
                 valpos=(157.48, y + 5.08), desc=pr["desc"])
        s.stub(ref, "1", n1)
        a = s.pinpos[(ref, "2")]
        b = s.pinpos[(uref, upin)]
        s.wire((a[0], a[1]), (b[0], b[1]))
        s.label(n2, (a[0] + 5.08, a[1]), 0)
    for ref in ("U1", "U2"):
        for pin, net in D.PART[ref]["pins"].items():
            x_, y_, _ = s.pinpos[(ref, pin)]
            if net in ("PSON_DRV", "RST_DRV", "PWR_DRV"):
                continue
            s.stub(ref, pin, net)

    # connectors
    for ref, y in [("J1", 40.64), ("J2", 66.04), ("J3", 93.98)]:
        pr = D.PART[ref]
        lib = "Connector_Generic:Conn_01x03" if ref == "J1" else "Connector_Generic:Conn_01x04"
        s.symbol(ref, lib, pr["value"], pr["fp"], 292.1, y, 0, 1, refpos=(289.56, y - 7.62),
                 valpos=(289.56, y + 10.16), desc=pr["desc"])
        for pin, net in pr["pins"].items():
            end = s.stub(ref, pin, net)
            if net == "+5VSB":
                s.tap(end, (-1, 0), down=True)
    s.text("J1  ATX power supply: 1 PS_ON# (green, pin 16), 2 GND (black), 3 +5VSB (purple, pin 9)", (250, 135))
    s.text("J2  BC-250: 1 power LED +, 2 power LED -, 3 power button, 4 reset button", (250, 139))
    s.text("J3  front switch: 1 contact NO (white), 2 common (yellow), 3 LED + (red), 4 LED - (black)", (250, 143))

    # optional parts
    for ref, lib, x_, y_, ang in [("R4", "Device:R", 347.98, 40.64, 90),
                                  ("JP1", "Jumper:SolderJumper_2_Bridged", 347.98, 53.34, 0),
                                  ("D1", "Device:D_Schottky", 347.98, 76.2, 180),
                                  ("JP2", "Jumper:SolderJumper_2_Bridged", 347.98, 88.9, 0)]:
        pr = D.PART[ref]
        s.symbol(ref, lib, pr["value"], pr["fp"], x_, y_, ang, 1, refpos=(x_ - 5.08, y_ - 5.08),
                 valpos=(x_ - 5.08, y_ + 6.35), desc=pr["desc"], dnp=pr.get("dnp", False))
        for pin, net in pr["pins"].items():
            s.stub(ref, pin, net)
    s.text("Optional: LED sense series resistor R4 (470R).", (330, 31))
    s.text("JP1 is closed (R4 bypassed). Cut JP1 and fit R4 if the sense current is too high.", (330, 34.5))
    s.text("Optional: Schottky D1 in the +5VSB line (about 0.3 V drop).", (330, 66))
    s.text("JP2 is closed (D1 bypassed). Cut JP2 and fit D1 to allow USB and +5VSB together.", (330, 69.5))
    s.text("With D1 NOT fitted: never connect USB and +5VSB at the same time.", (330, 103), 1.27, True)

    # buzzer and mounting holes
    pr = D.PART["BZ1"]
    s.symbol("BZ1", "Device:Buzzer", pr["value"], pr["fp"], 228.6, 160.02, 0, 1, refpos=(233, 156), valpos=(233, 164.5), desc=pr["desc"])
    for pin, net in pr["pins"].items():
        end = s.stub("BZ1", pin, net)
        if net == "GND":
            s.tap(end, (-1, 0), down=True)
    s.text("Active buzzer, driven directly from GPIO4 (firmware sets maximum drive strength)", (200, 172))

    for i, ref in enumerate(["MH1", "MH2", "MH3", "MH4"]):
        pr = D.PART[ref]
        s.symbol(ref, "Mechanical:MountingHole", pr["value"], pr["fp"], 330 + 15 * i, 160, 0, 1,
                 refpos=(330 + 15 * i - 3, 164), valpos=(330 + 15 * i - 3, 167), desc=pr["desc"])

    # notes
    s.text("BC-250 power controller, rev A. Same circuit and GPIO map as the perfboard build (docs/HARDWARE.md).", (20, 20), 1.8, True)
    s.text("GPIO2, 8, 9 (strapping) and GPIO21 are unused. GPIO20 is the UART0 RX pin: never start UART0.", (20, 26))
    s.text("The optos are open-collector switches to the common ground; they do not isolate anything here.", (20, 190))
    s.text("U1/U2: TLP521-2 (LTV-827, PC827 and EL827 are pin-compatible).", (20, 195))
    s.text("Sense: BC-250 power LED current flows through the opto LED. Pull-up is internal (GPIO5), low = LED lit.", (20, 200))
    s.text("The XIAO 5V pin is USB VBUS: do not connect USB and +5VSB together unless D1 is fitted and JP2 is cut.", (20, 205))

    return s


def write(s):
    libs = []
    for lib_id in s.libs:
        libs.append(SYMS[lib_id]["text"])
    out = ['(kicad_sch\n\t(version 20250610)\n\t(generator "eeschema")\n\t(generator_version "10.0")\n\t(uuid "%s")\n\t(paper "A3")'
           % ROOT,
           '\t(title_block\n\t\t(title "%s")\n\t\t(date "%s")\n\t\t(rev "%s")\n\t\t(company "%s")\n\t\t(comment 1 "License: %s")\n\t\t(comment 2 "Same circuit and GPIO map as the tested perfboard build")\n\t)'
           % (D.TITLE, D.DATE, D.REV, D.COMPANY, D.LICENCE),
           "\t(lib_symbols\n" + "\n".join("\t\t" + l.replace("\n", "\n\t\t") for l in libs) + "\n\t)"]
    out += s.items
    out.append('\t(sheet_instances\n\t\t(path "/" (page "1"))\n\t)\n\t(embedded_fonts no)\n)')
    open(os.path.join(KICAD, D.PROJECT + ".kicad_sch"), "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    write(build())
