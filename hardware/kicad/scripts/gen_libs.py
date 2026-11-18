# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the project libraries (bc250.kicad_sym, bc250.pretty, 3d model).

Run inside the KiCad flatpak so the standard libraries are found:
  flatpak run --command=python3 org.kicad.KiCad hardware/kicad/scripts/gen_libs.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sexpr import extract_symbol_text  # noqa: E402

KICAD = os.path.dirname(HERE)  # hardware/kicad
LIB = "/app/extensions/Library"

# GPIO numbers of the XIAO ESP32-C3 pins (Seeed pinout)
LEFT = [("D0", 2), ("D1", 3), ("D2", 4), ("D3", 5), ("D4", 6), ("D5", 7), ("D6", 21)]
RIGHT = [("5V", None), ("GND", None), ("3V3", None), ("D10", 10), ("D9", 9), ("D8", 8), ("D7", 20)]

FX = "(effects (font (size 1.27 1.27)))"


def xiao_symbol():
    pins = []
    for i, (n, g) in enumerate(LEFT):
        y = 7.62 - 2.54 * i
        nm = "%s/GPIO%d" % (n, g)
        pins.append('(pin bidirectional line (at -15.24 %.2f 0) (length 2.54) (name "%s" %s) (number "%d" %s))'
                    % (y, nm, FX, i + 1, FX))
    for i, (n, g) in enumerate(RIGHT):
        y = 7.62 - 2.54 * i
        if n == "5V":
            typ, nm = "power_in", "5V"
        elif n == "GND":
            typ, nm = "power_in", "GND"
        elif n == "3V3":
            typ, nm = "power_out", "3V3"
        else:
            typ, nm = "bidirectional", "%s/GPIO%d" % (n, g)
        pins.append('(pin %s line (at 15.24 %.2f 180) (length 2.54) (name "%s" %s) (number "%d" %s))'
                    % (typ, y, nm, FX, 8 + i, FX))
    return '''	(symbol "XIAO_ESP32C3"
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(property "Reference" "U" (at 0 12.7 0) %(fx)s)
		(property "Value" "XIAO_ESP32C3" (at 0 -12.7 0) %(fx)s)
		(property "Footprint" "bc250:XIAO_ESP32C3_THT" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
		(property "Datasheet" "https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
		(property "Description" "Seeed Studio XIAO ESP32-C3 module, 2 x 7 pins on 2.54 mm, pins as seen with the USB-C connector at the top" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
		(symbol "XIAO_ESP32C3_0_1"
			(rectangle (start -12.7 10.16) (end 12.7 -10.16)
				(stroke (width 0.254) (type default))
				(fill (type background)))
		)
		(symbol "XIAO_ESP32C3_1_1"
			%(pins)s
		)
		(embedded_fonts no)
	)
''' % {"fx": FX, "pins": "\n\t\t\t".join(pins)}


def tlp_symbol():
    t = open(LIB + "/symbols/Isolator.kicad_sym").read()
    s = extract_symbol_text(t, "PC827")
    s = s.replace("PC827", "TLP521-2")
    s = re.sub(r'\(property "Value" "TLP521-2"', '(property "Value" "TLP521-2"', s)
    s = re.sub(r'\(property "Footprint" "[^"]*"',
               '(property "Footprint" "Package_DIP:DIP-8_W7.62mm"', s)
    return "\t" + s + "\n"


def write_symbols():
    out = '(kicad_symbol_lib\n\t(version 20251024)\n\t(generator "bc250-gen")\n\t(generator_version "10.0")\n'
    out += tlp_symbol() + xiao_symbol() + ")\n"
    open(os.path.join(KICAD, "bc250.kicad_sym"), "w").write(out)


