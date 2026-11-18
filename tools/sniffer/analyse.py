#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find out what protocol a recording holds.

  uv run tools/sniffer/analyse.py NAME [--bitrate auto|0.5|1|2] [--min-us 40]
                                  [--thr 8] [--max-snapshots N] [--top 8] [--json OUT]

Steps: find bursts in every snapshot -> carrier offset -> FM demodulation ->
bit rate (1 or 2 Mbit/s ...) -> slice the bits at every sample phase and both
polarities -> test: Nordic ShockBurst / Enhanced ShockBurst (3-5 byte address,
CRC-8 or CRC-16, both preambles), Bluetooth Classic access codes (any LAP,
BCH-checked), Bluetooth LE advertising access address 0x8E89BED6 (a known
other), and an "unknown" list: preamble and the bits after it, repeated.

The summary lists candidate keys (address / LAP) with counts, channels, bit
errors and payload samples.  A key that is the same on every switch-on is
what a wake list entry needs.  A CRC-8 hit is weak (1 in 256 by chance).
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import argparse
import json
import re
import sys
from collections import Counter, defaultdict

import numpy as np

import dsp
import protocols as P
import sigmfio


def alternating_run(s, max_start=60, min_len=8):
    """First run of alternating bits in s[:max_start+...]: (start, length) or None."""
    best = None
    i = 0
    n = min(len(s), max_start + 40)
    while i < n - 1:
        j = i
        while j + 1 < len(s) and s[j + 1] != s[j]:
            j += 1
        if j - i + 1 >= min_len and (best is None or i < max_start):
            return i, j - i + 1
        i = j + 1 if j > i else i + 1
    return best


def analyse(caps, fs, bitrate="auto", min_us=40, thr=8.0, max_snapshots=None, log=None):
    """caps: [(centre_hz, samples)].  Returns a result dict."""
    res = dict(bursts=0, snapshots=0, rates=Counter(), lengths=[], esb={}, bt={}, ble=Counter(),
               unknown=Counter(), unknown_pos={}, no_preamble=0, rate=None)
    items = []
    for ci, (centre, x) in enumerate(caps[:max_snapshots]):
        res["snapshots"] += 1
        for b in dsp.find_bursts(x, fs, thr=thr, min_us=min_us):
            wide = 1.4e6
            y, f1 = dsp.baseband(x, fs, b[0], b[1], b[2], span=wide)
            pad = int(8e-6 * fs)
            lo, hi = min(b[0], pad) + int(2e-6 * fs), len(y) - pad - int(2e-6 * fs)
            r, _ = dsp.estimate_bitrate(y[lo:hi], fs) if hi - lo > 200 else (None, None)
            if r:
                res["rates"][r] += 1
            items.append((ci, centre, b, x))
    res["bursts"] = len(items)
    if bitrate == "auto":
        rate = res["rates"].most_common(1)[0][0] if res["rates"] else 1e6
    else:
        rate = float(bitrate) * 1e6
    res["rate"] = rate
    order = [rate] + ([r for r in (1e6, 2e6) if r != rate] if bitrate == "auto" else [])
    for r in order:
        part = decode_all(items, fs, r)
        if part["esb"] or part["bt"] or part["ble"] or r == order[-1]:
            break
    res.update(part)
    res["rate"] = r
    res["lengths"] = [(b[1] - b[0]) / fs * 1e6 for _, _, b, _ in items]
    return res


def decode_all(items, fs, rate):
    res = dict(esb={}, bt={}, ble=Counter(), unknown=Counter(), unknown_pos={}, no_preamble=0)
    for ci, centre, b, x in items:
        slices, f1 = dsp.burst_bits(x, fs, b, rate)
        mhz = round((centre + f1) / 1e6, 1)
        seen = set()
        top = sorted({(q, ph) for ph, pol, bits, q in slices}, reverse=True)[:4]
        top_ph = {ph for q, ph in top}
        for ph, pol, bits, q in slices:
            if ph in top_ph:
                s = dsp.to_str(bits)
                for h in P.esb_find(s):
                    key = (h["kind"], P.bytes_bits(h["addr"]), h["crc"])
                    if key in seen:
                        continue
                    seen.add(key)
                    d = res["esb"].setdefault(key, dict(addr=h["addr"], kind=h["kind"], crc=h["crc"], n=0,
                                                       mhz=Counter(), pol=Counter(), payloads=Counter(),
                                                       pcf=Counter()))
                    d["n"] += 1
                    d["mhz"][mhz] += 1
                    d["pol"][pol] += 1
                    d["payloads"][" ".join(f"{v:02X}" for v in h["payload"])] += 1
                    d["pcf"][(h["len"], h["pid"], h["noack"])] += 1
            for lap, err in P.bt_find(bits):
                if ("bt", lap) in seen:
                    continue
                seen.add(("bt", lap))
                d = res["bt"].setdefault(lap, dict(n=0, errs=Counter(), mhz=Counter(), pol=Counter()))
                d["n"] += 1
                d["errs"][err] += 1
                d["mhz"][mhz] += 1
                d["pol"][pol] += 1
            if rate in (1e6, 2e6) and ("ble" not in seen):
                dist = P.window_distances(bits, P.ble_pattern())
                if len(dist) and dist.min() <= 2:
                    seen.add("ble")
                    res["ble"][mhz] += 1
        # unknown: best slice, polarity 0, preamble run, bits after it
        q, ph = max((q, ph) for ph, pol, bits, q in slices)
        s = dsp.to_str([bits for p2, pol, bits, q2 in slices if p2 == ph and pol == 0][0])
        run = alternating_run(s)
        if run is None:
            res["no_preamble"] += 1
        else:
            st, ln = run
            cand = s[st + 8: st + 8 + 40]
            res["unknown"][cand] += 1
            res["unknown_pos"].setdefault(cand, (st, ln))
    return res


