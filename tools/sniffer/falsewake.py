#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How likely is a false wake?  A calculator and a test on a background recording.

  uv run tools/sniffer/falsewake.py calc --bits 48 --errors 2
  uv run tools/sniffer/falsewake.py calc --bits 64 --errors 3 --bursts-per-s 50
  uv run tools/sniffer/falsewake.py scan BACKGROUND --key esb:A1B2C3D4E5 --errors 2
  uv run tools/sniffer/falsewake.py scan BACKGROUND --key bt:9A8B7C --errors 3

calc: a window of n bits matches a random key with at most e wrong bits with
probability p = sum_{k<=e} C(n,k) / 2^n.  Each detected burst is searched at
`phases` sample phases and `positions` start positions (an upper bound: windows
at neighbouring phases are not independent).  False matches per day =
p * windows * bursts/s * 86400.  Wake rule: an exact match wakes alone, an
approximate one needs a second match within T seconds.  Both are given, with
and without that confirmation; "armed" is the share of time the entry can fire.

scan: runs the same matcher over a recording made with the device OFF
(the more busy 2.4 GHz traffic in it, the better) and reports the smallest
Hamming distance seen between the key and any window, how many windows came
within e, e+1 and e+2 bits, and what chance alone would give.  The margin is
(best distance) - e: the bigger, the safer.
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import argparse
import math
import sys

import numpy as np

import dsp
import protocols as P
import sigmfio

YEAR = 365.25 * 86400


def p_window(n, e):
    return sum(math.comb(n, k) for k in range(e + 1)) / 2.0 ** n


def calc(n, e, phases=16, positions=200, bursts_per_s=20.0, confirm_s=5.0, armed=1.0, out=print):
    p = p_window(n, e)
    p0 = 2.0 ** -n
    W = phases * positions
    lam = p * W * bursts_per_s * armed          # matches per second (any <= e wrong bits)
    lam0 = p0 * W * bursts_per_s * armed        # exact matches per second
    wake_no_conf = lam                                  # every match wakes
    wake_conf = lam0 + (lam - lam0) * (1 - math.exp(-lam * confirm_s))
    out(f"key {n} bits, up to {e} wrong bits")
    out(f"  per-window probability  sum C({n},k<={e}) / 2^{n} = {p:.3e}")
    out(f"  windows per burst       {phases} phases x {positions} positions = {W}")
    out(f"  bursts per second       {bursts_per_s:g}   (armed {armed:.0%} of the time)")
    out(f"  false matches          {lam:.3e} /s   {lam * 86400:.3e} /day   {lam * YEAR:.3e} /year")
    out(f"  false WAKES, any match wakes (no confirmation)         {wake_no_conf * YEAR:.3e} /year"
        f"   = one per {1 / (wake_no_conf * YEAR):.3g} years" if wake_no_conf > 0 else "  no false wakes")
    out(f"  false WAKES, exact alone / approximate needs 2nd in {confirm_s:g} s  "
        f"{wake_conf * YEAR:.3e} /year   = one per {1 / (wake_conf * YEAR):.3g} years")
    return dict(p=p, matches_per_day=lam * 86400, wakes_year_no_conf=wake_no_conf * YEAR,
                wakes_year_conf=wake_conf * YEAR)


def key_pattern(spec):
    kind, _, val = spec.partition(":")
    kind = kind.lower()
    if kind == "esb":
        return P.esb_pattern(list(bytes.fromhex(val))), 75
    if kind == "bt":
        return P.bt_syncword(int(val, 16)), 40
    if kind == "ble":
        return P.ble_pattern(), 40
    raise SystemExit("key must be esb:ADDRHEX, bt:LAPHEX or ble:")


def scan(name, key, e, bitrate=1e6, thr=8.0, both_pol=False, out=print):
    pat, min_us = key_pattern(key)
    n = len(pat)
    caps, fs, meta = sigmfio.load(name)
    hist = np.zeros(n + 1, np.int64)
    nb = 0
    secs = 0.0
    stamps = [c.get("bc250:t_s") for c in meta.get("captures", []) if "bc250:t_s" in c]
    for centre, x in caps:
        secs += len(x) / fs
        for b in dsp.find_bursts(x, fs, thr=thr, min_us=min_us):
            nb += 1
            slices, _ = dsp.burst_bits(x, fs, b, bitrate)
            for ph, pol, bits, q in slices:
                if pol == 1 and not both_pol:
                    continue
                d = P.window_distances(bits, pat)
                if len(d):
                    hist += np.bincount(d, minlength=n + 1)
    N = int(hist.sum())
    wall = (max(stamps) - min(stamps)) if len(stamps) > 1 else None
    out(f"{name}: {len(caps)} snapshots, {secs:.2f} s of air time"
        + (f" over {wall / 60:.1f} min" if wall else "") + f", {nb} bursts, {N} windows tested (key {key}, {n} bits)")
    if wall:
        out(f"  bursts per second of wall time: {nb / wall:.2f} (use as --bursts-per-s in `calc`)")
    if N == 0:
        out("no windows: no bursts long enough in this recording; nothing to say")
        return None
    best = int(np.flatnonzero(hist)[0])
    out(f"  best (lowest) distance seen: {best} wrong bits   margin to the limit ({e}): {best - e}")
    for lim in (e, e + 1, e + 2):
        exp = N * p_window(n, lim)
        out(f"  windows within {lim} bits: {int(hist[:lim + 1].sum()):7d}   (chance alone: {exp:.2g})")
    out("  distance histogram (first 16): " + " ".join(f"{k}:{int(hist[k])}" for k in range(min(16, n + 1)) if hist[k]))
    if best <= e:
        out("  WARNING: windows within the limit exist in the background: this key would false-match here.")
    elif best == e + 1:
        out("  WARNING: margin of 1 bit only: lower the limit by one or pick another key.")
    if (wall or 0) < 1800:
        out("  note: recording shorter than 30 min (or no time stamps): use 30-60 min of busy 2.4 GHz for a real margin")
    return dict(best=best, windows=N, hist=hist)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("calc")
    c.add_argument("--bits", type=int, required=True)
    c.add_argument("--errors", type=int, required=True)
    c.add_argument("--phases", type=int, default=16)
    c.add_argument("--positions", type=int, default=200, help="start positions searched per burst")
    c.add_argument("--bursts-per-s", type=float, default=20.0, help="detected bursts per second of other traffic")
    c.add_argument("--confirm-s", type=float, default=5.0)
    c.add_argument("--armed", type=float, default=1.0, help="share of time the entry is armed")
    s = sub.add_parser("scan")
    s.add_argument("name")
    s.add_argument("--key", required=True)
    s.add_argument("--errors", type=int, required=True)
    s.add_argument("--bitrate", type=float, default=1.0, help="Mbit/s")
    s.add_argument("--thr", type=float, default=8.0)
    s.add_argument("--both-polarities", action="store_true")
    a = ap.parse_args()
    if a.cmd == "calc":
        calc(a.bits, a.errors, a.phases, a.positions, a.bursts_per_s, a.confirm_s, a.armed)
    else:
        scan(a.name, a.key, a.errors, a.bitrate * 1e6, a.thr, a.both_polarities)


if __name__ == "__main__":
    main()
