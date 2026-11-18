// SPDX-License-Identifier: GPL-3.0-or-later
// Signal processing for the 2.4G/BT wake: burst finding, carrier offset and
// the FM demodulator. No ESP-IDF dependencies: the firmware (rf.cpp) and the
// host test bench (host_test/) compile the same code. See
// docs/ADDING_PROTOCOLS.md.
//
// Input: one snapshot of rfdsp::IQ_WORDS packed samples at 16 MS/s, as the
// radio's capture engine writes them: bits 0..9 = I, bits 10..19 = -Q, both
// signed 10 bit.
#pragma once
#include <stdint.h>

namespace rfdsp {

constexpr unsigned IQ_WORDS = 16380;      // samples per snapshot (~1 ms)
constexpr unsigned ENVN = IQ_WORDS / 16;  // envelope points (one per 16 samples)
constexpr unsigned MBUF = 5200;           // demodulated samples kept per burst
constexpr int FS = 16000000;              // sample rate

typedef const volatile uint32_t *Iq;      // volatile: the firmware reads the capture bank

static inline int s10(uint32_t v) { return (int)(v << 22) >> 22; }
static inline int sI(uint32_t w) { return s10(w); }
static inline int sQ(uint32_t w) { return -s10(w >> 10); }
static inline uint32_t packIQ(int i, int q) { return ((uint32_t)i & 0x3ff) | (((uint32_t)(-q) & 0x3ff) << 10); }

// Working memory, owned by the caller (the firmware puts it in quiet memory,
// the tests on the heap): sintab 256 x int16, mbuf MBUF x int32, env and
// envSorted ENVN x uint16.
struct Work {
  int16_t *sintab;
  int32_t *mbuf;
  uint16_t *env, *envSorted;
};
void fillSintab(int16_t *tab);   // 256 entries, 256 * sin

// A burst found by its envelope: sample range [s, e) (the envelope lags by
// 64 samples, s is corrected for it).
struct Span { unsigned s, e, blen; };
struct Envelope { uint32_t thr, med; };

// Bursts above 3x the median envelope, at least minBlen samples long, at most
// maxBursts of them. Returns the count.
int findBursts(Iq iq, Work &w, unsigned minBlen, int maxBursts, Span *out, Envelope *info = nullptr);

// Carrier offset of a burst in Hz: peak of a 250 kHz-step DFT between 1 and
// 7.25 MHz either side of the centre, smoothed over +-250 kHz.
int burstOffset(Iq iq, const Work &w, unsigned s, unsigned e);

// A demodulated burst: w.mbuf holds the integrated FM discriminator output,
// one value per sample. The bits of a stream sampled at phase p are
// m[p + spb * i] > mean, p = 0..spb-1.
struct Burst {
  const int32_t *m;
  int len;          // samples in m
  int32_t mean;     // slicing threshold (the burst's mean)
  int spb;          // samples per bit: 16 at 1 Mbit/s, 8 at 2 Mbit/s
  int offsetHz;     // carrier offset used
  inline bool bit(int p, int i) const { return m[p + spb * i] > mean; }
  template <int SPB> inline bool bitT(int p, int i) const { return m[p + SPB * i] > mean; }   // stride known at compile time: faster
  inline int nbits() const { return (len - spb) / spb - 1; }   // bits that fit at every phase
};

// Mixes the burst down by -offsetHz, filters (box of spb/2 samples), takes the
// FM discriminator and integrates it over spb samples (one bit). Fills w.mbuf.
Burst demodulate(Iq iq, Work &w, const Span &sp, int offsetHz, int spb);

}  // namespace rfdsp
