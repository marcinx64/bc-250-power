# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame tests for the protocols the controller knows, plus a known-other.

- Nordic ShockBurst / Enhanced ShockBurst: preamble 0x55/0xAA, 3-5 byte
  address, optional 9-bit PCF (6-bit length, 2-bit PID, 1-bit no-ack),
  payload, CRC-8 (poly 0x07, init 0xFF) or CRC-16 (CCITT, init 0xFFFF).
- Bluetooth Classic access codes (ID packets): 64-bit sync word of a LAP,
  a BCH(64,30) codeword XOR a PN sequence (Core spec Vol 2 Part B 6.3.3.1).
  Ported from btc.py.
- Bluetooth LE advertising access address 0x8E89BED6.

Bits are sent MSB first per byte for ESB, in the order of the air stream.
"""
import re
from itertools import combinations

import numpy as np

# ------------------------------------------------------------------ ESB


def crc_bits(bits, width):
    """bits: str of 0/1.  CRC-16 CCITT init FFFF or CRC-8 poly 07 init FF."""
    if width == 16:
        crc, poly, top = 0xFFFF, 0x1021, 0x8000
    else:
        crc, poly, top = 0xFF, 0x07, 0x80
    for b in bits:
        fb = ((crc & top) != 0) ^ (b == "1")
        crc = ((crc << 1) & (top * 2 - 1)) ^ (poly if fb else 0)
    return crc


def bytes_bits(bs):
    return "".join(format(b, "08b") for b in bs)


def esb_preamble(addr):
    return "10101010" if addr[0] & 0x80 else "01010101"


def esb_pattern(addr):
    """Preamble + address as a 0/1 numpy array (the 48-bit key for 5 bytes)."""
    s = esb_preamble(addr) + bytes_bits(addr)
    return np.array([int(c) for c in s], np.uint8)


def esb_build(addr, payload, pid=0, noack=1, crc=16, enhanced=True):
    """Bits (str) of a complete frame as sent on air."""
    s = esb_preamble(addr) + bytes_bits(addr)
    if enhanced:
        s += format(len(payload), "06b") + format(pid, "02b") + str(noack)
    s += bytes_bits(payload)
    body = s[8:]
    return s + format(crc_bits(body, crc), f"0{crc}b")


def esb_find(bits, max_start=48, widths=(3, 4, 5)):
    """Find CRC-valid ESB / plain ShockBurst frames in a bit string.
    Returns list of dict(kind, aw, addr, len, crc, payload, pid, noack, start)."""
    hits = []
    for m in re.finditer(r"(?=(01010101|10101010))", bits[: max_start + 8]):
        k = m.start()
        p = bits[k + 8:]
        for aw in widths:
            a = aw * 8
            if len(p) < a + 9:
                continue
            addr = [int(p[j:j + 8], 2) for j in range(0, a, 8)]
            if esb_preamble(addr) != bits[k:k + 8]:
                continue      # a real preamble matches the address's first bit
            ln = int(p[a:a + 6], 2)
            if ln <= 32:
                end = a + 9 + ln * 8
                for cw in (16, 8):
                    if len(p) >= end + cw and crc_bits(p[:end], cw) == int(p[end:end + cw], 2):
                        hits.append(dict(kind="ESB", aw=aw, addr=addr, len=ln, crc=cw, start=k, end=k + 8 + end + cw,
                                         pid=int(p[a + 6:a + 8], 2), noack=int(p[a + 8]),
                                         payload=[int(p[a + 9 + j:a + 17 + j], 2) for j in range(0, ln * 8, 8)]))
            # plain ShockBurst: fixed payload, no PCF; one running CRC-16
            crc = 0xFFFF
            for i, ch in enumerate(p[: a + 256]):
                fb = ((crc & 0x8000) != 0) ^ (ch == "1")
                crc = ((crc << 1) & 0xFFFF) ^ (0x1021 if fb else 0)
                end = i + 1
                if end > a and (end - a) % 8 == 0 and len(p) >= end + 16 and crc == int(p[end:end + 16], 2):
                    ln = (end - a) // 8
                    hits.append(dict(kind="SB", aw=aw, addr=addr, len=ln, crc=16, start=k, end=k + 8 + end + 16, pid=0, noack=0,
                                     payload=[int(p[a + j:a + 8 + j], 2) for j in range(0, ln * 8, 8)]))
    # A CRC-valid ESB frame also parses as plain ShockBurst with another
    # address width, and (half of the frames, when the CRC register's last
    # feedback bit is zero) ends one bit short.  Drop those ghosts.
    ends = {h["end"] + d for h in hits if h["kind"] == "ESB" for d in (-1, 0, 1)}
    return [h for h in hits if h["kind"] == "ESB" or h["end"] not in ends]


# ------------------------------------------------------------------ Bluetooth Classic
G = int("260534236651", 8)                       # BCH generator g(D), degree 34
PN = [(0x3F2A33DD69B121C1 >> (63 - i)) & 1 for i in range(64)]


def _mod(v):
    while v.bit_length() > 34:
        v ^= G << (v.bit_length() - 35)
    return v


def bt_syncword(lap):
    """Sync word bits s0..s63 (s0 sent first) of a 24-bit LAP, uint8 array."""
    a = [(lap >> i) & 1 for i in range(24)]
    a += [0, 0, 1, 1, 0, 1] if a[23] == 0 else [1, 1, 0, 0, 1, 0]
    x = sum((a[i] ^ PN[34 + i]) << i for i in range(30))
    s = _mod(x << 34) | (x << 34)
    return np.array([((s >> i) & 1) ^ PN[i] for i in range(64)], np.uint8)


def bt_idpacket(lap):
    """4-bit preamble + sync word, as sent by a paging device (68 us total)."""
    sw = bt_syncword(lap)
    pre = [1, 0, 1, 0] if sw[0] == 0 else [0, 1, 0, 1]
    return np.r_[pre, sw].astype(np.uint8)


assert int("".join(map(str, bt_syncword(0x9E8B33))), 2) == 0x475C58CC73345E72

SYN_BIT = np.array([_mod(1 << i) for i in range(64)], np.int64)
SYN_PN = int(np.bitwise_xor.reduce(SYN_BIT[np.array(PN, bool)]))
ERR, _amb = {0: ()}, set()
for _k in (1, 2):
    for _pos in combinations(range(64), _k):
        _syn = int(np.bitwise_xor.reduce(SYN_BIT[list(_pos)]))
        if _syn in ERR and len(ERR[_syn]) == _k:
            _amb.add(_syn)
        ERR.setdefault(_syn, _pos)
for _syn in _amb:
    del ERR[_syn]
_ERRKEYS = np.array(sorted(ERR), np.int64)


def bt_decode(w):
    """64 received bits -> (LAP, wrong bits) or None."""
    syn = int(np.bitwise_xor.reduce(SYN_BIT[w.astype(bool)])) ^ SYN_PN
    pos = ERR.get(syn)
    if pos is None:
        return None
    w = w.copy()
    w[list(pos)] ^= 1
    a = [int(w[34 + i]) for i in range(30)]
    if a[24:] != ([0, 0, 1, 1, 0, 1] if a[23] == 0 else [1, 1, 0, 0, 1, 0]):
        return None
    return sum(a[i] << i for i in range(24)), len(pos)


def bt_find(b):
    """b: uint8 bit array -> list of (LAP, wrong bits) over every 64-bit window."""
    if len(b) < 64:
        return []
    W = np.lib.stride_tricks.sliding_window_view(b, 64)
    syn = np.bitwise_xor.reduce(np.where(W.astype(bool), SYN_BIT, 0), axis=1) ^ SYN_PN
    out = []
    for i in np.flatnonzero(np.isin(syn, _ERRKEYS)):
        r = bt_decode(W[i])
        if r:
            out.append(r)
    return out


# ------------------------------------------------------------------ Bluetooth LE
BLE_AA = 0x8E89BED6


def ble_pattern(aa=BLE_AA):
    """Preamble + access address in air order (LSB first), 40 bits."""
    aab = [(aa >> i) & 1 for i in range(32)]
    pre = [0, 1, 0, 1, 0, 1, 0, 1] if aab[0] == 0 else [1, 0, 1, 0, 1, 0, 1, 0]
    return np.array(pre + aab, np.uint8)


def window_distances(b, pattern):
    """Hamming distance of the pattern to every window of b."""
    n = len(pattern)
    if len(b) < n:
        return np.zeros(0, int)
    W = np.lib.stride_tricks.sliding_window_view(b, n)
    return (W != pattern).sum(1)
