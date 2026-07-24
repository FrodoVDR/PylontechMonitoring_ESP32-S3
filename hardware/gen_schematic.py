#!/usr/bin/env python3
"""Generator for the Pylontech RS-232 interface KiCad schematic.

Builds custom symbols with REAL pin names (ESP32-S3 GPIOs, MAX3232 module,
battery RJ45, optional ST7735 display) and routes every wire to the exact
computed pin endpoint so nothing 'ends nowhere'.

KiCad pin geometry used here:
  - A pin's (at x y angle) is its CONNECTION endpoint (the outer tip).
  - angle = direction from tip toward the body: 0=+x, 90=+y(up), 180=-x, 270=-y(down)
  - Left  side pin: tip_lx = -(halfW+L), angle 0
    Right side pin: tip_lx = +(halfW+L), angle 180
    Top   pin:      tip_ly = +(halfH+L), angle 270
    Bottom pin:     tip_ly = -(halfH+L), angle 90
  - Placement (rotation 0, no mirror): abs = (ox+lx, oy-ly)   [Y is flipped]
"""
import uuid

ROOT = "00000000-0000-4000-8000-000000000001"
L = 5.08  # pin length


def U():
    return str(uuid.uuid4())


# ---------------------------------------------------------------------------
# Symbol builders. Each returns (lib_symbol_text, pins_local) where pins_local
# maps pin-name -> (local_x, local_y) of the connection endpoint.
# ---------------------------------------------------------------------------

def _pin(ptype, tip_x, tip_y, ang, name, num):
    return (f'\t\t\t\t(pin {ptype} line (at {tip_x} {tip_y} {ang}) (length {L})\n'
            f'\t\t\t\t\t(name "{name}" (effects (font (size 1.27 1.27))))\n'
            f'\t\t\t\t\t(number "{num}" (effects (font (size 1.27 1.27)))))\n')


def build_symbol(libid, ref_prefix, value, halfW, halfH,
                 left=None, right=None, top=None, bottom=None):
    """left/right: list of (name, ly, num, ptype); top/bottom: (name, lx, num, ptype)."""
    left = left or []
    right = right or []
    top = top or []
    bottom = bottom or []
    pins_local = {}
    body = (f'\t\t\t(symbol "{libid}"\n'
            f'\t\t\t\t(pin_names (offset 1.016))\n'
            f'\t\t\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)\n'
            f'\t\t\t\t(property "Reference" "{ref_prefix}" (at 0 {halfH+2.54} 0)\n'
            f'\t\t\t\t\t(effects (font (size 1.27 1.27))))\n'
            f'\t\t\t\t(property "Value" "{value}" (at 0 {-halfH-2.54} 0)\n'
            f'\t\t\t\t\t(effects (font (size 1.27 1.27))))\n'
            f'\t\t\t\t(property "Footprint" "" (at 0 0 0)\n'
            f'\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n'
            f'\t\t\t\t(property "Datasheet" "~" (at 0 0 0)\n'
            f'\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n')
    unit = f'\t\t\t\t(symbol "{libid.split(":")[-1]}_1_1"\n'
    unit += (f'\t\t\t\t\t(rectangle (start {-halfW} {halfH}) (end {halfW} {-halfH})\n'
             f'\t\t\t\t\t\t(stroke (width 0.254) (type default))\n'
             f'\t\t\t\t\t\t(fill (type background)))\n')
    for name, ly, num, ptype in left:
        tip_x = -(halfW + L)
        unit += _pin(ptype, tip_x, ly, 0, name, num)
        pins_local[name] = (tip_x, ly)
    for name, ly, num, ptype in right:
        tip_x = halfW + L
        unit += _pin(ptype, tip_x, ly, 180, name, num)
        pins_local[name] = (tip_x, ly)
    for name, lx, num, ptype in top:
        tip_y = halfH + L
        unit += _pin(ptype, lx, tip_y, 270, name, num)
        pins_local[name] = (lx, tip_y)
    for name, lx, num, ptype in bottom:
        tip_y = -(halfH + L)
        unit += _pin(ptype, lx, tip_y, 90, name, num)
        pins_local[name] = (lx, tip_y)
    unit += '\t\t\t\t)\n'
    return body + unit + '\t\t\t)\n', pins_local


