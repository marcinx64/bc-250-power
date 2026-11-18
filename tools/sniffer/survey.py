#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Spectrum survey over 2400-2480 MHz with an esp-sdr board: where does a
device transmit?

  uv run tools/sniffer/survey.py record NAME [SECONDS] [--port P] [--dir captures]
  uv run tools/sniffer/survey.py compare BEFORE AFTER [--dir captures]
  uv run tools/sniffer/survey.py bursts NAME MHZ [MHZ ...] [--dir captures]
  uv run tools/sniffer/survey.py plot NAME [--dir captures]

record  max-hold spectra at 80 MS/s, 1024 bins (78 kHz), into DIR/NAME.npz and
        DIR/NAME.png (waterfall).  The max detector keeps short packets that a
        mean would average away.
compare what got stronger in AFTER than in BEFORE (record the device off, then
        on, with its dongle unplugged).
bursts  timing of the bursts seen on the given frequencies.

Ported from the owner's sniff.py.  The record command uses esp-sdr's SPEC
streaming (https://github.com/ESPARGOS/esp-sdr).
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial", "numpy", "matplotlib"]
# ///
import json
import sys
import zlib
from pathlib import Path

import numpy as np

CENTER_MHZ = 2440
RATE_CODE, FS = 0, 80_000_000
BINS = 1024


def freqs():
    # Natural FFT order; RF above the LO shows at negative frequency
    # (esp-sdr docs/iq-stream.md), so flip the sign.
    k = np.arange(BINS)
    k = np.where(k < BINS // 2, k, k - BINS)
    return CENTER_MHZ - k * (FS / BINS) / 1e6


def profile(b):
    info = json.loads(b.cmd("SPECINFO?")[9:])
    for fs, rate, n, stride, units, *_ in info["profiles"]:
        if rate == RATE_CODE and n == BINS:
            return stride, units
    raise RuntimeError(f"no {BINS}-bin 80 MS/s profile: {info}")


def record(name, seconds, port, d):
    from espsdr import Board
    d.mkdir(exist_ok=True)
    b = Board(port)
    print(b.cmd("INFO"))
    b.setup(CENTER_MHZ)
    stride, units = profile(b)
    head = b.cmd(f"SPEC {int(seconds * 1000)} {stride} {units} 1 {RATE_CODE} {BINS}").split()
    print(" ".join(head))
    rows, idx, nfft = [], [], []
    while True:
        magic = b.exact(4)
        if magic == b"SPEC":
            end = (magic + b.p.readline()).decode().split()
            break
        if magic == b"SPS1":
            b.exact(36)
            continue
        if magic != b"SPC1":
            raise RuntimeError(f"lost framing: {magic!r}")
        f = magic + b.exact(BINS + 28)
        if zlib.crc32(f[:-4]) != int.from_bytes(f[-4:], "little"):
            raise RuntimeError("frame CRC mismatch")
        idx.append(int.from_bytes(f[8:16], "little"))
        nfft.append(int.from_bytes(f[20:22], "little"))
        rows.append(np.frombuffer(f[28:28 + BINS], np.uint8))
    b.close()
    rows = np.array(rows)
    t = (np.array(idx) - idx[0]) / FS
    np.savez_compressed(d / f"{name}.npz", power=rows, t=t, ffts=np.array(nfft), f=freqs())
    print(f"{len(rows)} frames over {t[-1]:.1f} s, end report: {' '.join(end)}")
    plot(name, d)


def load(name, d):
    z = np.load(d / f"{name}.npz")
    order = np.argsort(z["f"])
    return z["f"][order], z["power"][:, order].astype(float) / 2, z["t"]   # codes are 2 per dB


def plot(name, d):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    f, pw, t = load(name, d)
    fig, (a, b) = plt.subplots(2, 1, figsize=(12, 8), height_ratios=[1, 3], sharex=True)
    a.plot(f, pw.max(0), lw=0.7, label="max")
    a.plot(f, np.median(pw, 0), lw=0.7, label="median")
    a.set_ylabel("dB (uncalibrated)")
    a.legend(loc="upper right")
    a.set_title(name)
    b.imshow(pw, aspect="auto", origin="lower", extent=[f[0], f[-1], t[0], t[-1]], cmap="viridis")
    b.set_xlabel("MHz")
    b.set_ylabel("s")
    fig.tight_layout()
    out = d / f"{name}.png"
    fig.savefig(out, dpi=110)
    print(f"waterfall: {out}")


def compare(a, b, d, out=print):
    f, pa, _ = load(a, d)
    _, pb, _ = load(b, d)

    def busy(pw):
        return (pw > np.median(pw, 0) + 10).mean(0)
    da, db = np.percentile(pa, 99, 0), np.percentile(pb, 99, 0)
    diff = db - da
    ba, bb = busy(pa), busy(pb)
    out(f"{'MHz':>9} {'99% before':>10} {'99% after':>10} {'diff dB':>8} {'busy before':>11} {'busy after':>10}")
    for i in np.argsort(-diff)[:25]:
        out(f"{f[i]:9.2f} {da[i]:10.1f} {db[i]:10.1f} {diff[i]:8.1f} {ba[i]:11.1%} {bb[i]:10.1%}")


def bursts(name, mhz, d, out=print):
    f, pw, t = load(name, d)
    for m in mhz:
        i = int(np.argmin(abs(f - m)))
        col = pw[:, max(0, i - 1):i + 2].max(1)
        hot = col > np.median(col) + 10
        on = t[np.flatnonzero(hot[1:] & ~hot[:-1]) + 1]
        out(f"{f[i]:.2f} MHz: {hot.mean():.1%} of frames hot, {len(on)} bursts")
        if len(on) > 2:
            gaps = np.diff(on) * 1000
            out(f"  gap between bursts ms: median {np.median(gaps):.2f}, "
                f"10% {np.percentile(gaps, 10):.2f}, 90% {np.percentile(gaps, 90):.2f}")
    out(f"frame spacing ms: median {np.median(np.diff(t)) * 1000:.2f} (timing resolution)")


def main():
    a = sys.argv[1:]

    def opt(flag, default=None):
        if flag in a:
            i = a.index(flag)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default
    port, d = opt("--port"), Path(opt("--dir", "captures"))
    if not a or a[0] not in ("record", "compare", "bursts", "plot"):
        print(__doc__)
        sys.exit(1)
    if a[0] == "record":
        record(a[1], float(a[2]) if len(a) > 2 else 10, port, d)
    elif a[0] == "compare":
        compare(a[1], a[2], d)
    elif a[0] == "plot":
        plot(a[1], d)
    else:
        bursts(a[1], [float(x) for x in a[2:]], d)


if __name__ == "__main__":
    main()
