# 2.4G dongle-mode wake

With the wake method set to 2.4G, the firmware wakes the PC when a device
with a **2.4G USB dongle** is switched on, instead of a Bluetooth device.
Tested: 8BitDo Ultimate 2 and Wireless 2C game controllers in 2.4G mode, and a
Rii mini i25 keyboard. Anything speaking Nordic Enhanced ShockBurst at
1 Mbit/s with a 5-byte address and a 16-bit CRC should work (many dongle
devices use Nordic or compatible chips); devices with other radio settings or
protocols will not be found by Learn. Other packet formats are added as
protocol modules (`docs/ADDING_PROTOCOLS.md`); how to find out what a device
sends: `docs/SNIFFING.md` and `tools/sniffer/`.

Bluetooth is off in this mode (its memory goes to the heap at start). The
wake method is chosen on the wake page and applies after a restart; settings
and all wake lists are kept. Use: `docs/USER_GUIDE.md`; API:
`docs/FINAL_FIRMWARE_SPEC.md` section 23. Classic Bluetooth controllers
(DualShock 4 ...) work in the same mode: `docs/WAKE_BT_CLASSIC.md`.

## Structure

The 2.4G/BT code is split so that a new packet format does not touch the
rest (`docs/ADDING_PROTOCOLS.md`):

| Part | Files | Role |
|---|---|---|
| Core | `src/rf.cpp` | channel visits, capture, wake lists in NVS, Learn, wake rules |
| Signal processing | `src/rf_dsp.cpp` | bursts, carrier offset, FM demodulator |
| Detector and registry | `src/rf_detect.cpp`, `rf_proto.cpp` | runs the registered protocols over each burst |
| `esb1m` | `src/proto_esb1m.cpp`, `proto_esb.h` | this page |
| `btpage` | `src/proto_btpage.cpp`, `btc.h` | `docs/WAKE_BT_CLASSIC.md` |
| `esb2m` (experimental, off) | `src/proto_esb2m.cpp` | Nordic ESB at 2 Mbit/s, template for new protocols |

Everything except the core compiles on a PC and is tested there
(`host_test/`, `make test`).

## What the controller sends

- Nordic Enhanced ShockBurst: 1 Mbit/s GFSK, preamble 0x55 or 0xAA (0xAA
  when the address starts with a 1 bit), 5-byte address, 9-bit packet
  control field, payload, CRC-16 (CCITT, init 0xFFFF).
- With the dongle unpowered (PC off) the controller searches for about a
  minute after switching on, sending a packet every few ms. It hops over
  much of the band (seen: 2414 to 2475 MHz), so there is no single channel
  to watch.
- Each controller has its own address. Seen here (examples, not real
  devices): `A1 B2 C3 D4 E5`, `5A 6B 7C 8D 9E`.

## How the board listens

- Only while WiFi is connected as a station and the PC is off (or while
  learning). The WiFi driver is asked to visit a WiFi channel for a moment
  (`esp_wifi_remain_on_channel`); the board takes one 1 ms raw snapshot
  there with the radio's capture engine and returns. Channels 1..13 (as the
  country setting allows) are swept in turn: about 23 visits a second when
  watching, 30 when learning, which leaves WiFi about 90 % of the time on its
  own channel. The receiver is never tuned or calibrated by this code; the
  WiFi driver does that, so WiFi keeps working. While a WiFi scan, a roam or
  an update runs, no visit is made.
- Each snapshot: bursts found by their envelope (3x the median, at least
  75 us for ESB), carrier offset from a 250 kHz-step DFT (1 to 7.25 MHz either
  side of the centre), FM demodulation at that offset, one bias per burst,
  all 16 sample phases sliced.
- `esb1m` counts a packet when the whole ESB frame has a valid CRC (learning
  finds addresses this way), or, for a listed address or one Learn has
  already found with a valid CRC, when the preamble and address match with 2
  or fewer wrong bits of 48 (random data, 2 wrong bits: ~4e-12 per
  position). Long packets often fail the CRC: in a 2-minute Learn, 58 packets
  had a valid CRC and 108 more matched by address (Ultimate 2: 33 valid;
  Rii mini i25: 96). Of the demodulated burst, 5200 samples are kept: frames
  with a payload up to about 25 bytes are decoded completely, longer ones are
  found by their address only.
