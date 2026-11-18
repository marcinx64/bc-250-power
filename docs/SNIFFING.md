# Adding a new controller: sniffing guide

How to find out whether a device can wake the BC-250, and what the controller
must listen for. Tools: `tools/sniffer/` (commands in its README).

## Which path?

- **The device is a Bluetooth LE advertiser** (most controllers in their
  Bluetooth mode): nothing to do here. Use Scan in the web UI with the wake
  method set to Bluetooth.
- **The device has a 2.4G USB dongle**, or is a **Bluetooth Classic**
  controller (DualShock 4, DualSense, Switch Pro): this guide. First check
  `devices/README.md` and `docs/WAKE_2G4.md` / `docs/WAKE_BT_CLASSIC.md`; it
  may already work with Learn.

The use case matters: the PC is off, so the dongle is unpowered. You record
the device switched on **with the dongle unplugged**. It searches for its
dongle and sends the same packets that the board must recognise.

## What you need

- An esp-sdr board: any ESP32-S3 dev board or an M5Stack Cardputer flashed
  with [esp-sdr](https://github.com/ESPARGOS/esp-sdr) (ESPARGOS,
  GPL-3.0-or-later) through the browser installer at
  https://espargos.net/espsdr/. The same page has a viewer. esp-sdr uses the
  radio's undocumented capture engine; the 2.4G wake of this project uses
  the same registers.
- Or a HackRF, PlutoSDR or other SDR that can write SigMF. Record 2.4 GHz at
  16 MS/s or more, complex samples, with the centre frequency in each
  recording.