def cap_symbol():
    txt = ('\t\t\t(symbol "Device:C"\n'
           '\t\t\t\t(pin_names (offset 0.254))\n'
           '\t\t\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)\n'
           '\t\t\t\t(property "Reference" "C" (at 0.635 2.54 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27)) (justify left)))\n'
           '\t\t\t\t(property "Value" "C" (at 0.635 -2.54 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27)) (justify left)))\n'
           '\t\t\t\t(property "Footprint" "" (at 0 0 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n'
           '\t\t\t\t(property "Datasheet" "~" (at 0 0 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n'
           '\t\t\t\t(symbol "C_0_1"\n'
           '\t\t\t\t\t(polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762))\n'
           '\t\t\t\t\t\t(stroke (width 0.508) (type default)) (fill (type none)))\n'
           '\t\t\t\t\t(polyline (pts (xy -2.032 0.762) (xy 2.032 0.762))\n'
           '\t\t\t\t\t\t(stroke (width 0.508) (type default)) (fill (type none))))\n'
           '\t\t\t\t(symbol "C_1_1"\n'
           '\t\t\t\t\t(pin passive line (at 0 3.81 270) (length 2.794)\n'
           '\t\t\t\t\t\t(name "~" (effects (font (size 1.27 1.27))))\n'
           '\t\t\t\t\t\t(number "1" (effects (font (size 1.27 1.27)))))\n'
           '\t\t\t\t\t(pin passive line (at 0 -3.81 90) (length 2.794)\n'
           '\t\t\t\t\t\t(name "~" (effects (font (size 1.27 1.27))))\n'
           '\t\t\t\t\t\t(number "2" (effects (font (size 1.27 1.27)))))))\n')
    # connection points local: pin1 (0, 3.81), pin2 (0, -3.81)
    return txt, {"1": (0, 3.81), "2": (0, -3.81)}


def power_symbols():
    gnd = ('\t\t\t(symbol "power:GND"\n'
           '\t\t\t\t(power) (pin_numbers (hide yes)) (pin_names (offset 0))\n'
           '\t\t\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)\n'
           '\t\t\t\t(property "Reference" "#PWR" (at 0 -6.35 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n'
           '\t\t\t\t(property "Value" "GND" (at 0 -3.81 0)\n'
           '\t\t\t\t\t(effects (font (size 1.27 1.27))))\n'
           '\t\t\t\t(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
           '\t\t\t\t(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
           '\t\t\t\t(symbol "GND_0_1"\n'
           '\t\t\t\t\t(pin power_in line (at 0 0 270) (length 0) hide\n'
           '\t\t\t\t\t\t(name "GND" (effects (font (size 1.27 1.27))))\n'
           '\t\t\t\t\t\t(number "1" (effects (font (size 1.27 1.27)))))\n'
           '\t\t\t\t\t(polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27))\n'
           '\t\t\t\t\t\t(stroke (width 0) (type default)) (fill (type none)))))\n')
    v3 = ('\t\t\t(symbol "power:+3V3"\n'
          '\t\t\t\t(power) (pin_numbers (hide yes)) (pin_names (offset 0))\n'
          '\t\t\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)\n'
          '\t\t\t\t(property "Reference" "#PWR" (at 0 -3.81 0)\n'
          '\t\t\t\t\t(effects (font (size 1.27 1.27)) hide))\n'
          '\t\t\t\t(property "Value" "+3V3" (at 0 3.556 0)\n'
          '\t\t\t\t\t(effects (font (size 1.27 1.27))))\n'
          '\t\t\t\t(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
          '\t\t\t\t(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
          '\t\t\t\t(symbol "+3V3_0_1"\n'
          '\t\t\t\t\t(pin power_in line (at 0 0 90) (length 0) hide\n'
          '\t\t\t\t\t\t(name "+3V3" (effects (font (size 1.27 1.27))))\n'
          '\t\t\t\t\t\t(number "1" (effects (font (size 1.27 1.27)))))\n'
          '\t\t\t\t\t(polyline (pts (xy -0.762 1.27) (xy 0 2.54) (xy 0.762 1.27))\n'
          '\t\t\t\t\t\t(stroke (width 0) (type default)) (fill (type none)))\n'
          '\t\t\t\t\t(polyline (pts (xy 0 0) (xy 0 2.54))\n'
          '\t\t\t\t\t\t(stroke (width 0) (type default)) (fill (type none)))))\n')
    return gnd + v3


