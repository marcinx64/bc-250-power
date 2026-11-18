// SPDX-License-Identifier: GPL-3.0-or-later
// Reference copy of the detection pipeline of the firmware before it was split
// into modules (rf.cpp, ESB 1 Mbit/s + Bluetooth Classic paging hard-wired),
// kept ONLY for the host tests: test_detect compares the modular pipeline
// with it hit for hit on a corpus of synthetic snapshots and times both.
// The code between the markers is copied unchanged; only the state it read
// from the rf task (buffers, lists) is passed in. Not used by the firmware.
#include "legacy_ref.h"
#include "../src/btc.h"
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace legacy {
#define FS 16000000
#define IQ_WORDS 16380u
#define MBUF 5200
#define ENVN (IQ_WORDS / 16)
static const int MATCH_MAX_ERR = 2, BT_MATCH_MAX_ERR = 3;
static const unsigned BURST_MIN = 640, ESB_MIN = 1200;
static const int BURST_MAX = 8;
static const volatile uint32_t *IQ_BUFFER;   // volatile, as in the firmware (the capture bank)
static int16_t sintab[256];
static int32_t mbuf[MBUF];
static uint16_t env[ENVN], envSorted[ENVN];
static btc::Checker *btck;


// ---- begin copy of rf.cpp (git 08f37c2)

static inline int s10(uint32_t v) { return (int)(v << 22) >> 22; }
static inline int sI(uint32_t w) { return s10(w); }
static inline int sQ(uint32_t w) { return -s10(w >> 10); }

// Demodulator state: mix by -offset, 8-sample box filter, normalised FM
// discriminator, 16-sample integration (one bit). step(n) returns the
// integrated value for sample n.
struct Demod {
  uint32_t step, ph = 0;
  int32_t ai = 0, aq = 0, pi = 0, pq = 0, acc = 0;
  int32_t bi[8] = {0}, bq[8] = {0}, dr[16] = {0};
  bool first = true;
  explicit Demod(int offsetHz) { step = (uint32_t)((int64_t)(-offsetHz) * 4294967296LL / FS); }
  inline int32_t next(uint32_t w, unsigned n) {
    int i = sI(w), q = sQ(w);
    int c = sintab[(uint8_t)((ph >> 24) + 64)], s = sintab[ph >> 24];
    ph += step;
    int32_t mi = (i * c - q * s) >> 8, mq = (i * s + q * c) >> 8;
    ai += mi - bi[n & 7]; aq += mq - bq[n & 7];
    bi[n & 7] = mi; bq[n & 7] = mq;
    int32_t re = ai * pi + aq * pq;
    int32_t im = aq * pi - ai * pq;
    pi = ai; pq = aq;
    int32_t mag = (re < 0 ? -re : re) + (im < 0 ? -im : im);
    int32_t d = first ? 0 : im / ((mag >> 10) + 1);
    first = false;
    acc += d - dr[n & 15];
    dr[n & 15] = d;
    return acc;
  }
};

static inline bool bitAt(int p, int i, int32_t mean) { return mbuf[p + 16 * i] > mean; }

static uint16_t crc16Bits(int p, int from, int nbits, int32_t mean) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < nbits; i++) {
    bool fb = ((crc >> 15) & 1) ^ bitAt(p, from + i, mean);
    crc = (uint16_t)(crc << 1) ^ (fb ? 0x1021 : 0);
  }
  return crc;
}

// Find a CRC-valid ESB frame (5-byte address) in mbuf[0..len). Returns true
// and the address if one is found.
static bool findFrame(int len, int32_t mean, uint8_t *addr) {
  int nbits = len / 16 - 1;
  for (int p = 0; p < 16; p++) {
    for (int k = 0; k + 8 + 49 + 16 <= nbits; k++) {
      bool ok = true;
      for (int i = 1; i < 8 && ok; i++) ok = bitAt(p, k + i, mean) != bitAt(p, k + i - 1, mean);
      if (!ok || bitAt(p, k + 7, mean) == bitAt(p, k + 8, mean)) continue;
      int ln = 0;
      for (int i = 0; i < 6; i++) ln = ln << 1 | bitAt(p, k + 48 + i, mean);
      if (ln > 32) continue;
      int body = 40 + 9 + ln * 8;
      if (k + 8 + body + 16 > nbits) continue;
      uint16_t want = 0;
      for (int i = 0; i < 16; i++) want = want << 1 | bitAt(p, k + 8 + body + i, mean);
      if (crc16Bits(p, k + 8, body, mean) != want) continue;
      for (int b = 0; b < 5; b++) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) v = v << 1 | bitAt(p, k + 8 + b * 8 + i, mean);
        addr[b] = v;
      }
      return true;
    }
  }
  return false;
}

// Carrier offset of a burst: peak of a 250 kHz-step DFT between 1 and
// 7.5 MHz either side (skips the DC spike and the band edges), smoothed
// over +-250 kHz (the GFSK deviation). Returns Hz.
static int burstOffset(unsigned s, unsigned e) {
  const int STEPS = 26;               // 1.0 .. 7.25 MHz
  static float pw[2 * STEPS];
  unsigned n0 = s + 64, n1 = std::min(e > 64 ? e - 64 : e, n0 + 2048u);
  for (int k = 0; k < 2 * STEPS; k++) {
    int hz = (k < STEPS ? -(7250000 - k * 250000) : 1000000 + (k - STEPS) * 250000);
    uint32_t step = (uint32_t)((int64_t)(-hz) * 4294967296LL / FS), ph = 0;
    int32_t re = 0, im = 0;
    for (unsigned n = n0; n < n1; n++) {
      uint32_t w = IQ_BUFFER[n];
      int i = sI(w), q = sQ(w);
      int c = sintab[(uint8_t)((ph >> 24) + 64)], sn = sintab[ph >> 24];
      ph += step;
      re += (i * c - q * sn) >> 4;
      im += (i * sn + q * c) >> 4;
    }
    pw[k] = (float)re * re + (float)im * im;
  }
  int best = 0;
  float bestP = -1;
  for (int k = 0; k < 2 * STEPS; k++) {
    float p = pw[k] + (k > 0 && k != STEPS ? pw[k - 1] : 0) + (k < 2 * STEPS - 1 && k != STEPS - 1 ? pw[k + 1] : 0);
    if (p > bestP) { bestP = p; best = k; }
  }
  return best < STEPS ? -(7250000 - best * 250000) : 1000000 + (best - STEPS) * 250000;
}


