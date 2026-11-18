// SPDX-License-Identifier: GPL-3.0-or-later
// See rf_dsp.h. Plain C++, no ESP-IDF.
#include "rf_dsp.h"
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace rfdsp {

void fillSintab(int16_t *tab) {
  for (int k = 0; k < 256; k++) tab[k] = (int16_t)lrintf(256.0f * sinf(k * 2 * (float)M_PI / 256));
}

int findBursts(Iq iq, Work &w, unsigned minBlen, int maxBursts, Span *out, Envelope *info) {
  // 64-sample envelope, kept every 16 samples
  int32_t run = 0;
  for (unsigned n = 0; n < IQ_WORDS; n++) {
    uint32_t v = iq[n];
    run += abs(sI(v)) + abs(sQ(v));
    if (n >= 64) { uint32_t o = iq[n - 64]; run -= abs(sI(o)) + abs(sQ(o)); }
    if ((n & 15) == 15) w.env[n >> 4] = (uint16_t)std::min<int32_t>(run >> 2, 65535);
  }
  memcpy(w.envSorted, w.env, ENVN * sizeof(uint16_t));
  std::nth_element(w.envSorted, w.envSorted + ENVN / 2, w.envSorted + ENVN);
  uint32_t thr = 3u * w.envSorted[ENVN / 2] + 8;
  if (info) { info->thr = thr; info->med = w.envSorted[ENVN / 2]; }
  int n = 0;
  for (unsigned k = 4; k < ENVN && n < maxBursts;) {
    if (w.env[k] <= thr) { k++; continue; }
    unsigned k0 = k;
    while (k < ENVN && w.env[k] > thr) k++;
    unsigned blen = (k - k0) * 16;
    if (blen < minBlen) continue;
    out[n].s = (k0 * 16 > 64 ? k0 * 16 - 64 : 0);   // envelope lags 64 samples
    out[n].e = k * 16;
    out[n].blen = blen;
    n++;
  }
  return n;
}

int burstOffset(Iq iq, const Work &w, unsigned s, unsigned e) {
  const int STEPS = 26;               // 1.0 .. 7.25 MHz
  static float pw[2 * STEPS];
  unsigned n0 = s + 64, n1 = std::min(e > 64 ? e - 64 : e, n0 + 2048u);
  for (int k = 0; k < 2 * STEPS; k++) {
    int hz = (k < STEPS ? -(7250000 - k * 250000) : 1000000 + (k - STEPS) * 250000);
    uint32_t step = (uint32_t)((int64_t)(-hz) * 4294967296LL / FS), ph = 0;
    int32_t re = 0, im = 0;
    for (unsigned n = n0; n < n1; n++) {
      uint32_t v = iq[n];
      int i = sI(v), q = sQ(v);
      int c = w.sintab[(uint8_t)((ph >> 24) + 64)], sn = w.sintab[ph >> 24];
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

namespace {
// Demodulator state: mix by -offset, box filter, normalised FM discriminator,
// integration over one bit. next(w, n) returns the integrated value for
// sample n.
template <int SPB>
struct Demod {
  static const unsigned bm = SPB / 2 - 1, dm = SPB - 1;   // box length - 1, bit length - 1
  uint32_t step, ph = 0;
  const int16_t *sintab;
  int32_t ai = 0, aq = 0, pi = 0, pq = 0, acc = 0;
  int32_t bi[SPB / 2] = {0}, bq[SPB / 2] = {0}, dr[SPB] = {0};
  bool first = true;
  Demod(const int16_t *tab, int offsetHz) : sintab(tab) {
    step = (uint32_t)((int64_t)(-offsetHz) * 4294967296LL / FS);
  }
  inline int32_t next(uint32_t w, unsigned n) {
    int i = sI(w), q = sQ(w);
    int c = sintab[(uint8_t)((ph >> 24) + 64)], s = sintab[ph >> 24];
    ph += step;
    int32_t mi = (i * c - q * s) >> 8, mq = (i * s + q * c) >> 8;
    ai += mi - bi[n & bm]; aq += mq - bq[n & bm];
    bi[n & bm] = mi; bq[n & bm] = mq;
    int32_t re = ai * pi + aq * pq;
    int32_t im = aq * pi - ai * pq;
    pi = ai; pq = aq;
    int32_t mag = (re < 0 ? -re : re) + (im < 0 ? -im : im);
    int32_t d = first ? 0 : im / ((mag >> 10) + 1);
    first = false;
    acc += d - dr[n & dm];
    dr[n & dm] = d;
    return acc;
  }
};

template <int SPB>
static int64_t run(Iq iq, Work &w, unsigned d0, unsigned d1, int offsetHz) {
  Demod<SPB> dm(w.sintab, offsetHz);
  int64_t sum = 0;
  for (unsigned n = d0; n < d1; n++) { int32_t v = dm.next(iq[n], n - d0); w.mbuf[n - d0] = v; sum += v; }
  return sum;
}
}  // namespace

Burst demodulate(Iq iq, Work &w, const Span &sp, int offsetHz, int spb) {
  unsigned d0 = sp.s > 256 ? sp.s - 256 : 0, d1 = std::min(IQ_WORDS, std::min(sp.e + 512, d0 + MBUF));
  int64_t sum = spb == 8 ? run<8>(iq, w, d0, d1, offsetHz) : run<16>(iq, w, d0, d1, offsetHz);
  Burst b;
  b.m = w.mbuf;
  b.len = (int)(d1 - d0);
  b.mean = (int32_t)(sum / b.len);
  b.spb = spb;
  b.offsetHz = offsetHz;
  return b;
}

}  // namespace rfdsp