def report(res, top=8, out=print):
    L = np.array(res["lengths"]) if res["lengths"] else np.zeros(1)
    out(f"{res['snapshots']} snapshots, {res['bursts']} bursts; length us: "
        f"median {np.median(L):.0f}, min {L.min():.0f}, max {L.max():.0f}")
    if res["rates"]:
        out("bit rate estimates (bursts): " + ", ".join(f"{r / 1e6:g} Mbit/s: {n}" for r, n in res["rates"].most_common()))
    out(f"bit rate used for slicing: {res['rate'] / 1e6:g} Mbit/s")
    found = False
    out("")
    out("Nordic ShockBurst / Enhanced ShockBurst (CRC valid):")
    for key, d in sorted(res["esb"].items(), key=lambda kv: -kv[1]["n"])[:top]:
        found = True
        weak = "  [WEAK: CRC-8]" if d["crc"] == 8 else ""
        pre = "AA" if d["addr"][0] & 0x80 else "55"
        out(f"  {d['n']:4d}x  {d['kind']} address {' '.join(f'{b:02X}' for b in d['addr'])} "
            f"({len(d['addr'])} B, CRC-{d['crc']}, preamble {pre}){weak}")
        out(f"        channels MHz: {', '.join(f'{m:g}' for m, _ in d['mhz'].most_common(6))}; "
            f"polarity 0/1: {d['pol'][0]}/{d['pol'][1]}")
        out(f"        (length, pid, no_ack): {d['pcf'].most_common(3)}")
        for pl, c in d["payloads"].most_common(3):
            out(f"        payload {c:3d}x  {pl}")
    if not res["esb"]:
        out("  none")
    out("Bluetooth Classic access codes (BCH-valid sync word, up to 2 wrong bits):")
    for lap, d in sorted(res["bt"].items(), key=lambda kv: -kv[1]["n"])[:top]:
        found = True
        e = d["errs"]
        out(f"  {d['n']:4d}x  LAP {lap:06X}  wrong bits 0/1/2 = {e[0]}/{e[1]}/{e[2]}  "
            f"channels MHz: {', '.join(f'{m:g}' for m, _ in d['mhz'].most_common(6))}")
    if not res["bt"]:
        out("  none")
    out("Bluetooth LE advertising (access address 8E89BED6, known other):")
    out(f"  {sum(res['ble'].values())} bursts" + (f", channels {sorted(res['ble'])[:6]}" if res["ble"] else ""))
    out("")
    out("Unknown: first 40 bits after an 8-bit preamble (alternating run), best sample phase,")
    out("polarity 0.  A repeated line is a sync word / address candidate.")
    for cand, n in res["unknown"].most_common(top):
        if n < 2 and len(res["unknown"]) > top:
            continue
        st, ln = res["unknown_pos"][cand]
        out(f"  {n:4d}x  {cand}  = {dsp.hexbytes(cand)}  (preamble run starts at bit {st}, {ln} long)")
    out(f"  {res['no_preamble']} bursts without an alternating run of 8 bits (not a preamble-framed 1/2 Mbit/s signal?)")
    if not found and not res["unknown"]:
        out("nothing recognised")


def jsonable(res):
    return dict(
        snapshots=res["snapshots"], bursts=res["bursts"], bit_rate=res["rate"],
        esb=[dict(addr=" ".join(f"{b:02X}" for b in d["addr"]), kind=d["kind"], crc=d["crc"], count=d["n"],
                  mhz=dict(d["mhz"])) for d in res["esb"].values()],
        bt=[dict(lap=f"{k:06X}", count=d["n"], errors=dict(d["errs"])) for k, d in res["bt"].items()],
        ble=sum(res["ble"].values()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("--bitrate", default="auto", help="auto, or Mbit/s: 0.5, 1, 2")
    ap.add_argument("--min-us", type=float, default=40)
    ap.add_argument("--thr", type=float, default=8.0, help="burst threshold over the per-bin median")
    ap.add_argument("--max-snapshots", type=int)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--json")
    a = ap.parse_args()
    caps, fs, meta = sigmfio.load(a.name)
    print(f"{a.name}: {len(caps)} snapshots at {fs / 1e6:g} MS/s, hw {meta['global'].get('core:hw')}")
    res = analyse(caps, fs, a.bitrate, a.min_us, a.thr, a.max_snapshots)
    report(res, a.top)
    if a.json:
        json.dump(jsonable(res), open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
