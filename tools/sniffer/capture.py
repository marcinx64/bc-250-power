#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record raw I/Q snapshots from an esp-sdr board into a SigMF file.

  uv run tools/sniffer/capture.py NAME SECONDS [--centre 2464[,2437,...]]
                                  [--rate 16|40|80] [--gain hw|0..82] [--port P]

Each snapshot is 16380 samples (1.02 ms at 16 MS/s) and becomes one entry of
"captures" in NAME.sigmf-meta with its centre frequency.  Several centres
(comma list) are visited in turn, one snapshot each.  Datatype ci8, standard
I/Q (the board's (I, -Q) pairs are converted, see sigmfio.py).

Tune about 4 MHz below the signal so it stays away from the receiver's DC
spike; at 16 MS/s the window covers +-8 MHz around the centre.
Use 16 MS/s for anything meant for `rfreplay`.
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial", "numpy"]
# ///
import argparse
import time

import numpy as np

import espsdr
import sigmfio


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("seconds", type=float)
    ap.add_argument("--centre", default="2464", help="MHz, or a comma list visited in turn")
    ap.add_argument("--rate", type=int, default=16, choices=[16, 40, 80])
    ap.add_argument("--gain", default="hw")
    ap.add_argument("--port")
    a = ap.parse_args()
    centres = [int(c) for c in a.centre.split(",")]
    b = espsdr.Board(a.port)
    print(b.cmd("INFO"))
    w = sigmfio.Writer(a.name, a.rate * 1e6, "ci8", "esp-sdr board (CAP16 snapshots)",
                       "raw I/Q snapshots recorded with tools/sniffer/capture.py",
                       {"bc250:gain": "hardware AGC" if a.gain == "hw" else f"manual {a.gain}",
                        "bc250:snapshot_samples": espsdr.SNAPSHOT})
    t0 = time.monotonic()
    k = bad = 0
    cur = None
    while time.monotonic() - t0 < a.seconds:
        c = centres[k % len(centres)]
        if c != cur:
            b.setup(c, a.gain)
            cur = c
        r = b.snapshot(a.rate)
        k += 1
        if r is None:
            bad += 1
            continue
        x, stamp = r
        w.add(c * 1e6, x * 1.0, **{"bc250:t_s": round(time.monotonic() - t0, 4), "bc250:board_stamp": stamp})
    w.close()
    b.close()
    dur = time.monotonic() - t0
    print(f"{len(w.caps)} snapshots in {dur:.0f} s ({len(w.caps) / dur:.1f}/s), {bad} CRC errors -> {w.mp}")


if __name__ == "__main__":
    main()