# ---------------------------------------------------------------------------
# Define symbols
# ---------------------------------------------------------------------------
esp_lib, esp_pins = build_symbol(
    "ESP32S3_ETH:Module", "J", "ESP32-S3-ETH", 12.7, 20.32,
    right=[("3V3", 15.24, "1", "power_in"),
           ("GPIO17/TX", 7.62, "2", "output"),
           ("GPIO16/RX", 2.54, "3", "input"),
           ("GND", -15.24, "4", "power_in")],
    left=[("GPIO18/MOSI", 12.7, "5", "output"),
          ("GPIO15/SCK", 7.62, "6", "output"),
          ("GPIO38/CS", 2.54, "7", "output"),
          ("GPIO39/DC", -2.54, "8", "output"),
          ("GPIO40/RST", -7.62, "9", "output"),
          ("GPIO41/BL", -12.7, "10", "output")])

max_lib, max_pins = build_symbol(
    "Interface_UART:MAX3232_Module", "U", "MAX3232-Modul", 12.7, 12.7,
    left=[("T1IN", 5.08, "1", "input"),
          ("R1OUT", -5.08, "2", "output")],
    right=[("T1OUT", 5.08, "3", "output"),
           ("R1IN", -5.08, "4", "input")],
    top=[("VCC", 0, "5", "power_in")],
    bottom=[("GND", 0, "6", "power_in")])

bat_lib, bat_pins = build_symbol(
    "Connector:Battery_RJ45", "J", "Pylontech RS232", 7.62, 17.78,
    left=[("P6_T1OUT", 5.08, "6", "passive"),
          ("P3_R1IN", -5.08, "3", "passive"),
          ("P8_GND", -15.24, "8", "passive")])

disp_lib, disp_pins = build_symbol(
    "Display:ST7735", "J", "TFT ST7735 (opt.)", 7.62, 25.4,
    right=[("VCC", 15.24, "1", "power_in"),
           ("MOSI", 7.62, "2", "input"),
           ("SCK", 2.54, "3", "input"),
           ("CS", -2.54, "4", "input"),
           ("DC", -7.62, "5", "input"),
           ("RST", -12.7, "6", "input"),
           ("BL", -17.78, "7", "input"),
           ("GND", -22.86, "8", "power_in")])

cap_lib, cap_pins = cap_symbol()

# ---------------------------------------------------------------------------
# Placement helpers
# ---------------------------------------------------------------------------
placements = {}  # ref -> (ox, oy, pins_local)


def abs_pin(ref, name):
    ox, oy, pins = placements[ref]
    lx, ly = pins[name]
    return (round(ox + lx, 4), round(oy - ly, 4))


parts = []


