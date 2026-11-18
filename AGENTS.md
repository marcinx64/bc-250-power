# Rules for AI agents and contributors

Firmware for a Seeed XIAO ESP32-C3 that switches an ATX supply and a BC-250
PC (`README.md` has the overview, `CONTRIBUTING.md` the contribution flow).
The firmware is plain ESP-IDF 6.1.1 built with PlatformIO in
`firmware/bc250_controller/`; the version is in `src/version.h`.

## Read first

1. `docs/FINAL_FIRMWARE_SPEC.md`: behaviour and HTTP API. Keep it in step with
   the code when you change either.
2. `docs/FLASHING.md` and `docs/ESP_IDF_BUILD.md`: build and flash commands.
3. `docs/HARDWARE.md` if you need to know what a pin does.
4. `docs/TRANSLATING.md`, `docs/ADDING_PROTOCOLS.md` for UI text and wake
   protocols.

## Build and test commands

```sh
# build (raise FW_VERSION in src/version.h first when the build is to be installed)
cd firmware/bc250_controller && uvx platformio run

# web UI tests: scripted against a mock, see firmware/bc250_controller/ui_test/README.md
# wake-protocol host tests
make -C firmware/bc250_controller/host_test test
# translation catalogs
python3 tools/i18n.py check
# device list
python3 tools/devices.py check
```

Built files go to `firmware/releases/firmware-<version>.bin` (not committed).
Flash over USB only when the maintainer says the board is connected by USB
and nothing else is connected to it.

## Honesty about testing

The firmware is usually only compiled before it reaches a board. Say so.
Do not report hardware behaviour as working, or a hardware test as passed,
unless the maintainer or a named tester ran it. UI tests run against a mock
in jsdom; layout and styling are checked by eye in a real browser.

## Safety rules for this hardware

- Never have USB and the ATX +5VSB wire connected at the same time. The
  XIAO's 5V pin is its USB VBUS.
- All outputs LOW first thing at start. Nothing may drive GPIO3, GPIO6 or
  GPIO7 high except the power state machine. They switch a power supply and
  press a PC's power and reset buttons.
- A deliberate restart or update keeps the PC on by latching GPIO3 (spec
  section 17). Do not change the boot path or the restart path without
  re-reading that section: on a resume PSON must never be driven low, and the
  latch must be released on every boot. A crash still cuts the PC's power.
- Do not use GPIO2, GPIO8 or GPIO9 (strapping pins) or GPIO21.
- Never start UART0: its RX pin is GPIO20, the front switch input. The
  console is USB-Serial-JTAG only.
- The buzzer needs `gpio_set_drive_capability(GPIO_NUM_4, GPIO_DRIVE_CAP_3)`.
- Keep the pin numbers. They match soldered hardware.

## Code layout

- `src/`: C++ modules. Tasks: control (button, power state machine, buzzer,
  LED; every 5 ms), the wake radio tasks (Bluetooth or 2.4G capture), and the
  main task (WiFi and web server). Lock order is documented in `eventlog.h`
  and `ble.cpp`. Nothing in the control task may block.
- `ui/index.html`, `ui/setup.html`: self-contained pages, embedded into the
  firmware. No external resources. The firmware-file marker text must not
  appear literally in `index.html` (spec section 16).
- Partition table and bootloader settings must stay compatible with devices
  in the field (`docs/ESP_IDF_BUILD.md`).

## Documentation and privacy

- Docs state the current behaviour: no version history, changelog remarks,
  dates or session notes.
- No personal data in the tree: use made-up device addresses (for example
  `A1 B2 C3 D4 E5`), example SSIDs and private-range example IPs.
