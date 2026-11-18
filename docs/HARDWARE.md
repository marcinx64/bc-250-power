# Hardware reference (perfboard build)

This page describes the hand-built perfboard controller. It is the tested
reference, and the firmware pin map follows it. A designed-but-not-yet-built
PCB (rev A) with the same circuit lives in [`../hardware/`](../hardware/README.md).
The machine-readable pin map is [`../hardware/hardware.json`](../hardware/hardware.json).

## Coordinates

All positions are `column, row` on the component side (top view):
columns 1 to 20 left to right, rows 1 to 14 top to bottom, with the XIAO's
USB-C socket pointing at row 1.

The board's own printing is on the underside and runs the other way:
printed numbers 1 to 20 and letters A to N. To convert:

- design column = 21 minus the printed number
- design row: N = 1, M = 2, L = 3, K = 4, J = 5, I = 6, H = 7, G = 8, F = 9,
  E = 10, D = 11, C = 12, B = 13, A = 14

Edge pads: 12 oval pads on each short side, L1 to L12 beside column 1 and
R1 to R12 beside column 20, numbered from the row 1 end. They do not line up
exactly with the 14 rows. Top and bottom pads are separate. External wires
are soldered to the top pads; a short link goes from each pad into the
nearest hole in column 1 or 20 and is joined to the circuit on the underside.

## Parts and where they sit

| Part | Holes | Notes |
|---|---|---|
| U1, TLP521-2 | pins 1 to 4 at 2,3 to 2,6; pins 8 to 5 at 5,3 to 5,6 | Pin 1 dot toward row 1, column 2 |
| U2, TLP521-2 | pins 5 to 8 at 2,9 to 2,12; pins 4 to 1 at 5,9 to 5,12 | Rotated 180 degrees: pin 1 at 5,12 |
| XIAO ESP32-C3 | D0 to D6 at 13,2 to 13,8; 5V, GND, 3V3, D10, D9, D8, D7 at 19,2 to 19,8 | Soldered flat on the board |
| R1, 220 R | 2,2 to 6,2 | PS_ON drive |
| R2, 220 R | 6,10 to 10,10 | PWR button drive |
| R3, 220 R | 6,12 to 10,12 | RST button drive |
| Buzzer | + at 9,4; - on the row 1 ground run near 9,1 | Active type, from a Grove Buzzer v1.2 |

The 220 R resistors are blue 5-band parts: red, red, black, black, brown.

TLP521-2 pinout: 1 anode A, 2 cathode A, 3 anode B, 4 cathode B,
5 emitter B, 6 collector B, 7 emitter A, 8 collector A.

## Netlist

| Net | Connects |
|---|---|
| PSON_DRV | XIAO D1 (GPIO3) to R1 to U1 pin 1 |
| PS_ON# | U1 pin 8 to pad L6, then the ATX green wire |
| LED_A | pad L4 to U1 pin 3 (BC-250 power LED anode) |
| LED_K | pad L5 to U1 pin 4 (BC-250 power LED cathode) |
| SENSE | U1 pin 6 to XIAO D3 (GPIO5), internal pull-up |
| PWR_DRV | XIAO D4 (GPIO6) to R2 to U2 pin 3 |
| PWR_HOT | U2 pin 6 to pad L9, then the BC-250 power button's live pad |
| RST_DRV | XIAO D5 (GPIO7) to R3 to U2 pin 1 |
| RST_HOT | U2 pin 8 to pad L10, then the BC-250 reset button's live pad |
| BUZZ | XIAO D2 (GPIO4) to buzzer + |
| SW | XIAO D7 (GPIO20) to pad R7, switch white wire |
| SW_LED | XIAO D10 (GPIO10) to pad R4, switch red wire |
| +5V | pad R1 to XIAO 5V pin. For the ATX +5VSB wire |
| GND | XIAO GND, pad R2 (ATX ground), pads R11 and R12, U1 pins 2, 5, 7, U2 pins 2, 4, 5, 7, buzzer - |

The power and reset buttons need only their live pad wired: the BC-250's
ground reaches this board through the power supply.

## Underside traces (design coordinates)