def place(libid, ref, value, ox, oy, pins_local, hide_val=False):
    placements[ref] = (ox, oy, pins_local)
    val_hide = " hide" if hide_val else ""
    s = f'\t(symbol (lib_id "{libid}") (at {ox} {oy} 0) (unit 1)\n'
    s += '\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
    s += f'\t\t(uuid "{U()}")\n'
    # reference/value label offset above/below
    s += f'\t\t(property "Reference" "{ref}" (at {ox} {oy - 24} 0) (effects (font (size 1.27 1.27))))\n'
    s += f'\t\t(property "Value" "{value}" (at {ox} {oy + 26} 0) (effects (font (size 1.27 1.27)){val_hide}))\n'
    # pin uuids
    for num in sorted({v for v in range(1, 20)}):
        pass
    s += f'\t\t(instances (project "PylontechInterface" (path "/{ROOT}" (reference "{ref}") (unit 1)))))'
    parts.append(s)


def place_pwr(libid, ref, value, ox, oy):
    s = f'\t(symbol (lib_id "{libid}") (at {ox} {oy} 0) (unit 1)\n'
    s += '\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
    s += f'\t\t(uuid "{U()}")\n'
    s += f'\t\t(property "Reference" "{ref}" (at {ox} {oy} 0) (effects (font (size 1.27 1.27)) hide))\n'
    s += f'\t\t(property "Value" "{value}" (at {ox} {oy - 3.81 if value=="GND" else oy + 3.81} 0) (effects (font (size 1.27 1.27))))\n'
    s += f'\t\t(instances (project "PylontechInterface" (path "/{ROOT}" (reference "{ref}") (unit 1)))))'
    parts.append(s)


def wire(x1, y1, x2, y2):
    parts.append(f'\t(wire (pts (xy {x1} {y1}) (xy {x2} {y2})) (stroke (width 0) (type default)) (uuid "{U()}"))')


def junction(x, y):
    parts.append(f'\t(junction (at {x} {y}) (diameter 0) (color 0 0 0 0) (uuid "{U()}"))')


def label(txt, x, y, ang):
    just = "left" if ang == 0 else "right"
    parts.append(f'\t(label "{txt}" (at {x} {y} {ang}) (effects (font (size 1.27 1.27)) (justify {just})) (uuid "{U()}"))')


def text(t, x, y):
    parts.append(f'\t(text "{t}" (at {x} {y} 0) (effects (font (size 1.5 1.5)) (justify left)))')


# ---------------------------------------------------------------------------
# Place components
# ---------------------------------------------------------------------------
place("ESP32S3_ETH:Module", "J1", "ESP32-S3-ETH", 95, 95, esp_pins)
place("Interface_UART:MAX3232_Module", "U1", "MAX3232-Modul", 150, 95, max_pins)
place("Connector:Battery_RJ45", "J2", "Pylontech RS232", 210, 95, bat_pins)
place("Display:ST7735", "J3", "TFT ST7735 (opt.)", 60, 150, disp_pins)
place("Device:C", "C1", "10uF", 140, 130, cap_pins)
place("Device:C", "C2", "0.1uF", 155, 130, cap_pins)

# ---------------------------------------------------------------------------
# Signal wires (routed to exact pin endpoints)
# ---------------------------------------------------------------------------
# ESP GPIO17/TX -> MAX T1IN
x1, y1 = abs_pin("J1", "GPIO17/TX")
x2, y2 = abs_pin("U1", "T1IN")
midx = 122.5
wire(x1, y1, midx, y1); wire(midx, y1, midx, y2); wire(midx, y2, x2, y2)
# ESP GPIO16/RX -> MAX R1OUT
x1, y1 = abs_pin("J1", "GPIO16/RX")
x2, y2 = abs_pin("U1", "R1OUT")
midx = 120
wire(x1, y1, midx, y1); wire(midx, y1, midx, y2); wire(midx, y2, x2, y2)
# MAX T1OUT -> Battery P6
x1, y1 = abs_pin("U1", "T1OUT")
x2, y2 = abs_pin("J2", "P6_T1OUT")
wire(x1, y1, x2, y2)
# MAX R1IN -> Battery P3
x1, y1 = abs_pin("U1", "R1IN")
x2, y2 = abs_pin("J2", "P3_R1IN")
wire(x1, y1, x2, y2)

