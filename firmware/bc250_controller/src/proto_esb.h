// SPDX-License-Identifier: GPL-3.0-or-later
// Nordic Enhanced ShockBurst, shared by the 1 Mbit/s and 2 Mbit/s modules:
// preamble (0xAA when the address starts with a 1 bit, else 0x55), ALEN
// address bytes, 9-bit packet control field (6-bit payload length, 2-bit
// packet id, no-ack bit), payload, CRC-16 (CCITT, init 0xFFFF) over
// address + control field + payload. The bit rate only changes the samples
// per bit, which the Burst carries.
#pragma once
#include "rf_dsp.h"

namespace esb {

template <int ALEN, int SPB>   // address bytes, samples per bit (16: 1 Mbit/s, 8: 2 Mbit/s)
struct Esb {
  static_assert(ALEN >= 3 && ALEN <= 5, "ESB addresses are 3 to 5 bytes");
  static const int PATBITS = 8 + 8 * ALEN;   // preamble + address

  // preamble + address as one pattern, MSB first
  static uint64_t pattern(const uint8_t *a) {
    uint64_t v = 0;
    for (int i = 0; i < ALEN; i++) v = v << 8 | a[i];
    return ((v >> (8 * ALEN - 1) & 1 ? 0xAAull : 0x55ull) << (8 * ALEN)) | v;
  }

  static uint16_t crc16(const rfdsp::Burst &b, int p, int from, int nbits) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < nbits; i++) {
      bool fb = ((crc >> 15) & 1) ^ b.bitT<SPB>(p, from + i);
      crc = (uint16_t)(crc << 1) ^ (fb ? 0x1021 : 0);
    }
    return crc;
  }

  // A CRC-valid frame anywhere in the burst, at any sample phase.
  static bool findAny(const rfdsp::Burst &b, uint8_t *addr, int *wrong) {
    int nbits = b.nbits();
    for (int p = 0; p < SPB; p++) {
      for (int k = 0; k + PATBITS + 9 + 16 <= nbits; k++) {
        bool ok = true;
        for (int i = 1; i < 8 && ok; i++) ok = b.bitT<SPB>(p, k + i) != b.bitT<SPB>(p, k + i - 1);
        if (!ok || b.bitT<SPB>(p, k + 7) == b.bitT<SPB>(p, k + 8)) continue;
        int ln = 0;
        for (int i = 0; i < 6; i++) ln = ln << 1 | b.bitT<SPB>(p, k + 8 + 8 * ALEN + i);
        if (ln > 32) continue;
        int body = 8 * ALEN + 9 + ln * 8;
        if (k + 8 + body + 16 > nbits) continue;
        uint16_t want = 0;
        for (int i = 0; i < 16; i++) want = want << 1 | b.bitT<SPB>(p, k + 8 + body + i);
        if (crc16(b, p, k + 8, body) != want) continue;
        for (int j = 0; j < ALEN; j++) {
          uint8_t v = 0;
          for (int i = 0; i < 8; i++) v = v << 1 | b.bitT<SPB>(p, k + 8 + j * 8 + i);
          addr[j] = v;
        }
        for (int j = ALEN; j < 5; j++) addr[j] = 0;
        *wrong = 0;
        return true;
      }
    }
    return false;
  }

  // Preamble + address of a listed key with at most `budget` wrong bits (a
  // frame with a bit error somewhere in the payload fails the CRC). Returns
  // the index of the first pattern that matches.
  static int matchListed(const rfdsp::Burst &b, const uint64_t *pats, int n, int budget, int *wrong) {
    int nbits = b.nbits();
    const uint64_t mask = (1ull << PATBITS) - 1;
    for (int p = 0; p < SPB; p++) {
      uint64_t win = 0;
      for (int k = 0; k < nbits; k++) {
        win = ((win << 1) | b.bitT<SPB>(p, k)) & mask;
        if (k < PATBITS - 1) continue;
        for (int i = 0; i < n; i++) {
          if (pats[i] && __builtin_popcountll(win ^ pats[i]) <= budget) {
            *wrong = __builtin_popcountll(win ^ pats[i]);
            return i;
          }
        }
      }
    }
    return -1;
  }
};

}  // namespace esb
