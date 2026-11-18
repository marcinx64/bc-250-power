// SPDX-License-Identifier: GPL-3.0-or-later
// Classic Bluetooth (BR/EDR) access codes: sync word of a LAP, and checking a
// received 64-bit window (docs/WAKE_BT_CLASSIC.md).
//
// A device that pages another sends ID packets: a 4-bit preamble and the
// 64-bit sync word of the paged device's LAP (the lower 24 bits of its
// BD_ADDR). The sync word is a BCH codeword XOR a fixed PN sequence (Core
// spec Vol 2 Part B 6.3.3.1), so any window can be checked like a CRC and
// the LAP read back from it.
//
// Window layout used here: bits shifted in as received (s0 first), so after
// 64 bits s_i sits at bit 63-i. In that layout the GIAC (LAP 9E8B33) sync
// word is 0x475C58CC73345E72 and the PN sequence is 0x3F2A33DD69B121C1.
// No ESP-IDF dependencies: host_test/btc_selftest.cpp checks it on the PC.
#pragma once
#include <stdint.h>

namespace btc {

static const uint64_t G = 0260534236651ull;          // g(D), degree 34 (octal, as in the spec)
static const uint64_t PN_WIN = 0x3F2A33DD69B121C1ull;  // p0 at bit 63

static inline int bitlen(uint64_t v) { return v ? 64 - __builtin_clzll(v) : 0; }

// v(D) mod g(D); bit i of v is the coefficient of D^i
static inline uint64_t polymod(uint64_t v) {
  while (bitlen(v) > 34) v ^= G << (bitlen(v) - 35);
  return v;
}

static inline uint64_t reverse64(uint64_t v) {
  uint64_t r = 0;
  for (int i = 0; i < 64; i++) { r = r << 1 | (v & 1); v >>= 1; }
  return r;
}

// Sync word of a LAP, window layout (s0 at bit 63)
static inline uint64_t syncWord(uint32_t lap) {
  uint64_t pn = reverse64(PN_WIN);                      // p_i at bit i
  uint32_t a = lap & 0xFFFFFF;
  a |= (uint32_t)((lap >> 23 & 1) ? 0x13 : 0x2C) << 24; // Barker a24..a29: 110010 / 001101, a24 first
  uint64_t x = (a ^ (uint32_t)(pn >> 34)) & 0x3FFFFFFFull;
  uint64_t s = polymod(x << 34) | (x << 34);            // D^34 x~(D) + parity
  return reverse64(s ^ pn);
}

// Syndrome tables: 16 nibbles of the window, 16 values each (2 KiB)
struct Checker {
  uint64_t t[16][16];
  uint64_t synPn, one[64];       // one[b]: syndrome of a single wrong bit at window bit b
  uint64_t synAll;               // syndrome of all 64 bits set (inverted polarity)
  void init() {
    uint64_t syn[64];            // D^i mod g, for s_i = window bit 63-i
    for (int i = 0; i < 64; i++) syn[i] = polymod(1ull << i);
    for (int b = 0; b < 64; b++) one[b] = syn[63 - b];
    for (int k = 0; k < 16; k++)
      for (int v = 0; v < 16; v++) {
        uint64_t s = 0;
        for (int j = 0; j < 4; j++) if (v >> j & 1) s ^= one[4 * k + j];
        t[k][v] = s;
      }
    synPn = syndrome(PN_WIN);
    synAll = syndrome(~0ull);
  }
  inline uint64_t syndrome(uint64_t w) const {
    uint64_t s = 0;
    for (int k = 0; k < 16; k++) s ^= t[k][(w >> (4 * k)) & 15];
    return s;
  }
  // A valid sync word with at most one wrong bit: returns true with the LAP
  // and the number of wrong bits. Bits 0 and 63 share a syndrome (g divides
  // D^63 + 1), so a single error there is not corrected.
  bool check(uint64_t w, uint32_t &lap, int &errs) const {
    uint64_t syn = syndrome(w) ^ synPn;
    errs = 0;
    if (syn) {
      int bit = -1;
      for (int b = 1; b < 63; b++) if (one[b] == syn) { bit = b; break; }
      if (bit < 0) return false;
      w ^= 1ull << bit;
      errs = 1;
    }
    // info bits a_j = s_{34+j} = window bit 29-j; Barker a24..a29 = bits 5..0
    uint32_t a = 0;
    for (int j = 0; j < 24; j++) a |= (uint32_t)(w >> (29 - j) & 1) << j;
    uint32_t barker = (uint32_t)(w & 0x3F);
    if (barker != ((a >> 23 & 1) ? 0x32u : 0x0Du)) return false;
    lap = a;
    return true;
  }
};

}  // namespace btc