# ---------------------------------------------------------------------------
# Power connections (short stubs + power symbols)
# ---------------------------------------------------------------------------
def to_pwr(ref, pin, kind, dy):
    x, y = abs_pin(ref, pin)
    ty = y + dy
    wire(x, y, x, ty)
    if kind == "+3V3":
        place_pwr("power:+3V3", f"#PWR{len(parts)}", "+3V3", x, ty)
    else:
        place_pwr("power:GND", f"#PWR{len(parts)}", "GND", x, ty)

to_pwr("J1", "3V3", "+3V3", -4.0)
to_pwr("J1", "GND", "GND", 4.0)
to_pwr("U1", "VCC", "+3V3", -4.0)
to_pwr("U1", "GND", "GND", 4.0)
to_pwr("J2", "P8_GND", "GND", 4.0)
to_pwr("J3", "VCC", "+3V3", -4.0)
to_pwr("J3", "GND", "GND", 4.0)

# Decoupling caps C1, C2: pin1(top)->+3V3, pin2(bottom)->GND
for capref in ("C1", "C2"):
    xt, yt = abs_pin(capref, "1")
    xb, yb = abs_pin(capref, "2")
    wire(xt, yt, xt, yt - 4.0)
    place_pwr("power:+3V3", f"#PWR{len(parts)}", "+3V3", xt, yt - 4.0)
    wire(xb, yb, xb, yb + 4.0)
    place_pwr("power:GND", f"#PWR{len(parts)}", "GND", xb, yb + 4.0)

# ---------------------------------------------------------------------------
# Display SPI via net labels (ESP left pins <-> J3 pins)
# ---------------------------------------------------------------------------
disp_map = [("GPIO18/MOSI", "MOSI", "GPIO18"),
            ("GPIO15/SCK", "SCK", "GPIO15"),
            ("GPIO38/CS", "CS", "GPIO38"),
            ("GPIO39/DC", "DC", "GPIO39"),
            ("GPIO40/RST", "RST", "GPIO40"),
            ("GPIO41/BL", "BL", "GPIO41")]
for esp_pin, disp_pin, net in disp_map:
    x, y = abs_pin("J1", esp_pin)
    label(net, x, y, 180)
    x, y = abs_pin("J3", disp_pin)
    label(net, x, y, 0)

# ---------------------------------------------------------------------------
# Notes
# ---------------------------------------------------------------------------
text("Batterie-Konsolenport (J2):", 185, 60)
text("RJ45: Pin3->R1IN  Pin6->T1OUT  Pin8->GND", 185, 65)
text("RJ11: Pin2->R1IN  Pin3->T1OUT  Pin4->GND", 185, 70)
text("Optionales ST7735 TFT (J3, SPI3/HSPI)", 40, 118)
text("kein Pin-Konflikt mit W5500 Ethernet (SPI2/FSPI)", 40, 123)

# ---------------------------------------------------------------------------
# Assemble file
# ---------------------------------------------------------------------------
header = ('(kicad_sch\n'
          '\t(version 20231120)\n'
          '\t(generator "eeschema")\n'
          '\t(generator_version "8.0")\n'
          f'\t(uuid "{ROOT}")\n'
          '\t(paper "A4")\n'
          '\t(title_block\n'
          '\t\t(title "Pylontech Monitoring - RS-232 Interface")\n'
          '\t\t(date "2026-07-13")\n'
          '\t\t(rev "1.1"))\n')

libsyms = ('\t(lib_symbols\n'
           + esp_lib + max_lib + bat_lib + disp_lib + cap_lib
           + power_symbols()
           + '\t)\n')

footer = '\t(sheet_instances\n\t\t(path "/" (page "1"))))\n'

out = header + libsyms + '\n'.join(parts) + '\n' + footer
open("PylontechInterface.kicad_sch", "w").write(out)
print("wrote PylontechInterface.kicad_sch")
