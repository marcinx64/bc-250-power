# SPDX-License-Identifier: GPL-3.0-or-later
"""Single source of truth for the BC-250 power controller PCB (rev A).

Parts, footprints and net assignment per pin. The schematic generator and the
PCB generator both read this file; the PCB generator takes the final net names
from the netlist that KiCad exports from the schematic, so both always agree.
"""

PROJECT = "bc250-power-controller"
TITLE = "BC-250 Power Controller"
REV = "A"
DATE = "2026-11-17"
COMPANY = "Marcin K. - github.com/marcinx64"
LICENCE = "CERN-OHL-S-2.0"

# nets named by function (global labels in the schematic)
# pin maps: symbol pin number -> net
PARTS = [
    # ref, symbol (lib:name), value, footprint, pins {num: net}, fields
    dict(ref="U3", sym="bc250:XIAO_ESP32C3", value="XIAO_ESP32C3",
         fp="bc250:XIAO_ESP32C3_THT",
         pins={"2": "PSON", "3": "BUZZ", "4": "SENSE", "5": "PWR", "6": "RST",
               "8": "+5V", "9": "GND", "11": "SW_LED", "14": "SW"},
         nc=["1", "7", "10", "12", "13"],
         desc="Seeed Studio XIAO ESP32-C3 on two 1x7 2.54 mm female headers"),
    dict(ref="U1", sym="bc250:TLP521-2", value="TLP521-2",
         fp="Package_DIP:DIP-8_W7.62mm",
         pins={"1": "PSON_DRV", "2": "GND", "3": "LED_A", "4": "LED_K", "5": "GND",
               "6": "SENSE", "7": "GND", "8": "PS_ON#"},
         desc="Dual optocoupler DIP-8 (LTV-827 / PC827 / EL827 are pin-compatible)"),
    dict(ref="U2", sym="bc250:TLP521-2", value="TLP521-2",
         fp="Package_DIP:DIP-8_W7.62mm",
         pins={"1": "RST_DRV", "2": "GND", "3": "PWR_DRV", "4": "GND", "5": "GND",
               "6": "PWR_HOT", "7": "GND", "8": "RST_HOT"},
         desc="Dual optocoupler DIP-8 (LTV-827 / PC827 / EL827 are pin-compatible)"),
    dict(ref="R1", sym="Device:R", value="220R",
         fp="Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
         pins={"1": "PSON", "2": "PSON_DRV"}, desc="PS_ON opto LED resistor"),
    dict(ref="R2", sym="Device:R", value="220R",
         fp="Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
         pins={"1": "PWR", "2": "PWR_DRV"}, desc="Power button opto LED resistor"),
    dict(ref="R3", sym="Device:R", value="220R",
         fp="Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal",
         pins={"1": "RST", "2": "RST_DRV"}, desc="Reset button opto LED resistor"),
    dict(ref="R4", sym="Device:R", value="470R",
         fp="Resistor_THT:R_Axial_DIN0204_L3.6mm_D1.6mm_P2.54mm_Vertical",
         pins={"1": "LED_K_IN", "2": "LED_K"}, dnp=True,
         desc="Optional series resistor for the LED sense input (bridged by JP1)"),
    dict(ref="JP1", sym="Jumper:SolderJumper_2_Bridged", value="R4 bypass",
         fp="Jumper:SolderJumper-2_P1.3mm_Bridged_Pad1.0x1.5mm",
         pins={"1": "LED_K_IN", "2": "LED_K"},
         desc="Solder jumper across R4, closed by default. Cut it when R4 is fitted"),
    dict(ref="D1", sym="Device:D_Schottky", value="1N5819",
         fp="Diode_THT:D_DO-41_SOD81_P3.81mm_Vertical_CathodeUp",
         pins={"1": "+5V", "2": "+5VSB"}, dnp=True,
         desc="Optional Schottky diode in the +5VSB line (bridged by JP2)"),
    dict(ref="JP2", sym="Jumper:SolderJumper_2_Bridged", value="D1 bypass",
         fp="Jumper:SolderJumper-2_P1.3mm_Bridged_Pad1.0x1.5mm",
         pins={"1": "+5VSB", "2": "+5V"},
         desc="Solder jumper across D1, closed by default. Cut it when D1 is fitted"),
    dict(ref="BZ1", sym="Device:Buzzer", value="Active buzzer 12 mm",
         fp="Buzzer_Beeper:Buzzer_12x9.5RM7.6",
         pins={"1": "BUZZ", "2": "GND"}, desc="12 mm active buzzer, 5 V, pin spacing 7.6 mm"),
    dict(ref="J1", sym="Connector_Generic:Conn_01x03", value="ATX",
         fp="Connector_JST:JST_XH_B3B-XH-A_1x03_P2.50mm_Vertical",
         pins={"1": "PS_ON#", "2": "GND", "3": "+5VSB"},
         desc="JST XH 3-pin: ATX PS_ON# (green), GND (black), +5VSB (purple)"),
    dict(ref="J2", sym="Connector_Generic:Conn_01x04", value="BC-250",
         fp="Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical",
         pins={"1": "LED_A", "2": "LED_K_IN", "3": "PWR_HOT", "4": "RST_HOT"},
         desc="JST XH 4-pin: BC-250 power LED +, LED -, power button, reset button"),
    dict(ref="J3", sym="Connector_Generic:Conn_01x04", value="FRONT SWITCH",
         fp="Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical",
         pins={"1": "SW", "2": "GND", "3": "SW_LED", "4": "GND"},
         desc="JST XH 4-pin: front switch contact NO, common, LED +, LED -"),
    dict(ref="MH1", sym="Mechanical:MountingHole", value="M3", fp="MountingHole:MountingHole_3.2mm_M3",
         pins={}, desc="M3 mounting hole"),
    dict(ref="MH2", sym="Mechanical:MountingHole", value="M3", fp="MountingHole:MountingHole_3.2mm_M3",
         pins={}, desc="M3 mounting hole"),
    dict(ref="MH3", sym="Mechanical:MountingHole", value="M3", fp="MountingHole:MountingHole_3.2mm_M3",
         pins={}, desc="M3 mounting hole"),
    dict(ref="MH4", sym="Mechanical:MountingHole", value="M3", fp="MountingHole:MountingHole_3.2mm_M3",
         pins={}, desc="M3 mounting hole"),
]

PART = {p["ref"]: p for p in PARTS}