static int matchListed(int len, int32_t mean, const uint64_t *pats, int npat) {
  int nbits = len / 16 - 1;
  const uint64_t mask = (1ull << 48) - 1;
  for (int p = 0; p < 16; p++) {
    uint64_t win = 0;
    for (int k = 0; k < nbits; k++) {
      win = ((win << 1) | bitAt(p, k, mean)) & mask;
      if (k < 47) continue;
      for (int i = 0; i < npat; i++)
        if (pats[i] && __builtin_popcountll(win ^ pats[i]) <= MATCH_MAX_ERR) return i;
    }
  }
  return -1;
}


// ---- end copy

static Hits *sink;
static void btFound(uint32_t lap, bool exact, int) { sink->push_back({true, {(uint8_t)(lap >> 16), (uint8_t)(lap >> 8), (uint8_t)lap, 0, 0}, exact}); }
// ---- begin copy

static void btScan(int len, int32_t mean, int khz, const uint64_t *sws, const uint32_t *laps, int WAKE_MAX) {
  int nbits = len / 16 - 1, best = BT_MATCH_MAX_ERR + 1, bestI = -1;
  for (int p = 0; p < 16; p++) {
    uint64_t w = 0;
    for (int k = 0; k < nbits; k++) {
      w = w << 1 | bitAt(p, k, mean);
      if (k < 63) continue;
      uint32_t lap;
      int e;
      if (btck->check(w, lap, e)) { btFound(lap, e == 0, khz); return; }
      for (int i = 0; i < WAKE_MAX; i++) {
        if (!sws[i]) continue;
        int d = __builtin_popcountll(w ^ sws[i]);
        if (d < best) { best = d; bestI = i; }
      }
    }
  }
  if (bestI >= 0) btFound(laps[bestI], false, khz);
}

// ---- end copy


void analyse(const uint32_t *iq, const uint64_t *pats, const uint8_t (*addrs)[5], int npat, const uint64_t *sws,
             const uint32_t *laps, int nws, bool btWant, Hits &hits) {
  static bool inited;
  if (!inited) {
    for (int k = 0; k < 256; k++) sintab[k] = (int16_t)lrintf(256.0f * sinf(k * 2 * (float)M_PI / 256));
    btck = new btc::Checker;
    btck->init();
    inited = true;
  }
  IQ_BUFFER = iq;
  sink = &hits;
  const int WAKE_MAX = nws, NPAT = npat;
  (void)WAKE_MAX;
  btWant = btWant && btck;
  // ---- begin copy

  // 64-sample envelope, kept every 16 samples
  int32_t run = 0;
  for (unsigned n = 0; n < IQ_WORDS; n++) {
    uint32_t w = IQ_BUFFER[n];
    run += abs(sI(w)) + abs(sQ(w));
    if (n >= 64) { uint32_t o = IQ_BUFFER[n - 64]; run -= abs(sI(o)) + abs(sQ(o)); }
    if ((n & 15) == 15) env[n >> 4] = (uint16_t)std::min<int32_t>(run >> 2, 65535);
  }
  memcpy(envSorted, env, ENVN * sizeof(uint16_t));
  std::nth_element(envSorted, envSorted + ENVN / 2, envSorted + ENVN);
  uint32_t thr = 3u * envSorted[ENVN / 2] + 8;
  int bursts = 0;
  for (unsigned k = 4; k < ENVN && bursts < BURST_MAX;) {
    if (env[k] <= thr) { k++; continue; }
    unsigned k0 = k;
    while (k < ENVN && env[k] > thr) k++;
    unsigned blen = (k - k0) * 16;
    if (blen < (btWant ? BURST_MIN : ESB_MIN)) continue;
    bool shortB = blen < ESB_MIN;   // only an access code fits
    bursts++;
    unsigned s = (k0 * 16 > 64 ? k0 * 16 - 64 : 0), e = k * 16;   // envelope lags 64 samples
    int off = burstOffset(s, e);
    unsigned d0 = s > 256 ? s - 256 : 0, d1 = std::min(IQ_WORDS, std::min(e + 512, d0 + MBUF));
    Demod dm(off);
    int64_t sum = 0;
    for (unsigned n = d0; n < d1; n++) { int32_t v = dm.next(IQ_BUFFER[n], n - d0); mbuf[n - d0] = v; sum += v; }
    int len = (int)(d1 - d0);
    uint8_t a[5];
    int32_t mean = (int32_t)(sum / len);
    bool got = !shortB && len > 400 && findFrame(len - 16, mean, a), crc = got;
    if (!got && !shortB && len > 400) {
      int i = matchListed(len - 16, mean, pats, NPAT);
      if (i >= 0) {
        memcpy(a, addrs[i], 5); got = true;
      }
    }
    if (got) hits.push_back({false, {a[0], a[1], a[2], a[3], a[4]}, crc});
    if (!got && btWant && len > 1100) btScan(len - 16, mean, 0, sws, laps, nws);
  }
  // ---- end copy
}
}  // namespace legacy
