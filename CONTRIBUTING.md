# Contributing

Thanks for helping. This project controls a power supply and a PC, so please
read the safety rules at the end first.

## Ways to contribute

| What | Where to start |
|---|---|
| Translate the web UI | [docs/TRANSLATING.md](docs/TRANSLATING.md) |
| Report a device that works (or does not) | [docs/SNIFFING.md](docs/SNIFFING.md), [devices/README.md](devices/README.md), the ["New device" issue form](https://github.com/marcinx64/bc-250-power/issues/new/choose) |
| Add a wake protocol | [docs/ADDING_PROTOCOLS.md](docs/ADDING_PROTOCOLS.md) |
| Hardware changes (PCB, BOM, wiring) | [hardware/README.md](hardware/README.md) |
| Fix a bug or improve the firmware or UI | read [AGENTS.md](AGENTS.md) and [docs/FINAL_FIRMWARE_SPEC.md](docs/FINAL_FIRMWARE_SPEC.md) |
| Improve documentation | edit the file and open a pull request |

Bugs and questions go to the [issue tracker](https://github.com/marcinx64/bc-250-power/issues); changes come as
pull requests against `main`.

## Build

Needs [uv](https://docs.astral.sh/uv/) (PlatformIO runs through `uvx`).

```sh
git clone https://github.com/marcinx64/bc-250-power.git
cd bc-250-power/firmware/bc250_controller
uvx platformio run
```

Flashing and updating: [docs/FLASHING.md](docs/FLASHING.md). Raise
`FW_VERSION` in `src/version.h` for every change that is installed on a board.

## Test

Run what your change touches, and say so in the pull request.

```sh
# web UI against a mock controller (needs Python 3 and bun), see ui_test/README.md
(cd firmware/bc250_controller/ui_test && bun install && bash run_all.sh)

# wake-detector selftest of the sniffer toolkit (needs uv)
uv run tools/sniffer/selftest.py

# wake-protocol host tests
make -C firmware/bc250_controller/host_test test

# translation catalogs complete and consistent
python3 tools/i18n.py check

# device list valid
python3 tools/devices.py check
```

Hardware behaviour can only be checked on a board. Do not claim a hardware
test passed unless you ran it; state which firmware version and setup you used.

## Commits and pull requests

- One topic per commit. Short imperative subject line, for example
  `wake: accept two address matches within 5 s`; explain why in the body.
- Keep [docs/FINAL_FIRMWARE_SPEC.md](docs/FINAL_FIRMWARE_SPEC.md) in step with
  the code when behaviour or the API changes.
- Documentation describes the current state: no changelogs, version history,
  dates or session notes. Version-specific compatibility facts are fine when
  users need them.
- No personal data: no real device addresses, WiFi names, passwords, MAC or IP
  addresses of your own network. Use made-up values such as `A1B2C3D4E5`.

## Licences of contributions

By contributing you agree that your work is licensed under the same terms as
the part it belongs to (texts in [LICENSES/](LICENSES/)):

- firmware, web UI, tools: GPL-3.0-or-later
- hardware (KiCad files, schematics, renders, gerbers): CERN-OHL-S-2.0
- documentation, photos, images: CC-BY-SA-4.0

Translations count as part of the web UI (GPL-3.0-or-later).

## Safety rules

- Never connect USB and the ATX +5VSB wire at the same time.
- GPIO3, GPIO6 and GPIO7 switch a power supply and a PC's buttons: only the
  power state machine may drive them high, and all outputs are low first
  thing at start.
- Do not change the restart or boot path without re-reading spec section 17
  (PS_ON latch).
- Do not use GPIO2, GPIO8, GPIO9 (strapping) or GPIO21, and never start UART0
  (its RX pin is the front switch).
- Keep the pin numbers: they match built hardware.

The full list is in [AGENTS.md](AGENTS.md).
