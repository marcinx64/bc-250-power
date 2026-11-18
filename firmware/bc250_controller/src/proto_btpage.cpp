// SPDX-License-Identifier: GPL-3.0-or-later
// Classic Bluetooth paging: the ID packet a controller sends to its host, a
// 4-bit preamble and the 64-bit sync word of the host's LAP (btc.h). The key
// is the 3-byte LAP; entries live in the "Bluetooth hosts" list (NVS "bth").
// See docs/WAKE_BT_CLASSIC.md.
#include "rf_proto.h"
#include "btc.h"
#include <stdlib.h>
#include <string.h>

static btc::Checker *chk;   // syndrome tables, 2 KiB on the heap

static bool init() {
  chk = (btc::Checker *)malloc(sizeof(btc::Checker));
  if (!chk) return false;
  chk->init();
  return true;
}

static uint64_t pattern(const uint8_t *k) { return btc::syncWord((uint32_t)k[0] << 16 | (uint32_t)k[1] << 8 | k[2]); }

// Any valid sync word (BCH check, one wrong bit corrected) of any LAP. Inverted
// polarity never showed up (DS4, 71 catches), so it is not searched.
static bool findAny(const rfdsp::Burst &b, uint8_t *key, int *wrong) {
  int nbits = b.nbits();
  for (int p = 0; p < 16; p++) {
    uint64_t w = 0;
    for (int k = 0; k < nbits; k++) {
      w = w << 1 | b.bitT<16>(p, k);
      if (k < 63) continue;
      uint32_t lap;
      int e;
      if (chk->check(w, lap, e)) {
        memset(key, 0, 5);
        key[0] = lap >> 16; key[1] = lap >> 8; key[2] = lap;
        *wrong = e;
        return true;
      }
    }
  }
  return false;
}

// A listed host's sync word with at most `budget` wrong bits of 64: the
// closest of all windows.
static int matchListed(const rfdsp::Burst &b, const uint64_t *sws, int n, int budget, int *wrong) {
  int nbits = b.nbits(), best = budget + 1, bestI = -1;
  for (int p = 0; p < 16; p++) {
    uint64_t w = 0;
    for (int k = 0; k < nbits; k++) {
      w = w << 1 | b.bitT<16>(p, k);
      if (k < 63) continue;
      for (int i = 0; i < n; i++) {
        if (!sws[i]) continue;
        int d = __builtin_popcountll(w ^ sws[i]);
        if (d < best) { best = d; bestI = i; }
      }
    }
  }
  if (bestI >= 0) *wrong = best;
  return bestI;
}

extern const rfp::Proto proto_btpage;
const rfp::Proto proto_btpage = {
    "btpage", "Bluetooth Classic paging", /*host*/ true, /*keyLen*/ 3, /*kbps*/ 1000,
    /*minBurst*/ 640,     // 40 us: an ID packet is 68 us
    /*minSamples*/ 1100, /*budget*/ 3,
    init, pattern, findAny, matchListed};
