#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Talk to a board running esp-sdr over USB serial.

esp-sdr (https://github.com/ESPARGOS/esp-sdr, GPL-3.0-or-later, by ESPARGOS)
turns an ESP32-S3 (and some other ESP32 chips) into a 2.4 GHz receiver by
using the radio's undocumented capture engine.  Install it with the browser
installer at https://espargos.net/espsdr/.  The serial commands used here
(INFO, FREQ, GAIN, BANDWIDTH, CAP16, SPEC, RELEASE ...) are esp-sdr's own;
see its docs/protocol.md.

  uv run tools/sniffer/espsdr.py info [--port /dev/ttyACM0]

The port comes from --port, the ESPSDR_PORT environment variable, or the
first USB serial port found.
"""
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial", "numpy"]
# ///
import json
import os
import sys
import time
import zlib

import numpy as np

# MS/s -> esp-sdr rate code
RATES = {16: 6, 40: 1, 80: 0}
SNAPSHOT = 16380          # samples in one CAP16 snapshot (1.02 ms at 16 MS/s)


def find_port():
    p = os.environ.get("ESPSDR_PORT")
    if p:
        return p
    from serial.tools import list_ports
    cands = [x.device for x in list_ports.comports()
             if "ACM" in x.device or "usbmodem" in x.device or "USB" in x.device]
    if not cands:
        raise SystemExit("no USB serial port found; give --port")
    return sorted(cands)[0]


def to_standard(raw):
    """esp-sdr sends int8 (I, -Q) pairs.  Return complex64 with standard
    orientation (positive frequency offset = above the centre)."""
    s = np.asarray(raw, np.float32)
    return (s[..., 0] - 1j * s[..., 1]).astype(np.complex64)


class Board:
    def __init__(self, port=None):
        import serial
        self.p = serial.Serial(port or find_port(), 115200, timeout=5)
        self._sync()

    def _sync(self):
        end = time.monotonic() + 10
        self.p.timeout = 0.2
        while time.monotonic() < end:
            m = f"SYNC {time.monotonic_ns()}".encode()
            self.p.write(b"\nRELEASE\n" + m + b"\n")
            t = time.monotonic() + 1
            while time.monotonic() < t:
                if self.p.readline().strip() == m:
                    self.p.timeout = 5
                    return
        raise TimeoutError("board did not answer (is esp-sdr running?)")

    def exact(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.p.read(n - len(b))
            if not c:
                raise TimeoutError(f"short read {len(b)}/{n}")
            b += c
        return bytes(b)

    def cmd(self, text):
        self.p.write((text + "\n").encode())
        line = self.p.readline().decode(errors="replace").strip()
        if line.startswith("ERR"):
            raise RuntimeError(f"{text}: {line}")
        return line

    def setup(self, centre_mhz, gain="hw"):
        """Tune, widest filter, hardware AGC or manual gain index 0..82."""
        self.cmd(f"FREQ {int(centre_mhz)}")
        self.cmd("BANDWIDTH 0")
        self.cmd("GAIN HARDWARE" if str(gain) == "hw" else f"GAIN MANUAL {gain}")

    def snapshot(self, msps=16, n=SNAPSHOT):
        """One raw snapshot -> (complex64 samples, board timestamp) or None on a CRC error."""
        head = self.cmd(f"CAP16 {n} {RATES[msps]}").split()
        if head[0] != "DATA" or int(head[1]) != n:
            raise RuntimeError(head)
        raw = self.exact(n * 2)
        if zlib.crc32(raw) != int(head[2], 16):
            return None
        return to_standard(np.frombuffer(raw, np.int8).reshape(-1, 2)), int(head[3])

    def close(self):
        try:
            self.cmd("RELEASE")
        finally:
            self.p.close()


def main():
    a = sys.argv[1:]
    port = a[a.index("--port") + 1] if "--port" in a else None
    if a[:1] != ["info"]:
        print(__doc__)
        sys.exit(1)
    b = Board(port)
    for c in ("INFO", "VERSION?", "CAPS", "LIMITS?", "SPECINFO?", "DUAL?"):
        print(b.cmd(c))
    b.close()


if __name__ == "__main__":
    main()
