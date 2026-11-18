# Sniffer toolkit

Tools to find out how a 2.4 GHz device (game controller, keyboard, dongle
device) announces itself when you switch it on, so that the BC-250 power
controller can wake the PC from it. The guide is `docs/SNIFFING.md`; this
page lists the commands.

All tools are Python scripts with inline dependencies. Install
[uv](https://docs.astral.sh/uv/) and run them directly:

```
uv run tools/sniffer/selftest.py
```

Nothing needs hardware except `survey record`, `capture` and `espsdr info`.

## Hardware

- A board running [esp-sdr](https://github.com/ESPARGOS/esp-sdr) by ESPARGOS
  (GPL-3.0-or-later): an ESP32-S3 dev board or an M5Stack Cardputer, flashed
  with the browser installer at https://espargos.net/espsdr/. It shows up as
  a USB serial port. Pass it with `--port`, or set `ESPSDR_PORT`; by default
  the first USB serial port is used.
- Or the controller itself, with `ctrlcap.py` (2.4G/BT wake method, WiFi
  connected): 1 ms snapshots of the WiFi channels, no extra board.
- Or any SDR (HackRF, PlutoSDR, ...) that can write SigMF. `analyse`,
  `falsewake` and `sigmfio` read `ci8`, `ci16_le` and `cf32_le`; the rate may
  be anything that is a multiple of 1 MS/s. Use 16 MS/s for files meant for
  `rfreplay` (`sigmfio.py resample` converts).

## Recording format

SigMF: `NAME.sigmf-meta` (JSON) and `NAME.sigmf-data`. Every snapshot is an
entry of `captures` with its own `core:frequency` (Hz) and `core:sample_start`.
The firmware's unit is a snapshot of 16380 samples (1.02 ms at 16 MS/s).
Extra fields are namespaced `bc250:` (gain mode, time of each snapshot).

I/Q convention: standard complex samples, a signal above the centre
frequency has a positive offset. The esp-sdr board sends `(I, -Q)` pairs; the
tools convert them (`espsdr.to_standard`) before writing, so the files are
standard.

Recordings contain real device addresses. `.gitignore` keeps them out of
git; do not publish them.

## Commands

| Command | What it does |
|---|---|
| `espsdr.py info [--port P]` | Talk to the board, print what it supports |
| `survey.py record NAME [SECONDS]` | Max-hold spectrum 2400-2480 MHz, 80 MS/s; writes `captures/NAME.npz` and a waterfall `NAME.png` |
| `survey.py compare BEFORE AFTER` | What got stronger after the device was switched on |
| `survey.py bursts NAME MHZ...` | Timing of bursts on given frequencies |
| `capture.py NAME SECONDS --centre 2464 [--rate 16] [--gain hw]` | Raw I/Q snapshots to SigMF; `--centre 2437,2468` visits several centres in turn |
| `ctrlcap.py [--host H] [--channels 1-13] [--count N] [--interval S] [--out NAME]` | Records snapshots with the controller itself (`GET /api/rf/snapshot`), one per request, channels in turn; SigMF `ci16_le` |
| `analyse.py NAME` | Bursts, carrier, bit rate, bits; tests ShockBurst/ESB, Bluetooth Classic, BLE; lists unknown preamble/sync candidates |
| `synth.py esb\|bt\|ble\|noise OUT` | Synthetic SigMF recordings |
| `falsewake.py calc` / `scan` | False wake probability: formula, and a test on a background recording |
| `sigmfio.py info\|resample` | Look at, or resample, a recording |
| `selftest.py` | synth -> analyse -> falsewake checks, no hardware |

## Examples

Find the channels (device off, then on, dongle unplugged):

```
uv run tools/sniffer/survey.py record off 20
uv run tools/sniffer/survey.py record on 20      # switch the device on in the first seconds
uv run tools/sniffer/survey.py compare off on
```

Capture 60 s around a strong frequency, tuned about 4 MHz below it, then analyse:

```
uv run tools/sniffer/capture.py mydev 60 --centre 2464
uv run tools/sniffer/analyse.py mydev
```

Try the tools without hardware, and a recording you can feed to `rfreplay`:

```
uv run tools/sniffer/selftest.py
uv run tools/sniffer/synth.py esb /tmp/test --addr A1B2C3D4E5
uv run tools/sniffer/analyse.py /tmp/test
```

False wake numbers:

```
uv run tools/sniffer/falsewake.py calc --bits 48 --errors 2
uv run tools/sniffer/falsewake.py scan background --key esb:A1B2C3D4E5 --errors 2
uv run tools/sniffer/falsewake.py scan background --key bt:9A8B7C --errors 3
```

## Limits

- `analyse` finds frames with a valid CRC. The firmware also accepts frames
  whose preamble and address match with a few wrong bits; those do not
  show up as ShockBurst hits here, but as repeated lines in the "unknown"
  list.
- Bit rate is estimated from the occupied bandwidth (about 0.39 x bit rate
  for the usual GFSK). If nothing is found, `analyse` also tries 1 and
  2 Mbit/s.
- A plain-ShockBurst hit (no packet control field) is ambiguous in address
  width: the same frame parses with 3, 4 or 5 bytes.
- The synthetic signals are clean; real recordings are harder. Expect fewer
  decoded frames than bursts.

## Credits

The capture engine and its registers come from esp-sdr (ESPARGOS, GPL-3.0-or-later).
Bluetooth Classic sync word handling follows the Core specification, Vol 2 Part B 6.3.3.1.
