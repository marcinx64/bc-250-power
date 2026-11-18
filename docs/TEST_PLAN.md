# Test status and procedure

"Passed" means a person ran the test on a real board and observed the result.
Automated tests are listed in `CONTRIBUTING.md`; this document covers what can
only be checked on hardware. Run the tests in order and stop at the first
failure. Report your own results by pull request.

## 1. Before any power (continuity)

| Check | Expect | Status |
|---|---|---|
| GND to 5V, pad R1 to R2 | open | passed |
| XIAO GND pin to U1 pin 2 | beeps | passed |
| GND to 3V3 | open | passed |
| GND to each of D2, D3, D4, D5 | open | passed |
| GND to U1 pins 5, 7 and U2 pins 2, 4, 5, 7 | beeps | passed |
| Front switch: R7 to R11 | open released, beeps pressed | passed |

## 2. USB only, bench test firmware (`firmware/bc250_test/`)

| Test | Status |
|---|---|
| Buzzer, 1 to 3 beeps (250 ms beeps, pin drive strength 3) | passed |
| Front switch shown as pressed | passed |
| Switch LED dimming and breathing | passed |
| PS_ON opto (L6 to ground) | passed: about 250 ohm on, open off |
| PWR opto (L9 to ground), RST opto (L10 to ground) | passed: brief dip on a pulse |
| LED sense (L4, L5) | passed with 3V3 through 470 ohm into L4 and L5 to ground (a multimeter's diode mode cannot light the opto) |
| Bluetooth detection next to WiFi | passed |

## 3. USB only, main firmware

| Test | Status |
|---|---|
| Setup wizard: set password, join WiFi | passed |
| Update over WiFi; settings, WiFi and password kept | passed |
| Scan lists game controllers and hides other devices | passed |

Open: use Debug, Simulated BC-250, so no PC is needed.

| # | Test | Expect |
|---|---|---|
| 1 | Short press while off | 1 beep, "starting", "on" after about 3 s, LED steady |
| 2 | Short press while on | 1 beep, "shutting down", "off" after about 5 s |
| 3 | Hold past the reset time while on, release | 2 beeps at the threshold, reset pulse on release, stays on |
| 4 | Hold past the forced-off time while on | 3 beeps, off at once |
| 5 | Hold the button while off | PC starts, nothing else, no hold timer on the page |
| 6 | Power on with Debug on Always dark | long beep and fast blink after 15 s, back to off |
| 7 | Add a controller that is switched on | PC stays off |
| 8 | Controller off for longer than the re-arm time, then on | PC starts |
| 9 | Shut down with the controller left on | PC stays off |
| 10 | Restart or update while "on" | dialog offers to shut down first, then carries on |
| 11 | Load pages while pressing the button | button reacts without delay |
| 12 | Log, 2 minutes after start | a "stack free" line |
| 13 | Hold the button while plugging in power, 10 s | 5 beeps and 5 LED flashes, password and WiFi erased, `BC250-Setup` appears |

## 4. Power supply alone

ATX +5VSB on R1, ground on R2, PS_ON# on L6, no USB.

| Test | Status |
|---|---|
| Controller starts from standby power and joins WiFi | passed |
| Power on from the UI with Debug on Always lit: supply starts | passed |
| Force off: supply stops | passed |
| Controller stays up while the supply switches | passed |

## 5. With the BC-250

| Test | Status |
|---|---|
| Power off by a short press: operating system shuts down, supply releases | passed |
| Reset by a hold past the reset time | passed |
| Forced off by a hold past the forced-off time | passed |
| Power on by the button | passed |
| Forgotten-password hold at power-up | passed |
| Controller wake (BLE and 2.4G/BT) | passed |
| Controller restart with the PC on | passed: PS_ON not released |
| Firmware update over WiFi with the PC on | passed: no interruption |
| Shut down from the operating system | open: supply releases, 1 beep |
| Log after a power-on shows whether a "start: PWR pulse" was needed | open: tells whether the BC-250 boots by itself on 12 V (AUTO_PWRON1 jumper) |
| Shut down with the controller left on | open: PC stays off |
| Sleep or suspend, if used | open: the LED must not blink; a blinking LED needs a longer power-off detect time |
| "Started after" on the Controller page after some days | open: still "Power connected" |
| Web UI power actions with the real PC | open: same results as the button |

## 6. Wake methods

| Test | Status |
|---|---|
| Switch between BLE and 2.4G/BT on the wake page, lists kept | passed |
| Learn: 8BitDo Ultimate 2, 8BitDo Ultimate 2C Wireless (2.4G mode), Rii mini i25 keyboard | passed |
| Wake by switching a listed device on, PC off | passed |
| Re-arm after a shutdown | passed: after the re-arm time |
| Rename, remove with confirmation, duplicate names refused | passed |
| Firmware update over WiFi in 2.4G/BT mode | passed |
| Bluetooth Classic: DualShock 4 paired with the BC-250 wakes it | passed |
| DualSense, Switch Pro | untested |
| Shut down with a 2.4G device left on and connected | open: PC stays off (the device must be unheard for the re-arm time first) |
| Several days in 2.4G/BT mode | open: no restart |

Free memory in 2.4G mode with no PC attached: about 52 KB idle, lowest about
12 KB over four uploads; no ping loss in a 10-minute soak; sightings of a
searching controller 1 to 17 s apart.

The current at the BC-250 LED pads has not been measured. If the sense ever
misbehaves, measure it; above about 20 mA fit 470 ohm in the anode wire.
