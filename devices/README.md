# Devices

One JSON file per device, checked against `schema.json`. The table in the
main README is generated from these files.

```
python3 tools/devices.py check     # validate
python3 tools/devices.py table     # print the Markdown table
python3 tools/devices.py readme    # update README.md between <!-- devices:start --> and <!-- devices:end -->
```

## Fields

| Field | Meaning |
|---|---|
| `id` | Lower case, dashes; same as the file name without `.json` |
| `vendor`, `model` | As printed on the device |
| `kind` | `gamepad`, `keyboard`, `mouse`, `remote`, `headset`, `other` |
| `modes` | One entry per radio mode the device has (a controller may have Bluetooth and a 2.4G dongle mode) |

Each mode:

| Field | Meaning |
|---|---|
| `name` | The mode as the user sees it, e.g. `2.4G dongle` |
| `wake_method` | `ble` (Bluetooth LE advertiser), `2g4-esb1m` (Nordic ShockBurst, 1 Mbit/s), `bt-classic-page` (Bluetooth Classic paging its host), `other` |
| `protocol` | The firmware's protocol id: `ble` (BLE advertising), `esb1m` (Nordic ESB 1 Mbit/s), `esb2m` (ESB 2 Mbit/s, experimental build), `btpage` (Bluetooth Classic paging), or `unknown`; see docs/ADDING_PROTOCOLS.md |
| `status` | See below |
| `tested_on` | Firmware version of the result, or `null` |
| `notes` | What is special; keep to facts you measured |
| `how_to_add` | What the user does in the web UI |

## Status

| Status | Meaning |
|---|---|
| `works` | Tested in the console, wakes the PC |
| `works-untested-on-console` | Detection and wake work on a spare board; not yet tried in the case |
| `expected` | The protocol is supported and the device should behave the same, nobody has tried |
| `help-wanted` | Protocol unknown or not supported; recordings wanted |
| `does-not-work` | Tried and failed, with the reason in `notes` (for example a key that changes every session) |

## Adding a device

1. Find out how it announces itself: `docs/SNIFFING.md`.
2. Copy a similar file, change the fields. Do not put real addresses in
   it: the address of a device is private to its owner.
3. Run `python3 tools/devices.py check`.
4. Open a pull request, or a "new device" issue with the same information
   and, if the protocol is new, a recording (strip the addresses first or
   say that you can share it privately).
