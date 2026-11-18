#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read and write SigMF recordings (the format shared with `rfreplay`).

A recording is two files: NAME.sigmf-meta (JSON) and NAME.sigmf-data (samples).
Datatypes handled: ci8, ci16_le, cf32_le.  Each entry of "captures" is one
snapshot with its own centre frequency (core:frequency, Hz) and start sample.

I/Q convention: standard complex samples.  A signal above the centre
frequency has a POSITIVE frequency offset (it rotates counter-clockwise).
The esp-sdr board sends (I, -Q) pairs; espsdr.to_standard() converts them
before anything is written here, so every file made by these tools is standard.

  uv run tools/sniffer/sigmfio.py info NAME
  uv run tools/sniffer/sigmfio.py resample NAME OUT [--rate 16e6]
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import json
import sys
from pathlib import Path

import numpy as np

DTYPES = {"ci8": np.int8, "ci16_le": "<i2", "cf32_le": "<f4"}


def _paths(name):
    p = str(name)
    for suf in (".sigmf-meta", ".sigmf-data"):
        if p.endswith(suf):
            p = p[: -len(suf)]
    return Path(p + ".sigmf-meta"), Path(p + ".sigmf-data")


def load(name):
    """-> (list of (centre_hz, complex64 samples), sample_rate_hz, meta dict)."""
    mp, dp = _paths(name)
    meta = json.loads(mp.read_text())
    g = meta["global"]
    dt = g["core:datatype"]
    if dt not in DTYPES:
        raise ValueError(f"unsupported datatype {dt}")
    raw = np.fromfile(dp, DTYPES[dt]).reshape(-1, 2).astype(np.float32)
    x = (raw[:, 0] + 1j * raw[:, 1]).astype(np.complex64)
    caps = meta.get("captures") or [{"core:sample_start": 0}]
    starts = [c.get("core:sample_start", 0) for c in caps] + [len(x)]
    out = []
    for i, c in enumerate(caps):
        f = c.get("core:frequency", g.get("core:frequency", 0.0))
        out.append((float(f), x[starts[i]:starts[i + 1]]))
    return out, float(g["core:sample_rate"]), meta


class Writer:
    """Write snapshots one at a time: w.add(centre_hz, complex samples)."""

    def __init__(self, name, fs, datatype="ci8", hw="unknown", description="", extra=None):
        self.mp, self.dp = _paths(name)
        self.fs, self.dt = float(fs), datatype
        self.fh = open(self.dp, "wb")
        self.n = 0
        self.caps = []
        self.g = {
            "core:datatype": datatype, "core:sample_rate": self.fs,
            "core:version": "1.2.0", "core:hw": hw,
            "core:description": description,
            "bc250:iq_convention": "standard I+jQ; positive offset = above centre",
        }
        self.g.update(extra or {})

    def add(self, centre_hz, x, **fields):
        x = np.asarray(x)
        if self.dt == "ci8":
            a = np.empty((len(x), 2), np.int8)
            a[:, 0] = np.clip(np.round(x.real), -128, 127)
            a[:, 1] = np.clip(np.round(x.imag), -128, 127)
        elif self.dt == "ci16_le":
            a = np.empty((len(x), 2), "<i2")
            a[:, 0] = np.clip(np.round(x.real), -32768, 32767)
            a[:, 1] = np.clip(np.round(x.imag), -32768, 32767)
        else:
            a = np.empty((len(x), 2), "<f4")
            a[:, 0], a[:, 1] = x.real, x.imag
        a.tofile(self.fh)
        c = {"core:sample_start": self.n, "core:frequency": float(centre_hz)}
        c.update(fields)
        self.caps.append(c)
        self.n += len(x)

    def close(self):
        self.fh.close()
        self.mp.write_text(json.dumps({"global": self.g, "captures": self.caps,
                                       "annotations": []}, indent=1))
        return self.mp

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def resample(x, fs_in, fs_out, cutoff=7.0e6):
    """Low-pass then linear-interpolate to another rate.  Good enough for
    looking at a recording; the firmware's own unit is 16 MS/s."""
    if fs_in == fs_out:
        return x
    taps = 129
    n = np.arange(taps) - (taps - 1) / 2
    fc = min(cutoff, 0.45 * min(fs_in, fs_out))
    h = np.sinc(2 * fc / fs_in * n) * np.hamming(taps)
    y = np.convolve(x, h / h.sum(), "same")
    t = np.arange(int(len(x) * fs_out / fs_in)) * fs_in / fs_out
    k = np.arange(len(y))
    return (np.interp(t, k, y.real) + 1j * np.interp(t, k, y.imag)).astype(np.complex64)


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "info":
        caps, fs, meta = load(a[1])
        print(f"{len(caps)} snapshots, {fs / 1e6:g} MS/s, {meta['global']['core:datatype']}, "
              f"hw {meta['global'].get('core:hw')}")
        print("centres (MHz):", sorted({round(c / 1e6, 3) for c, _ in caps}))
        print("samples per snapshot: median", int(np.median([len(x) for _, x in caps])))
    elif len(a) >= 3 and a[0] == "resample":
        rate = float(a[a.index("--rate") + 1]) if "--rate" in a else 16e6
        caps, fs, meta = load(a[1])
        with Writer(a[2], rate, meta["global"]["core:datatype"], meta["global"].get("core:hw", "?"),
                    "resampled from %g MS/s" % (fs / 1e6)) as w:
            for c, x in caps:
                w.add(c, resample(x, fs, rate))
        print("written", a[2])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
