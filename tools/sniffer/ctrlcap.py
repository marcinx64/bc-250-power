#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record snapshots with the BC-250 controller itself (no extra SDR needed).

  uv run tools/sniffer/ctrlcap.py [--host bc250.local] [--password PW]
                                  [--channels 1-13] [--count 200] [--interval 0.2]
                                  [--out NAME]

Needs the 2.4G/BT wake method and WiFi connected as a station.  Each snapshot
is 1 ms at 16 MS/s around the centre of one WiFi channel (+-8 MHz).  The
channels are visited round robin.  The result is a SigMF recording (NAME.sigmf-meta
and .sigmf-data, ci16_le) for `rfreplay` and `analyse.py`.  The password comes
from --password, the BC250_PASSWORD environment variable, or a prompt (it is
not asked when the controller has none).
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import argparse
import getpass
import http.client
import json
import os
import sys
import time
from datetime import datetime, timezone

import numpy as np

import sigmfio

NWORDS = 16380
FS = 16e6


def centre_hz(ch):
    return 2407e6 + 5e6 * ch


def raw_to_iq(raw):
    """65520 bytes of little-endian 32-bit words -> (I, Q) as the firmware's sI, sQ:
    I = signed bits 0..9, Q = -(signed bits 10..19); positive offset = above the centre."""
    w = np.frombuffer(raw, "<u4")
    i = ((w & 0x3FF).astype(np.int32) ^ 0x200) - 0x200
    q = -((((w >> 10) & 0x3FF).astype(np.int32) ^ 0x200) - 0x200)
    return i, q


def parse_channels(s):
    out = []
    for part in s.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    if not out or any(c < 1 or c > 13 for c in out):
        raise ValueError("channels must be 1..13")
    return out


class Ctrl:
    def __init__(self, host, port=80):
        self.host, self.port, self.cookie = host, port, None

    def req(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, self.port, timeout=10)
        h = {"Cookie": self.cookie} if self.cookie else {}
        if body is not None:
            body = json.dumps(body)
            h["Content-Type"] = "application/json"
        c.request(method, path, body, h)
        r = c.getresponse()
        data = r.read()
        hdr = dict(r.getheaders())
        c.close()
        return r.status, hdr, data

    def password_set(self):
        st, _, data = self.req("GET", "/api/auth")
        return st != 200 or json.loads(data).get("password_set", True)

    def login(self, password):
        if not self.password_set():
            return
        st, hdr, data = self.req("POST", "/api/login", {"password": password})
        if st != 200:
            raise RuntimeError(f"login failed ({st}): {data.decode(errors='replace')}")
        self.cookie = hdr.get("Set-Cookie", "").split(";")[0]

    def snapshot(self, ch):
        """-> raw bytes, or raises RuntimeError with the controller's message."""
        st, _, data = self.req("GET", f"/api/rf/snapshot?ch={ch}")
        if st != 200:
            try:
                msg = json.loads(data).get("error", "")
            except ValueError:
                msg = data[:80].decode(errors="replace")
            raise RuntimeError(f"channel {ch}: HTTP {st} {msg}")
        if len(data) != NWORDS * 4:
            raise RuntimeError(f"channel {ch}: {len(data)} bytes, expected {NWORDS * 4}")
        return data


def record(ctrl, channels, count, interval, out, log=print):
    done = fails = 0
    with sigmfio.Writer(out, FS, "ci16_le", "BC-250 controller (ESP32-C3)",
                        "snapshots recorded by the controller itself (ctrlcap.py)",
                        {"bc250:source": "controller /api/rf/snapshot"}) as w:
        for k in range(count):
            ch = channels[k % len(channels)]
            try:
                i, q = raw_to_iq(ctrl.snapshot(ch))
            except (RuntimeError, OSError) as e:
                fails += 1
                if fails <= 3:
                    log("  " + str(e))
                if fails >= 10 and done == 0:
                    raise RuntimeError("no snapshot worked; last error: " + str(e))
                time.sleep(max(interval, 0.1))
                continue
            w.add(centre_hz(ch), i.astype(np.float32) + 1j * q.astype(np.float32), **{
                "core:datetime": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z",
                "bc250:channel": ch})
            done += 1
            time.sleep(interval)
    return done, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="bc250.local")
    ap.add_argument("--password")
    ap.add_argument("--channels", default="1-13", help="e.g. 1-13 or 1,6,11")
    ap.add_argument("--count", type=int, default=200, help="snapshots in total")
    ap.add_argument("--interval", type=float, default=0.2, help="seconds between requests")
    ap.add_argument("--out", default=None, help="recording name (default ctrlcap-DATE-TIME)")
    a = ap.parse_args()
    out = a.out or time.strftime("ctrlcap-%Y%m%d-%H%M%S")
    host, _, port = a.host.partition(":")
    ctrl = Ctrl(host, int(port or 80))
    try:
        chans = parse_channels(a.channels)
        pw = a.password or os.environ.get("BC250_PASSWORD")
        if pw is None and ctrl.password_set():
            pw = getpass.getpass("Controller password: ")
        ctrl.login(pw or "")
        t0 = time.time()
        done, fails = record(ctrl, chans, a.count, a.interval, out)
    except (RuntimeError, OSError, ValueError) as e:
        print("error:", e, file=sys.stderr)
        sys.exit(1)
    print(f"{done} snapshots ({fails} failed) on channels {chans} in {time.time() - t0:.0f} s -> {out}.sigmf-meta")
    print(f"next: uv run tools/sniffer/analyse.py {out}")


if __name__ == "__main__":
    main()