Signals:
- 2,2 to 2,3 (R1 to U1 pin 1)
- 6,2 along row 2 to 12,2, down to 12,3, to 13,3 (R1 to D1)
- 5,3 to 3,3, down column 3 to 3,7, along row 7 to 1,7 (PS_ON#, runs under U1)
- 5,5 along row 5 to 13,5 (sense to D3)
- 13,4 along row 4 to 9,4 (D2 to buzzer +)
- 13,6 along row 6 to 10,6, down column 10 to 10,10 (D4 to R2)
- 5,10 to 6,10 (U2 pin 3 to R2)
- 13,7 along row 7 to 11,7, down column 11 to 11,12, to 10,12 (D5 to R3)
- 5,12 to 6,12 (U2 pin 1 to R3)
- 1,5 to 2,5 and 1,6 to 2,6 (LED pads to U1 pins 3, 4)
- 1,10 to 2,10 and 1,12 to 2,12 (U2 pins 6, 8 to their pads)
- 19,2 to 20,2 (5V), 19,5 to 20,5 (D10), 19,8 to 20,8 (D7)

Ground:
- 2,4 to 1,4, up column 1 to 1,1, along row 1 to 18,1, down column 18 to 18,14
- 18,3 to 19,3 to 20,3 (XIAO GND and pad R2)
- 4,4 to 5,4; column 4 from 4,4 down to 4,14; 4,6 to 5,6
- row 9 from 2,9 to 5,9 and row 11 from 2,11 to 5,11
- row 14 from 4,14 to 20,14; 20,13 to 20,14

## Edge pads as built

| Pad | Link hole | Wire |
|---|---|---|
| L4 | 1,5 | BC-250 power LED anode (+) |
| L5 | 1,6 | BC-250 power LED cathode (-) |
| L6 | 1,7 | ATX PS_ON# (green) |
| L9 | 1,10 | BC-250 power button live pad |
| L10 | 1,12 | BC-250 reset button live pad |
| R1 | 20,2 | ATX +5VSB (purple) |
| R2 | 20,3 | ATX ground (black) |
| R4 | 20,5 | Front switch red (LED +) |
| R7 | 20,8 | Front switch white (contact, normally open) |
| R11 | 20,13 | Front switch yellow (contact common) |
| R12 | 20,14 | Front switch black (LED -) |

## Front switch

Five-wire pigtail, identified with a multimeter:

- red and black: LED
- yellow: contact common
- white: normally open (closes while pressed)
- green: normally closed. Not used; cut short and insulated

## BC-250 connections

- LED pads: the left pad is the anode (+) and the right pad the cathode (-)
  (red probe on the left pad, black on the right, positive reading). The
  sense wiring is anode to pad L4, cathode to pad L5.
- The power LED reads about 2 V across its pads, the other LED about 3 V.
  These are most likely the forward voltages of the fitted LEDs. 2 V is
  enough for the opto input, which needs about 1.15 V.
- Pads L9 (PWR) and L10 (RST) are confirmed by measurement.

## Design notes

- Each opto input gets about 10 mA: (3.3 V - 1.15 V) / 220 R.
- The optos do not isolate anything here, because all grounds are common.
  They act as open-collector switches that cannot back-feed the BC-250.
- No series resistor is fitted in the LED-sense line. With the BC-250's own
  LED removed, the board's existing LED resistor limits the opto's input
  current. If that current measures above about 20 mA, fit 470 R inline in
  the LED anode wire. (PCB rev A has an optional 470 R footprint with a
  bypass jumper for this.)
- The XIAO is soldered flat, with the column 18 ground wire running under
  it. Check GND against 3V3 and against D2 to D5 for shorts.
- The 5V pin is the XIAO's USB VBUS. Never connect USB and ATX +5VSB
  together unless a Schottky diode is added in the +5VSB wire. (PCB rev A
  has an optional diode with a bypass jumper for this.)
- The design needs a supply with +5VSB (ATX style). A plain 12 V brick has
  none.
- The buzzer does not sound reliably from GPIO4 at the default drive
  strength. The firmware sets the strongest drive (about 40 mA). Beeps
  are 250 ms on, 200 ms apart.
- The switch's LED works from the 3.3 V pin with PWM.
- A multimeter's diode test may not light the opto input. Test the sense
  with 3V3 through 470 R into L4 and L5 to ground.

## Open questions

- The current the BC-250 LED pads supply. Above about 20 mA, fit 470 R.
- Which BC-250 LED is steady while running and off after shutdown. The
  firmware's idea of "PC is on" depends on it.
- Whether the BC-250's AUTO_PWRON1 jumper is on pins 1-2 (boots when 12 V
  arrives) or 2-3 (waits for the power button).
