#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write synthetic SigMF recordings: ESB frames, Bluetooth Classic ID packets,
BLE access addresses, or noise only.  For self-tests and for trying `rfreplay`.

  uv run tools/sniffer/synth.py esb  OUT --addr A1B2C3D4E5 [--rate 1|2] [--payload HEX]
  uv run tools/sniffer/synth.py bt   OUT --lap 9A8B7C
  uv run tools/sniffer/synth.py ble  OUT
  uv run tools/sniffer/synth.py noise OUT

Common options: --snapshots 200 (each 16380 samples at 16 MS/s), --centre-mhz 2464,
--offset-mhz 4.2 (signal above the centre), --snr 20, --every 1 (a burst in
every Nth snapshot), --fs 16 (MS/s), --seed 1.
The Gaussian filter (BT 0.5) and modulation index 0.32 match nRF24 / BR.
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import argparse
import sys

import numpy as np

import protocols as P
import sigmfio


def gfsk(bits, fs, bitrate, f0, h=0.32, bt=0.5):
    sps = int(round(fs / bitrate))
    nrz = np.repeat(2.0 * np.asarray(bits) - 1, sps)
    t = np.arange(-2 * sps, 2 * sps + 1) / sps
    g = np.exp(-(2 * np.pi ** 2 * bt ** 2 / np.log(2)) * t ** 2)
    freq = np.convolve(nrz, g / g.sum(), "same") * h / 2 * bitrate
    ph = 2 * np.pi * np.cumsum(freq + f0) / fs
    return np.exp(1j * ph)


def snapshot(n, fs, bursts, snr_db, rng):
    """bursts: list of (start sample, complex burst).  Unit-amplitude signal
    over complex noise at the given SNR; scaled for 8-bit samples."""
    x = np.zeros(n, complex)
    for s0, b in bursts:
        x[s0:s0 + len(b)] += b[: n - s0]
    sigma = 10 ** (-snr_db / 20)
    x += sigma * (rng.standard_normal(n) + 1j * rng.standard_normal(n)) / np.sqrt(2)
    return x * 40.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=["esb", "bt", "ble", "noise"])
    ap.add_argument("out")
    ap.add_argument("--addr", default="A1B2C3D4E5", help="ESB address, 3-5 bytes hex")
    ap.add_argument("--lap", default="9A8B7C", help="Bluetooth LAP, 6 hex digits")
    ap.add_argument("--rate", type=int, default=1, choices=[1, 2], help="ESB Mbit/s")
    ap.add_argument("--payload", default="B08872000000 00FF7FFF7FFF7FFF7F0000A207".replace(" ", ""))
    ap.add_argument("--crc", type=int, default=16, choices=[8, 16])
    ap.add_argument("--snapshots", type=int, default=200)
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--centre-mhz", default="2464", help="comma list: cycled per snapshot")
    ap.add_argument("--offset-mhz", type=float, default=4.2)
    ap.add_argument("--snr", type=float, default=20)
    ap.add_argument("--fs", type=float, default=16)
    ap.add_argument("--datatype", default="ci8", choices=["ci8", "ci16_le", "cf32_le"])
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    synth(a)


def synth(a):
    rng = np.random.default_rng(a.seed)
    fs = a.fs * 1e6
    n = int(round(16380 * fs / 16e6))
    centres = [float(c) for c in a.centre_mhz.split(",")]
    bitrate = 1e6
    if a.kind == "esb":
        addr = list(bytes.fromhex(a.addr))
        bits = P.esb_build(addr, bytes.fromhex(a.payload), crc=a.crc)
        bits = np.array([int(c) for c in bits], np.uint8)
        bitrate = a.rate * 1e6
    elif a.kind == "bt":
        bits = P.bt_idpacket(int(a.lap, 16))
    elif a.kind == "ble":
        bits = np.r_[P.ble_pattern(), np.zeros(0, np.uint8), rng.integers(0, 2, 120, dtype=np.uint8)]
    else:
        bits = None
    with sigmfio.Writer(a.out, fs, a.datatype, "synthetic (synth.py)",
                        f"synthetic {a.kind}", {"bc250:synth": vars(a)}) as w:
        for i in range(a.snapshots):
            c = centres[i % len(centres)]
            bl = []
            if bits is not None and i % a.every == 0:
                s0 = int(rng.integers(int(60e-6 * fs), int(600e-6 * fs)))
                f0 = a.offset_mhz * 1e6
                bl.append((s0, gfsk(bits, fs, bitrate, f0)))
            w.add(c * 1e6, snapshot(n, fs, bl, a.snr, rng), **{"bc250:t_s": i * 0.05})
    print("written", a.out + ".sigmf-meta")


if __name__ == "__main__":
    main()
