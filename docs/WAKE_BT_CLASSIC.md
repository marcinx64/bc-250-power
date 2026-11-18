# Classic Bluetooth controllers (2.4G wake)

With the wake method set to 2.4G, a **classic Bluetooth** controller
(DualShock 4; DualSense and Switch Pro work the same way, untested) also
wakes the BC-250. It runs alongside the 2.4G dongle devices: the same
snapshots and arming rules, but its own list ("Bluetooth hosts", up to 2;
2.4G devices up to 8) and its own Learn button. The detector is the protocol
module `btpage` (`src/proto_btpage.cpp`, `src/btc.h`); how modules plug in:
`docs/ADDING_PROTOCOLS.md`. Use: `docs/USER_GUIDE.md`; API:
`docs/FINAL_FIRMWARE_SPEC.md` section 23; the 2.4G side: `docs/WAKE_2G4.md`.

## What a controller sends

- A paired controller switched on pages its host: it sends ID packets, a
  4-bit preamble and the 64-bit sync word of the **host's** LAP (the lower
  24 bits of the host's Bluetooth address), 68 us at 1 Mbit/s GFSK, hopping
  over 32 channels at 3200 hops/s. With the BC-250 off nothing answers, so
  it keeps paging until it gives up.
- The sync word is a BCH codeword XOR a fixed PN sequence (Core spec Vol 2
  Part B 6.3.3.1). Any 64-bit window can be checked like a CRC and the LAP
  read back; `src/btc.h` does this (checked against the GIAC sync word
  0x475C58CC73345E72 by `host_test/btc_selftest.cpp`).
- The packets name the host, not the controller. **One entry (the
  BC-250's address) covers every controller paired with the BC-250**, and
  re-pairing a controller with another host stops it waking the BC-250.
  Other devices paired with the BC-250 that reconnect by themselves (for
  example a headset) page the same address and would wake it too; they
  cannot be told apart on air. A second Bluetooth adapter on the BC-250
  only for controllers would separate them (its address in the wake list).

## Detection

- Bursts from 40 us (an ID packet; ESB frames need 75 us) are demodulated
  with the ESB demodulator (the modulation is nearly the same) and every
  64-bit window, all 16 sample phases, is checked: a valid sync word with
  at most one wrong bit (corrected), or a listed host's sync word with at
  most 3 wrong bits of 64 (random data: ~2e-15 per window). Inverted
  polarity never showed up (DS4, 71 catches), so it is not searched. A
  single wrong bit in the first or last bit of the sync word is not
  corrected (they share a syndrome); a listed host still matches it by the
  3-bit budget.
- Wake rule, as for 2.4G devices: a listed host's sync word with no wrong
  bit counts like a CRC-valid frame and wakes at once; with 1 to 3 wrong
  bits it counts like an address match and needs a second within 5 s.
  Arming: the Re-arm time, as for every entry. The false-match arithmetic
  (about 5e-10 false wakes a year in the worst case) is in
  `docs/ADDING_PROTOCOLS.md` section 5.
- Learn for Bluetooth hosts records every LAP seen in a valid sync word
  (2.4G Learn ignores them, and the other way round; one Learn runs at a
  time); a host is "ready" from 3 catches. Other classic Bluetooth traffic
  nearby shows up as well (a phone and its headset: their own LAP, long
  packets), so add the one that appears when the controller's button is
  pressed, or type in the BC-250's address (`bluetoothctl show` on the
  BC-250).
- Memory: 2 KiB of syndrome tables on the heap, allocated by the module at
  start; without them Bluetooth hosts stay off and the 2.4G devices keep
  working.

## Measurements (one room, DS4 paired with a laptop, LAP 9A8B7C)

- S3 with esp-sdr, 80 MS/s snapshots of the whole band: 57 catches over 15 s
  of paging, 35 without a wrong bit; channels 2403-2470 MHz. One other LAP
  (4D5E6F) was a real link nearby, not noise.
- Spare C3 (log only, normal WiFi channel sweep), three presses: 21, 26 and
  24 catches over ~14 s each (1.5-1.9 a second, longest gap 3.3 s); catches
  on WiFi channels 1-12; WiFi stayed connected. For comparison, the dongle
  controllers are caught 1-17 s apart.
- The DS4 pages for about 14 s after the PS button and then turns off.
- On the PC, synthetic ID packets at carrier offsets of 1 to 7 MHz in noise
  are found about nine times in ten (`host_test/test_detect.cpp`); random
  data produced no sync word in 1e8 windows (theory 0.006).

## Status

- [x] Detection on the C3 with WiFi connected
- [x] Wake list entries (Learn or typed in), wake rules, web UI
- [ ] Owner's wake test on the spare and on the board in the case
- [ ] DualSense, Switch Pro