- Wake: one CRC-valid frame, or two address matches within 5 s after the
  entry armed; a single address match only waits for the second. Spare board,
  simulated PC, Rii switched on 6 times: all 6 woke, 4 by a CRC frame at the
  first catch, 2 by a second match 0.2 s and 2.7 s after the first. Of 58
  catches, 39 were CRC frames and 19 address matches (0 wrong bits 12, 1 bit
  2, 2 bits 3, 3 bits 2). Why 2 wrong bits and two matches: the false-match
  arithmetic is in `docs/ADDING_PROTOCOLS.md` section 5 (about one false wake
  in 500 years in the worst case).
- Channel visit rate, Rii searching, two rates alternating every 10 s for 7
  minutes: 0.93 catches a second at ~23 visits/s against 0.51 at half the
  rate; first catch after a window started: median 0.7 s against 1.2 s,
  longest 7.4 s against 8.2 s; longest gap between catches 5.8 s against
  7.2 s. The longest waits are quiet spells of the controller itself, so a
  faster rate shortens them little. Visiting every third channel caught no
  more per snapshot (4.1 % against 4.5 %), so every channel is visited.
- Signal strength (tried, not kept): a packet's burst level over the
  snapshot's median level does not follow distance. Rii mini i25 at 10 cm,
  1 m, 3 m and in the next room: typical 16, 18.5, 15.5 and 15 dB, each spread
  over about 10 to 27 dB. The receiver gain is set by the WiFi driver for
  each channel visit and cannot be read, so no signal bars are shown in 2.4G
  mode.
- Arming (as Bluetooth): an entry arms after `ble_rearm_s` unheard, counted
  from the PC turning off, from the start, or from its last sighting; a newly
  added entry counts as just heard. Measured with both controllers searching:
  sightings 1 to 17 s apart (typically 1 to 5 s). A Re-arm time under about
  20 s can arm a controller that is still searching.

## Memory

- The capture engine writes to 0x3fcb0000 (64 KiB). The whole 128 KiB
  bank 0x3fca0000..0x3fcc0000 is kept out of the heap
  (`SOC_RESERVE_MEMORY_REGION` in `rf.cpp`); `tools/ram_guard.py` stops the
  build if static RAM or code in RAM would reach it.
- The lower 64 KiB is "quiet memory" (`quietmem.h`): buffers only the CPU
  touches, never during a capture, live there (the demodulator's working
  buffers, event log, the control task's and the main loop's stacks). Never
  the rf task's stack: it runs the capture, and with its stack there no burst
  was found at all.
- Memory in 2.4G mode (ESP-IDF build, `docs/ESP_IDF_BUILD.md`): idle ~52 KB
  free; lowest 12.2 KB over four firmware uploads over WiFi. An upload needs
  up to ~40 KB for WiFi buffers while flash writes run.

## Recording snapshots

`GET /api/rf/snapshot?ch=N` returns one raw 1 ms snapshot of WiFi channel N
(the rf task takes it; the wake scan is paused until it is sent).
`tools/sniffer/ctrlcap.py` uses it to record SigMF files for `rfreplay` and
`analyse.py`. See `docs/SNIFFING.md`.

## Caveats

- The capture engine's registers are undocumented; they come from the esp-sdr
  project (https://github.com/ESPARGOS/esp-sdr, GPL-3.0-or-later). The
  licence of this repository is in the README.
- Tested in one room with two 8BitDo controllers and a Rii mini i25
  keyboard, on a spare XIAO and on the board in the case. Other devices,
  other rooms and the experimental 2 Mbit/s protocol are untested on air.

## Status

Stable, on par with Bluetooth wake: same settings, same arming rule, and the
same Re-arm time.

- [x] Detection of known devices, WiFi coexistence, memory budget
- [x] Learn a new device from the web UI; rename, remove
- [x] Wake list stored in NVS, wake with the arming rules
- [x] Web UI; switching between Bluetooth and 2.4G on the wake page
- [x] Firmware updates over WiFi
- [x] Modular protocols; detectors tested on a PC with synthetic signals
- [ ] 2 Mbit/s ESB (`esb2m`): synthetic signals only
- [ ] On-air test of the modular build
