#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-test without hardware: synth -> analyse must find what was put in.

  uv run tools/sniffer/selftest.py
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
import argparse
import http.server
import json
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

import numpy as np

import analyse
import ctrlcap
import falsewake
import protocols as P
import sigmfio
import synth

FAILS = []


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)
    if not ok:
        FAILS.append(text)


def make(tmp, kind, name, **kw):
    a = argparse.Namespace(kind=kind, out=str(tmp / name), addr="A1B2C3D4E5", lap="9A8B7C", rate=1,
                           payload="B0887200000000FF7FFF7FFF7FFF7F0000A207", crc=16, snapshots=30, every=1,
                           centre_mhz="2464,2437", offset_mhz=4.2, snr=20, fs=16.0, datatype="ci8", seed=1)
    for k, v in kw.items():
        setattr(a, k, v)
    synth.synth(a)
    return str(tmp / name)


def run(path):
    caps, fs, _ = sigmfio.load(path)
    return analyse.analyse(caps, fs)


def fake_controller():
    """A local HTTP server that answers like the controller: raw 10-bit words of one ESB burst."""
    rng = np.random.default_rng(3)
    bits = np.array([int(c) for c in P.esb_build(list(bytes.fromhex("A1B2C3D4E5")),
                                                  bytes.fromhex("B0887200000000FF7FFF7FFF7FFF7F0000A207"), crc=16)], np.uint8)

    def words():
        s0 = int(rng.integers(1000, 9000))
        x = synth.snapshot(16380, 16e6, [(s0, synth.gfsk(bits, 16e6, 1e6, 4.2e6))], 20, rng)
        i = np.clip(np.round(x.real), -512, 511).astype(np.int32) & 0x3FF
        qraw = (-np.clip(np.round(x.imag), -511, 511)).astype(np.int32) & 0x3FF   # the engine stores -Q
        return (i | (qraw << 10)).astype("<u4").tobytes()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, body, ctype="application/json", extra=()):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/api/auth":
                self.reply(200, b'{"password_set":true,"authed":false}')
            elif self.headers.get("Cookie") != "sid=abc":
                self.reply(401, b'{"ok":false,"error":"login required"}')
            elif self.path.startswith("/api/rf/snapshot?ch="):
                ch = int(self.path.split("=")[1])
                if ch == 13:
                    self.reply(409, b'{"ok":false,"error":"radio busy, try again"}')
                else:
                    self.reply(200, words(), "application/octet-stream", [("X-Channel", str(ch))])

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            ok = json.loads(self.rfile.read(n)).get("password") == "secret"
            self.reply(200 if ok else 401, b'{"ok":true}' if ok else b'{"ok":false}',
                       extra=[("Set-Cookie", "sid=abc; Path=/; HttpOnly")] if ok else ())

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    tmp = Path(tempfile.mkdtemp(prefix="sniffer-selftest-"))
    check(int("".join(map(str, P.bt_syncword(0x9E8B33))), 2) == 0x475C58CC73345E72,
          "GIAC (9E8B33) sync word is 475C58CC73345E72")
    check(abs(falsewake.p_window(48, 2) - 1177 / 2 ** 48) < 1e-20, "falsewake: 48 bits, <=2 errors = 1177/2^48")

    # ESB 1 Mbit/s, CRC-16, two centres, both preambles (A1 -> AA, 5A -> 55)
    for addr in ("A1B2C3D4E5", "5A6B7C8D9E"):
        res = run(make(tmp, "esb", "esb1_" + addr, addr=addr))
        keys = [d for d in res["esb"].values() if bytes(d["addr"]).hex().upper() == addr and d["crc"] == 16]
        check(bool(keys) and keys[0]["n"] >= 20 and abs(res["rate"] - 1e6) < 1,
              f"ESB 1 Mbit/s address {addr}: found {keys[0]['n'] if keys else 0}x of 30, rate {res['rate'] / 1e6:g}")
        if keys:
            check(set(keys[0]["mhz"]) <= {2468.2, 2441.2, 2468.1, 2468.3, 2441.1, 2441.3} and keys[0]["pol"][0] > 0,
                  f"  channels {sorted(keys[0]['mhz'])}, polarity 0 {keys[0]['pol'][0]}x (offset sign right)")
    # ESB 2 Mbit/s
    res = run(make(tmp, "esb", "esb2", addr="A1B2C3D4E5", rate=2))
    keys = [d for d in res["esb"].values() if bytes(d["addr"]).hex().upper() == "A1B2C3D4E5" and d["crc"] == 16]
    check(bool(keys) and keys[0]["n"] >= 15 and abs(res["rate"] - 2e6) < 1,
          f"ESB 2 Mbit/s: found {keys[0]['n'] if keys else 0}x of 30, rate {res['rate'] / 1e6:g}")
    # other sample rates, lower SNR, a payload heavy in zeros
    for label, kw in (("80 MS/s", dict(fs=80.0)), ("12 dB SNR, zero-heavy payload", dict(
            snr=12, payload="0000000000000000000000000000000001"))):
        res = run(make(tmp, "esb", "esb_x", addr="A1B2C3D4E5", **kw))
        keys = [d for d in res["esb"].values() if bytes(d["addr"]).hex().upper() == "A1B2C3D4E5" and d["crc"] == 16]
        check(bool(keys) and keys[0]["n"] >= 20, f"ESB, {label}: found {keys[0]['n'] if keys else 0}x of 30")
    res = run(make(tmp, "bt", "bt40", lap="9A8B7C", fs=40.0))
    check(res["bt"].get(0x9A8B7C, {}).get("n", 0) >= 20, "Bluetooth Classic at 40 MS/s found")
    # Bluetooth Classic ID packets
    for lap in ("9A8B7C", "9E8B33"):
        res = run(make(tmp, "bt", "bt_" + lap, lap=lap))
        d = res["bt"].get(int(lap, 16))
        check(d is not None and d["n"] >= 20, f"Bluetooth Classic LAP {lap}: found {d['n'] if d else 0}x of 30")
    # BLE access address
    res = run(make(tmp, "ble", "ble"))
    check(sum(res["ble"].values()) >= 20, f"BLE advertising access address: {sum(res['ble'].values())}x of 30")
    # Noise only: nothing recognised
    path = make(tmp, "noise", "noise", snapshots=60)
    res = run(path)
    check(not res["esb"] and not res["bt"] and not res["ble"],
          f"noise only: no protocol hits ({res['bursts']} bursts)")
    # false-wake scan on the noise: the key must stay far away
    r = falsewake.scan(path, "esb:A1B2C3D4E5", 2, out=lambda *_: None)
    check(r is None or r["best"] > 2, "falsewake scan on noise: no window within 2 bits of the ESB key")
    r = falsewake.scan(make(tmp, "esb", "esb_key", addr="A1B2C3D4E5"), "esb:A1B2C3D4E5", 2, out=lambda *_: None)
    check(r is not None and r["best"] == 0, "falsewake scan on a recording with the key: best distance 0")
    # SigMF round trip
    caps, fs, meta = sigmfio.load(path)
    check(fs == 16e6 and meta["global"]["core:datatype"] == "ci8" and len(caps) == 60
          and caps[1][0] == 2437e6 and len(caps[0][1]) == 16380, "SigMF read back: rate, datatype, 60 snapshots, centres")
    # ctrlcap against a fake controller; channel 13 answers 409 (counted as a failure)
    srv = fake_controller()
    c = ctrlcap.Ctrl("127.0.0.1", srv.server_address[1])
    try:
        c.login("wrong")
        check(False, "ctrlcap: wrong password refused")
    except RuntimeError:
        check(True, "ctrlcap: wrong password refused")
    c.login("secret")
    out = str(tmp / "ctrl")
    done, fails = ctrlcap.record(c, [1, 6, 13], 12, 0, out, log=lambda *_: None)
    srv.shutdown()
    caps, fs, meta = sigmfio.load(out)
    check(done == 8 and fails == 4 and len(caps) == 8 and fs == 16e6 and caps[0][0] == 2412e6 and caps[1][0] == 2437e6
          and meta["global"]["core:datatype"] == "ci16_le" and "ESP32-C3" in meta["global"]["core:hw"],
          f"ctrlcap: 8 snapshots saved, 4 refused, centres and hardware in the metadata ({done}/{fails})")
    res = analyse.analyse(caps, fs)
    keys = [d for d in res["esb"].values() if bytes(d["addr"]).hex().upper() == "A1B2C3D4E5" and d["crc"] == 16]
    check(bool(keys) and keys[0]["n"] >= 6 and keys[0]["pol"][0] > 0,
          f"ctrlcap recording: analyse.py finds the address ({keys[0]['n'] if keys else 0}x of 8)")
    rr = Path(__file__).resolve().parents[2] / "firmware/bc250_controller/host_test/build/rfreplay"
    if rr.exists():
        o = subprocess.run([str(rr), out], capture_output=True, text=True).stdout
        check("esb1m key A1B2C3D4E5 exact" in o, "ctrlcap recording: rfreplay finds the address")
    else:
        print("SKIP  rfreplay not built (make -C firmware/bc250_controller/host_test)")
    print("\nselftest:", "ALL PASSED" if not FAILS else f"{len(FAILS)} FAILED")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
