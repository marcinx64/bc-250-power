// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic 2.4G signals for the host tests: GFSK bursts at 16 MS/s with a
// carrier offset, noise, packed like the radio's capture engine writes them
// (rfdsp::packIQ). Header only.
#pragma once
#include "../src/rf_dsp.h"
#include "../src/btc.h"
#include <math.h>
#include <random>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace synth {

typedef std::vector<uint8_t> Bits;

struct Rng {
  std::mt19937_64 g;
  explicit Rng(uint64_t seed = 1) : g(seed) {}
  double u() { return std::uniform_real_distribution<double>(0, 1)(g); }
  double gauss() { return std::normal_distribution<double>(0, 1)(g); }
  uint32_t u32() { return (uint32_t)g(); }
  int range(int lo, int hi) { return lo + (int)(g() % (uint64_t)(hi - lo + 1)); }   // inclusive
};

static inline void pushByte(Bits &b, uint8_t v) { for (int i = 7; i >= 0; i--) b.push_back(v >> i & 1); }
static inline void pushBits(Bits &b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back(v >> i & 1); }

// ---- packets
// Nordic Enhanced ShockBurst frame: preamble, address (alen bytes), 9-bit
// packet control field, payload, CRC-16 (CCITT, init 0xFFFF).
static inline Bits esbFrame(const uint8_t *addr, int alen, const std::vector<uint8_t> &payload, int pid = 0) {
  Bits b;
  pushByte(b, addr[0] & 0x80 ? 0xAA : 0x55);
  for (int i = 0; i < alen; i++) pushByte(b, addr[i]);
  pushBits(b, (uint64_t)payload.size(), 6);
  pushBits(b, pid & 3, 2);
  pushBits(b, 0, 1);
  for (uint8_t v : payload) pushByte(b, v);
  uint16_t crc = 0xFFFF;
  for (size_t i = 8; i < b.size(); i++) {
    bool fb = ((crc >> 15) & 1) ^ b[i];
    crc = (uint16_t)(crc << 1) ^ (fb ? 0x1021 : 0);
  }
  pushBits(b, crc, 16);
  return b;
}
static inline Bits esbRandom(Rng &r, const uint8_t *addr, int alen, int plen) {
  std::vector<uint8_t> p(plen);
  for (auto &v : p) v = (uint8_t)r.u32();
  return esbFrame(addr, alen, p, r.range(0, 3));
}
// Bluetooth Classic ID packet: 4-bit preamble and the 64-bit sync word of a LAP
// (btc.h window layout: transmitted from bit 63 down to bit 0).
static inline Bits btId(uint32_t lap) {
  uint64_t sw = btc::syncWord(lap);
  Bits b;
  if (sw >> 63 & 1) { b.push_back(1); b.push_back(0); b.push_back(1); b.push_back(0); }
  else { b.push_back(0); b.push_back(1); b.push_back(0); b.push_back(1); }
  pushBits(b, sw, 64);
  return b;
}
// BLE-like advertising packet: preamble 0xAA, random access address, random
// header and payload and a random 24-bit "CRC" (never valid for ESB/BT).
static inline Bits bleLike(Rng &r, int plen) {
  Bits b;
  pushByte(b, 0xAA);
  for (int i = 0; i < 4 + 2 + plen + 3; i++) pushByte(b, (uint8_t)r.u32());
  return b;
}
static inline Bits randomBits(Rng &r, int n) {
  Bits b(n);
  for (auto &v : b) v = r.u32() & 1;
  return b;
}
// Flips k distinct bits of b[from, from+n).
static inline void flipBits(Bits &b, Rng &r, int from, int n, int k) {
  std::vector<int> done;
  while ((int)done.size() < k) {
    int p = from + r.range(0, n - 1);
    bool dup = false;
    for (int d : done) dup |= d == p;
    if (dup) continue;
    done.push_back(p);
    b[p] ^= 1;
  }
}

// ---- a snapshot: float I/Q, bursts summed in, noise last
struct Snap {
  std::vector<float> I, Q;
  Snap() : I(rfdsp::IQ_WORDS, 0.f), Q(rfdsp::IQ_WORDS, 0.f) {}

  // GFSK burst of `bits` starting at sample `start`: Gaussian filter BT 0.5,
  // peak deviation devHz, amplitude amp (10-bit counts), carrier offsetHz
  // from the centre. Two idle bits of carrier before and after.
  void burst(unsigned start, const Bits &bits, int spb, double offsetHz, double amp, double devHz = 250e3, double ph0 = 0) {
    int nb = (int)bits.size() + 4;
    std::vector<double> nrz((size_t)nb * spb, 0.0);
    for (int i = 0; i < nb; i++) {
      int bi = i - 2;
      double v = bi < 0 ? (bits[0] ? 1 : -1) : bi >= (int)bits.size() ? (bits.back() ? 1 : -1) : (bits[bi] ? 1 : -1);
      for (int j = 0; j < spb; j++) nrz[(size_t)i * spb + j] = v;
    }
    double sigma = 0.265 * spb;   // Gaussian BT 0.5, in samples
    int kr = (int)(3 * sigma) + 1;
    std::vector<double> k(2 * kr + 1);
    double ks = 0;
    for (int j = -kr; j <= kr; j++) ks += k[j + kr] = exp(-0.5 * j * j / (sigma * sigma));
    double ph = ph0;
    size_t n = nrz.size();
    for (size_t t = 0; t < n && start + t < rfdsp::IQ_WORDS; t++) {
      double f = 0;
      for (int j = -kr; j <= kr; j++) {
        long idx = (long)t + j;
        double v = idx < 0 ? nrz[0] : idx >= (long)n ? nrz[n - 1] : nrz[idx];
        f += k[j + kr] * v;
      }
      f = f / ks * devHz;
      ph += 2 * M_PI * (offsetHz + f) / rfdsp::FS;
      I[start + t] += (float)(amp * cos(ph));
      Q[start + t] += (float)(amp * sin(ph));
    }
  }
  void noise(Rng &r, double sigma) {
    for (unsigned n = 0; n < rfdsp::IQ_WORDS; n++) { I[n] += (float)(sigma * r.gauss()); Q[n] += (float)(sigma * r.gauss()); }
  }
  std::vector<uint32_t> words() const {
    std::vector<uint32_t> w(rfdsp::IQ_WORDS);
    for (unsigned n = 0; n < rfdsp::IQ_WORDS; n++) {
      int i = (int)lrintf(I[n]), q = (int)lrintf(Q[n]);
      i = i > 511 ? 511 : i < -512 ? -512 : i;
      q = q > 511 ? 511 : q < -512 ? -512 : q;
      w[n] = rfdsp::packIQ(i, q);
    }
    return w;
  }
};

// One packet in noise, at a random place with room for the demodulator's margins.
static inline std::vector<uint32_t> oneBurst(Rng &r, const Bits &bits, int spb, double offsetHz, double amp = 200, double noise = 8) {
  Snap s;
  unsigned len = (unsigned)bits.size() * spb;
  unsigned maxStart = rfdsp::IQ_WORDS - len - 1200;
  s.burst(800 + (unsigned)(r.u() * (maxStart - 800)), bits, spb, offsetHz, amp);
  s.noise(r, noise);
  return s.words();
}

}  // namespace synth