# ---------------------------------------------------------------- footprint
def xiao_footprint():
    pads = []
    for k in range(14):
        x, y = (0, 2.54 * k) if k < 7 else (15.24, 2.54 * (k - 7))
        shape = "rect" if k == 0 else "oval"
        pads.append('''	(pad "%d" thru_hole %s
		(at %.2f %.2f)
		(size 1.7 1.7)
		(drill 1)
		(layers "*.Cu" "*.Mask")
		(remove_unused_layers no)
	)''' % (k + 1, shape, x, y))
    w, h = 17.8, 21.0
    x0, x1 = 7.62 - w / 2, 7.62 + w / 2
    y0, y1 = -2.0, -2.0 + h  # USB-C end at y0
    usbw = 9.0
    ux0, ux1 = 7.62 - usbw / 2, 7.62 + usbw / 2

    def line(a, b, c, d, layer, wd=0.12):
        return ('	(fp_line (start %.2f %.2f) (end %.2f %.2f) (stroke (width %.2f) (type solid)) (layer "%s"))'
                % (a, b, c, d, wd, layer))
    g = []
    # silkscreen: module outline, open at the pin rows' inner side is not needed
    ys = y0 + 0.5  # keep the silkscreen 0.5 mm inside the USB-C end (board edge)
    for (a, b, c, d) in [(x0, ys, x1, ys), (x1, ys, x1, y1), (x1, y1, x0, y1), (x0, y1, x0, ys)]:
        g.append(line(a, b, c, d, "F.SilkS", 0.15))
    # USB-C outline on fab and a USB label arrow on silk
    for (a, b, c, d) in [(ux0, y0 - 0.8, ux1, y0 - 0.8), (ux1, y0 - 0.8, ux1, y0 + 6.5),
                         (ux1, y0 + 6.5, ux0, y0 + 6.5), (ux0, y0 + 6.5, ux0, y0 - 0.8)]:
        g.append(line(a, b, c, d, "F.Fab", 0.1))
    for (a, b, c, d) in [(x0 - 0.3, y0 - 1.0, x1 + 0.3, y0 - 1.0), (x1 + 0.3, y0 - 1.0, x1 + 0.3, y1 + 0.3),
                         (x1 + 0.3, y1 + 0.3, x0 - 0.3, y1 + 0.3), (x0 - 0.3, y1 + 0.3, x0 - 0.3, y0 - 1.0)]:
        g.append(line(a, b, c, d, "F.CrtYd", 0.05))
    # pin 1 marker
    g.append('	(fp_poly (pts (xy -1.9 -1.1) (xy -1.9 1.1) (xy -2.6 0)) (stroke (width 0.1) (type solid)) (fill yes) (layer "F.SilkS"))')
    # antenna end note on Fab
    ant = ('	(fp_text user "USB-C" (at 7.62 %.2f 0) (layer "F.SilkS") (effects (font (size 0.9 0.9) (thickness 0.15))))'
           % (y0 + 2.2))
    ant2 = ('	(fp_text user "ANTENNA - keep clear" (at 7.62 %.2f 0) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))'
            % (y1 - 1.4))
    return '''(footprint "XIAO_ESP32C3_THT"
	(version 20240108)
	(generator "bc250-gen")
	(generator_version "10.0")
	(layer "F.Cu")
	(descr "Seeed Studio XIAO ESP32-C3 on two 1x7 2.54 mm sockets, rows 15.24 mm apart, pin 1 = D0 (as seen with USB-C at the top)")
	(tags "XIAO ESP32C3 seeed module")
	(attr through_hole)
	(property "Reference" "REF**" (at 7.62 -3.4 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))
	(property "Value" "XIAO_ESP32C3" (at 7.62 21 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))
%s
%s
%s
%s
	(model "${KIPRJMOD}/3d/XIAO_ESP32C3_THT.wrl"
		(offset (xyz 0 0 0))
		(scale (xyz 0.3937 0.3937 0.3937))
		(rotate (xyz 0 0 0))
	)
	(model "${KICAD10_3DMODEL_DIR}/Connector_PinSocket_2.54mm.3dshapes/PinSocket_1x07_P2.54mm_Vertical.step"
		(offset (xyz 0 0 0))
		(scale (xyz 1 1 1))
		(rotate (xyz 0 0 0))
	)
	(model "${KICAD10_3DMODEL_DIR}/Connector_PinSocket_2.54mm.3dshapes/PinSocket_1x07_P2.54mm_Vertical.step"
		(offset (xyz 15.24 0 0))
		(scale (xyz 1 1 1))
		(rotate (xyz 0 0 0))
	)
)
''' % ("\n".join(g), ant + "\n" + ant2, "\n".join(pads), "")


