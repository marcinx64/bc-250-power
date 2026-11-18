# SPDX-License-Identifier: GPL-3.0-or-later
"""Burst finding, FM demodulation and bit slicing for 2.4 GHz GFSK signals.

Works at any sample rate that is a multiple of 1 MS/s (16, 40, 80 MS/s).
Ported from the owner's dongle-sniffer scripts (demod.py, bits.py, btc.py).
All functions take standard complex I/Q (see sigmfio.py).
"""
import numpy as np

SPS = 8      # samples per bit after decimation


def lowpass(y, cutoff, fs):
    taps = int(fs / 16e6 * 63) | 1
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / fs * n) * np.hamming(taps)
    return np.convolve(y, h / h.sum(), "same")


def find_bursts(x, fs, thr=8.0, min_us=40, max_us=1100, dc_hz=1e6):
    """Bursts on 1 MHz channels of one snapshot.
    Returns [(start sample, end sample, offset Hz from the centre)].
    1 us frames are transformed to 1 MHz bins; a bin is hot when it is
    thr times its own median; a run of hot frames is a burst, kept on its
    strongest bin only.  The receiver's DC spike (|f| < dc_hz) is skipped."""
    n = int(round(fs / 1e6))
    m = len(x) // n
    if m < 8:
        return []
    S = np.abs(np.fft.fftshift(np.fft.fft(x[: m * n].reshape(m, n), axis=1), axes=1)) ** 2
    k = np.ones(3) / 3
    S = np.apply_along_axis(lambda c: np.convolve(c, k, "same"), 0, S)
    floor = np.median(S, axis=0) + 1e-9
    hot = S > thr * floor
    f = (np.arange(n) - n // 2) * 1e6
    out = []
    for b in range(n):
        if abs(f[b]) < dc_hz:
            continue
        idx = np.flatnonzero(hot[:, b])
        if not len(idx):
            continue
        for r in np.split(idx, np.flatnonzero(np.diff(idx) > 3) + 1):
            if min_us <= r[-1] - r[0] + 1 <= max_us:
                if S[r[0]:r[-1] + 1].sum(0).argmax() == b:
                    out.append((int(r[0] * n), int((r[-1] + 1) * n), float(f[b])))
    return out


def refine_carrier(seg, fs, f0, span=7e5):
    """Centre of mass of the spectrum within +-span of f0 -> offset in Hz."""
    t = np.arange(len(seg)) / fs
    s = seg * np.exp(-2j * np.pi * f0 * t)
    F = np.fft.fftshift(np.fft.fft(s, 4096))
    ff = np.fft.fftshift(np.fft.fftfreq(4096, 1 / fs))
    sel = np.abs(ff) < span
    P = np.abs(F[sel]) ** 2
    return f0 + float((ff[sel] * P).sum() / (P.sum() + 1e-30))


def baseband(x, fs, s, e, f0, pad_us=8, span=7e5):
    """Cut a burst out, move it to 0 Hz (refined), return (samples, offset Hz)."""
    pad = int(pad_us * 1e-6 * fs)
    seg = x[max(s - pad, 0): e + pad]
    f1 = refine_carrier(seg, fs, f0, span)
    t = np.arange(len(seg)) / fs
    return seg * np.exp(-2j * np.pi * f1 * t), f1


def discriminator(y):
    d = np.angle(y[1:] * np.conj(y[:-1]))
    return d - np.median(d)


def estimate_bitrate(y, fs):
    """Bit rate of a baseband burst from its occupied bandwidth (5%..95% of the
    noise-subtracted power; about 0.39 x bit rate for GFSK with h = 0.32,
    BT 0.5).  Returns (rate in bit/s snapped to 250k/500k/1M/2M, raw estimate).
    It is an estimate: analyse.py also tries the neighbouring rates."""
    if len(y) < 256:
        return None, None
    F = np.fft.fftshift(np.abs(np.fft.fft(y * np.hanning(len(y)), 4096)) ** 2)
    f = np.fft.fftshift(np.fft.fftfreq(4096, 1 / fs))
    floor = np.median(F[np.abs(f) > 4e6])
    S = np.where(np.abs(f) < 4e6, np.maximum(F - floor, 0), 0)
    if S.sum() <= 0:
        return None, None
    c = np.cumsum(S) / S.sum()
    bw = f[np.searchsorted(c, 0.95)] - f[np.searchsorted(c, 0.05)]
    raw = bw / 0.39
    std = np.array([250e3, 500e3, 1e6, 2e6])
    return float(std[np.argmin(abs(np.log(std / max(raw, 1.0))))]), float(raw)


def slice_bits(y, fs, bitrate, cutoff_scale=0.65):
    """Baseband burst -> list of (phase, polarity, bits uint8 array, quality).
    Polarity 0: positive frequency deviation = 1 (standard I/Q)."""
    y = lowpass(y, cutoff_scale * bitrate, fs)
    step = max(int(fs // (SPS * bitrate)), 1)
    sps = int(round(fs / step / bitrate))
    y = y[::step]
    d = np.angle(y[1:] * np.conj(y[:-1]))
    m = np.convolve(d, np.ones(sps) / sps, "same")
    # Remove the carrier offset: middle of the 5% and 95% levels inside the
    # burst (not the median: a payload full of zeros would shift it).
    env = np.convolve(np.abs(y[1:]), np.ones(4 * sps) / (4 * sps), "same")
    inside = env > 0.5 * env.max()
    ref = m[inside] if inside.sum() > 8 * sps else m
    m = m - (np.percentile(ref, 5) + np.percentile(ref, 95)) / 2
    out = []
    for ph in range(sps):
        v = m[ph::sps]
        q = float(np.mean(np.abs(v)))
        b = (v > 0).astype(np.uint8)
        out.append((ph, 0, b, q))
        out.append((ph, 1, 1 - b, q))
    return out


def burst_bits(x, fs, burst, bitrate=1e6):
    """Convenience: burst tuple -> (slices, carrier offset Hz)."""
    s, e, f0 = burst
    span = 7e5 if bitrate <= 1e6 else 1.4e6
    y, f1 = baseband(x, fs, s, e, f0, span=span)
    return slice_bits(y, fs, bitrate), f1


def to_str(b):
    return "".join("1" if v else "0" for v in b)


def hexbytes(bits):
    bits = to_str(bits) if not isinstance(bits, str) else bits
    return " ".join(f"{int(bits[j:j + 8].ljust(8, '0'), 2):02X}" for j in range(0, len(bits), 8))