- A computer with [uv](https://docs.astral.sh/uv/). Install nothing else:
  `uv run tools/sniffer/<tool>.py` fetches numpy, pyserial and matplotlib.
- Try the tools first without hardware: `uv run tools/sniffer/selftest.py`.

### Recording with the controller itself

The controller can record snapshots for you, so no extra SDR is needed.

Requirements:

- The wake method is 2.4G/BT, and WiFi is connected (not the setup access
  point).
- You can log in to the controller (a password, if you set one).
- Nothing else is using the radio: no firmware upload, no WiFi scan.

Limits:

- A snapshot is 1 ms long, at 16 MS/s, within +-8 MHz of the centre of one
  WiFi channel (channel 1 is 2412 MHz, channel 13 is 2472 MHz). A device on
  2480 MHz is out of reach.
- The receiver gain is set by the WiFi driver at each visit. You cannot
  change it.
- It uses the antenna of the controller board, so put the device close to it.
- Each snapshot takes the radio away from WiFi for a few milliseconds, like
  a normal wake-scan visit. Request snapshots a few times a second at most.

Record while you switch the device on:

```
uv run tools/sniffer/ctrlcap.py --host bc250.local --count 300 --out mydevice
```

It asks for the password if one is set (or use `--password`, or the
`BC250_PASSWORD` variable). `--channels 1,6,11` picks channels, `--interval`
sets the pause between requests. Then replay or analyse the recording like
any other:

```
uv run tools/sniffer/analyse.py mydevice
firmware/bc250_controller/host_test/build/rfreplay mydevice
```

`analyse.py` shows the address; `rfreplay` shows what the controller's own
detectors would do with the recording.

## Method

### 1. Survey: where does it transmit?

Record the band twice, device off and on, dongle unplugged. Switch the
device on a few seconds after the recording starts.

```
uv run tools/sniffer/survey.py record off 20
uv run tools/sniffer/survey.py record on 20
uv run tools/sniffer/survey.py compare off on
```

`compare` lists the frequencies that got stronger. Look at the waterfall
(`captures/on.png`): a device searching for its dongle shows narrow lines
(about 1 MHz) 20 to 30 dB above the background, repeating every few
milliseconds. Note whether one channel appears on every switch-on (a fixed
channel, easy) and others change (hopping). `survey.py bursts on 2468` gives
the timing. Repeat 5 times; fixed channels show up every time.

Switch-on can give a different channel every time and WiFi can hide a
channel: record with the WiFi of your own network on a channel far from it.

### 2. Capture raw samples

Tune about 4 MHz below a strong frequency so the signal stays away from the
receiver's DC spike (the window covers +-8 MHz at 16 MS/s):

```
uv run tools/sniffer/capture.py mydevice 60 --centre 2464
```

Switch the device on after the start; some devices search only for about a
minute. For a device with several channels visit them in turn:
`--centre 2437,2464`.

### 3. Analyse

```
uv run tools/sniffer/analyse.py mydevice
```

It finds the bursts, measures the carrier offset, FM-demodulates, estimates
the bit rate (0.5, 1, 2 Mbit/s), slices bits at every sample phase and tests:

- **Nordic ShockBurst / Enhanced ShockBurst**: preamble 0x55 or 0xAA,
  address of 3 to 5 bytes, 9-bit control field, CRC-8 or CRC-16. A hit
  whose CRC is valid is certain (CRC-16: 1 in 65536 by chance; CRC-8 is
  weak). The address is the key.
- **Bluetooth Classic access codes**: 64-bit sync words; the tool reads the
  LAP back from any valid one.
- **BLE advertising** (access address 8E89BED6): a known other; use Scan.
- **Unknown**: the bits after the first preamble-like alternating run, best
  sample phase, counted over all bursts. A line that repeats many times is
  a sync word or an address. Compare the lines: constant part = key,
  changing part = payload.

If nothing is recognised: the bit rate may be different (try
`--bitrate 2` or `0.5`), the device may use another modulation (the tools
handle GFSK only), or the recording is too weak (move the board closer). A
new protocol needs a detector module: see `docs/ADDING_PROTOCOLS.md`.

### 4. Find the stable key

The wake list needs something that is the same every time the device is
switched on and different from other devices. Check it:

1. **Same on every switch-on.** Record 5 switch-ons; the address / LAP must
   be identical.
2. **Survives power cycles and a few hours**; batteries changed.
3. **What changes when you re-pair** the device with its dongle or host.
   Many devices get a new address; then the old entry must be learned again
   (note this in the device file).
4. **Which payload bytes vary** between switch-ons or when you move the
   sticks. Wake must not depend on them. (In the examples a header byte and
   the checksum differed between switch-ons; the address did not.)

A device whose key is random in every session cannot be used.

### Worked example 1: 8BitDo Ultimate 2, 2.4G dongle mode

Survey, dongle unplugged: two narrow channels about 30 dB over the
background, bursts every 6 to 7 ms. One channel, 2468 MHz, appeared on 8 of
8 switch-ons; the second changed each time. The device searched for about a
minute and then gave up.

Capture at 16 MS/s centred 4 MHz below, `analyse`: 1 Mbit/s GFSK, Nordic
Enhanced ShockBurst:

```
ESB address A1 B2 C3 D4 E5 (5 B, CRC-16, preamble AA)
payload B0 88 72 00 00 00 00 FF 7F FF 7F FF 7F FF 7F 00 00 A2 07
```

(The address is anonymised here.) Length 19, no-ack set. The payload looks
like an ordinary input report (four 16-bit sticks at centre, buttons 0): the
controller simply sends normal reports to its paired address. Between two
switch-ons one header byte and the last byte changed (a session value and a
checksum); the address did not. So the key is (address), the channel need
not be known: the board visits every WiFi channel and finds it.

The Wireless 2C is the same framing with its own address (`5A 6B 7C 8D 9E`
here) on a fixed channel near 2436.5 MHz and a weaker signal. Every device
has its own address and its own fixed channel.

### Worked example 2: DualShock 4 paging its host

A paired controller switched on pages its host: Bluetooth Classic ID
packets, 68 us, 1 Mbit/s, hopping over 32 channels at 3200 hops per second.
`analyse` shows:

```
Bluetooth Classic access codes ...
    57x  LAP 9A8B7C  wrong bits 0/1/2 = 35/..  channels MHz: 2403 ... 2470
     9x  LAP 4D5E6F  ...
```

57 catches in 15 s of paging, 35 of them without a wrong bit, across 2403 to
2470 MHz. `9A8B7C` is the LAP of the **host** the controller was paired with
(the lower 24 bits of its address), not of the controller. `4D5E6F` was a real link
of another device nearby (a phone and its headset): other Bluetooth
traffic appears too, so the right entry is the one that appears when the
controller's button is pressed. The controller pages for about 14 s and then
turns off.

## Avoiding false console wakes

A false wake starts the PC, perhaps at night, with nobody there. This is
the thing to get right when adding a device.

Rules:

1. **Key of at least 40 bits.** ESB: preamble + 5-byte address = 48 bits.
   A 3-byte address (24 bits + 8) is too short for the approximate match;
   use exact matching only.
2. **Keep the wrong-bit budget low.** Each allowed wrong bit multiplies the
   chance by about n/e (see the numbers below). Allow only as many as real
   signals need: measure with `analyse` how many wrong bits the genuine
   packets have.
3. **Require evidence.** A CRC-valid frame (CRC-16) wakes alone; an exact
   key match wakes alone; an approximate match needs a second match within a
   few seconds (5 s in the firmware).
4. **Arming and re-arm time.** An entry arms only after it was unheard for
   the re-arm time, so a device that is already on (or a neighbour's) does
   not repeat wakes. Do not set the re-arm time below about 20 s for
   devices that search for their dongle.
5. **Test against a background recording**: at least 30 to 60 minutes in
   the real, busy 2.4 GHz environment (WiFi, BLE, other dongles, a
   microwave oven running), device off:

   ```
   uv run tools/sniffer/capture.py background 3600 --centre 2437,2464
   uv run tools/sniffer/falsewake.py scan background --key esb:A1B2C3D4E5 --errors 2
   ```

   It reports the best (lowest) distance between your key and any window
   in the recording, how many windows came within the limit and within +1
   and +2 bits, and what chance alone predicts. Aim for a margin
   (best distance minus limit) of at least 3 bits; with a margin of 1 or
   less, lower the limit or pick another device.

### The numbers

The probability that a random window of n bits matches the key with at
most e wrong bits is the sum over k up to e of C(n,k) / 2^n.

```
uv run tools/sniffer/falsewake.py calc --bits 48 --errors 2
uv run tools/sniffer/falsewake.py calc --bits 64 --errors 3 --positions 100
```

Assumptions: 16 sample phases x 200 start positions per burst (ESB) or 100
(Bluetooth), 20 bursts per second from other traffic, entry always armed,
second match within 5 s. These are upper bounds (neighbouring phases are not
independent); measure your own burst rate with `falsewake.py scan` and give
it with `--bursts-per-s`.

| Pattern | p per window | False matches | False wakes without confirmation | With the firmware rule |
|---|---|---|---|---|
| ESB: preamble + 5-byte address, 48 bits, up to 2 wrong | 4.2e-12 | 0.023 per day | 8.4 per year | 7e-3 per year (one per 139 years) |
| Bluetooth Classic sync word, 64 bits, up to 3 wrong | 2.4e-15 | 6.6e-6 per day | 2.4e-3 per year | 5e-8 per year |

"With the firmware rule" means: an exact match wakes alone, anything
approximate needs a second match within 5 s. Read the first column of
false wakes as what would happen if every approximate match woke the PC: it
shows why the confirmation matters.

### Special cases

- **Bluetooth Classic paging.** Every device paired with the same host pages
  the same LAP. A headset that reconnects by itself therefore wakes the PC
  as well, and it cannot be told apart on air. A separate Bluetooth adapter
  on the BC-250, used only for controllers, solves it: put its address in
  the list.
- **A key that changes per session** (random address each switch-on)
  cannot be used. Say so in the device file (`does-not-work`).
- **Disabling.** Turn off the entry's Enabled switch on the Wake page: it is
  still heard and shown, but never wakes the PC. Or remove the entry. The
  other entries are not affected.

## After the capture

1. If the protocol is already supported (Nordic ESB 1 Mbit/s, Bluetooth
   Classic IDs): replay your recording through the exact firmware detector
   with `rfreplay` (`firmware/bc250_controller/host_test/`, see
   `docs/ADDING_PROTOCOLS.md`). It must find your key. Then try Learn on the
   board.
2. If not: the protocol needs a new detector module, see
   `docs/ADDING_PROTOCOLS.md`.
3. Report the device: add a file to `devices/` (see `devices/README.md`) or
   open a ["New device" issue](https://github.com/marcinx64/bc-250-power/issues/new/choose). Do not post real addresses.

## Next target: Steam Controller in 2.4G dongle mode

Not captured yet; status "help wanted" in `devices/`.

What is verified and what is not:

- Not verified by us: any radio detail. A blog post reports that the 2015
  controller's dongle carries a Nordic nRF24LU1+ (read from the chip
  marking); we have not seen it. If it is an nRF24 at 1 Mbit/s, the
  existing ESB path may work; if it is 2 Mbit/s, or uses an own framing, it
  will not (assumed, untested).
- The newer controller / "Puck" is reported (OpenPuck project, emulating the
  receiver on an nRF52840) to use another radio design. Assumed to be
  different from the 2015 one. Unverified.

Capture plan (dongle **unplugged** and not nearby, PC off):

1. Survey: `survey.py record off 20`, then `record on 20` with the controller
   switched on (the Steam button) a few seconds in. `compare off on`. Repeat
   5 times; note fixed and changing channels. Check the waterfall for
   channel width: 1 MHz (1 Mbit/s) or 2 MHz and more.
2. Capture 60 s at 16 MS/s around each fixed channel (tuned 4 MHz below),
   several switch-ons. `analyse` with `--bitrate 1` and `--bitrate 2`.
3. Repeat for each model you have: the original 2015 controller and the
   newer one, each on its own, and each after pairing it with a different
   dongle (does the key change?).
4. Also note: how long it searches, what happens when you press a button
   instead of the Steam button, whether it transmits at all with no dongle
   present.
5. Send `analyse` output with the addresses blanked, not the recordings,
   unless you can share them privately.