def xiao_wrl():
    """Simple stand-in 3D model of the XIAO module (blue board, USB-C, shield).
    Units: mm in this file, the footprint scales by 0.3937 (KiCad assumes 0.1 inch)."""
    w, h = 17.8, 21.0
    cx = 7.62
    z0 = 8.5          # board bottom sits on top of the sockets
    t = 1.0           # board thickness

    def box(x0, y0, x1, y1, za, zb, rgb):
        # model +y is toward board -y (north) in KiCad: footprint y maps to -model y
        mx0, mx1 = x0, x1
        my0, my1 = -y1, -y0
        return '''Shape { appearance Appearance { material Material { diffuseColor %s specularColor 0.4 0.4 0.4 shininess 0.4 } }
 geometry IndexedFaceSet { coord Coordinate { point [ %g %g %g, %g %g %g, %g %g %g, %g %g %g, %g %g %g, %g %g %g, %g %g %g, %g %g %g ] }
 coordIndex [ 0 1 2 3 -1  7 6 5 4 -1  0 4 5 1 -1  1 5 6 2 -1  2 6 7 3 -1  3 7 4 0 -1 ] solid FALSE } }
''' % (rgb, mx0, my0, za, mx1, my0, za, mx1, my1, za, mx0, my1, za,
         mx0, my0, zb, mx1, my0, zb, mx1, my1, zb, mx0, my1, zb)
    y0 = -2.0
    s = "#VRML V2.0 utf8\n"
    s += box(cx - w / 2, y0, cx + w / 2, y0 + h, z0, z0 + t, "0.05 0.25 0.55")
    s += box(cx - 4.5, y0 - 0.6, cx + 4.5, y0 + 7.0, z0 + t, z0 + t + 3.3, "0.75 0.75 0.78")
    s += box(cx - 3.5, y0 + 9.5, cx + 3.5, y0 + 15.5, z0 + t, z0 + t + 1.3, "0.7 0.7 0.72")
    s += box(cx - 2.2, y0 + 17.2, cx + 2.2, y0 + 19.5, z0 + t, z0 + t + 0.6, "0.85 0.8 0.7")
    return s


def main():
    os.makedirs(os.path.join(KICAD, "bc250.pretty"), exist_ok=True)
    os.makedirs(os.path.join(KICAD, "3d"), exist_ok=True)
    write_symbols()
    open(os.path.join(KICAD, "bc250.pretty", "XIAO_ESP32C3_THT.kicad_mod"), "w").write(xiao_footprint())
    open(os.path.join(KICAD, "3d", "XIAO_ESP32C3_THT.wrl"), "w").write(xiao_wrl())
    open(os.path.join(KICAD, "sym-lib-table"), "w").write(
        '(sym_lib_table\n  (version 7)\n  (lib (name "bc250")(type "KiCad")(uri "${KIPRJMOD}/bc250.kicad_sym")(options "")(descr "BC-250 power controller project symbols"))\n)\n')
    open(os.path.join(KICAD, "fp-lib-table"), "w").write(
        '(fp_lib_table\n  (version 7)\n  (lib (name "bc250")(type "KiCad")(uri "${KIPRJMOD}/bc250.pretty")(options "")(descr "BC-250 power controller project footprints"))\n)\n')


if __name__ == "__main__":
    main()
