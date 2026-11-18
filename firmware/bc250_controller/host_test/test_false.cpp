// SPDX-License-Identifier: GPL-3.0-or-later
// False detections: how often do the detectors fire on traffic that is not
// theirs, measured and compared with the theory of docs/ADDING_PROTOCOLS.md.
//
//  1. Monte Carlo on random bit streams, straight into the protocol
//     functions. With the real error budgets the false rates are far too small
//     to measure (that is the point), so the method is checked with relaxed
//     budgets that give a measurable rate and must agree with the formula
//     sum_{k<=e} C(n,k)/2^n; the real budgets are then run on the same data
//     and must give none.
//  2. The whole pipeline over synthetic snapshots of noise, noise bursts,
//     random GFSK, BLE-like packets, other devices' ESB frames and other
//     hosts' Bluetooth ID packets, with a wake list loaded: no listed key
//     may ever be reported.
//   make test   (or: ./test_false [fast])
#include "pipe.h"
#include "synth.h"
#include <chrono>
#include <stdio.h>

using namespace synth;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// sum_{k<=e} C(n,k) / 2^n
static long double tail(int n, int e) {
  long double s = 0, c = 1;
  for (int k = 0; k <= e; k++) {
    s += c;
    c = c * (n - k) / (k + 1);
  }
  return s / powl(2.0L, n);
}

struct Bits64 {   // fast random bits: splitmix64, not an LFSR (a CRC check on LFSR bits would not look random)
  uint64_t s;
  explicit Bits64(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

// A burst of random samples: every sample phase is an independent random bit stream.
static void randomBurst(Bits64 &g, int32_t *m, int len) {
  for (int i = 0; i < len; i += 64) {
    uint64_t v = g.next();
    for (int j = 0; j < 64 && i + j < len; j++) m[i + j] = (v >> j & 1) ? 1 : -1;
  }
}

static void poissonCheck(const char *what, long hits, long double expect) {
  long double sd = sqrtl(expect > 1 ? expect : 1);
  bool ok = fabsl((long double)hits - expect) <= 4 * sd + 0.1L * expect;
  printf("  %-44s measured %6ld, theory %9.1Lf  %s\n", what, hits, expect, ok ? "ok" : "MISMATCH");
  CHECK(ok, "%s: measured %ld, theory %.1Lf", what, hits, expect);
}

static void monteCarlo(bool fast) {
  printf("1. random bit streams (every sample phase an independent stream, 5200-sample bursts)\n");
  static int32_t m[rfdsp::MBUF];
  Bits64 g(12345);
  const int N = fast ? 5000 : 40000;
  struct Row { const char *id; int spb; };
  Row rows[] = {{"esb1m", 16},
#ifdef RF_PROTO_ESB2M
                {"esb2m", 8},
#endif
                {"btpage", 16}};
  for (const Row &row : rows) {
    int pi = rfp::find(row.id);
    const rfp::Proto *pr = rfp::at(pi);
    int patBits = pr->host ? 64 : 48;
    uint8_t key[5] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5};
    uint64_t pat = pr->pattern(key);
    rfdsp::Burst b = {m, (int)rfdsp::MBUF, 0, row.spb, 0};
    long windows = (long)row.spb * (b.nbits() - patBits + 1);
    // relaxed budget: the largest with <= 5 % windows*p per burst
    int relaxed = 0;
    while (windows * tail(patBits, relaxed + 1) <= 0.05L) relaxed++;
    long hitsRelaxed = 0, hitsReal = 0;
    for (int t = 0; t < N; t++) {
      randomBurst(g, m, rfdsp::MBUF);
      int w;
      if (pr->matchListed(b, &pat, 1, relaxed, &w) >= 0) hitsRelaxed++;
      if (pr->matchListed(b, &pat, 1, pr->budget, &w) >= 0) hitsReal++;
    }
    long double pRelaxed = tail(patBits, relaxed), pReal = tail(patBits, pr->budget);
    printf(" %s: %d bursts x %ld windows of %d bits (one listed key)\n", row.id, N, windows, patBits);
    char what[96];
    snprintf(what, sizeof(what), "%s listed match, relaxed budget %d", row.id, relaxed);
    poissonCheck(what, hitsRelaxed, N * (1 - expl(-windows * pRelaxed)));
    printf("  %-44s measured %6ld, theory %9.2Le per burst x %d = %.2Le  %s\n", "  real budget",
           hitsReal, windows * pReal, N, N * windows * pReal, hitsReal == 0 ? "ok" : "FALSE MATCH");
    printf("  real budget %d: p = %.3Le per window, %.3Le per burst\n", pr->budget, pReal, windows * pReal);
    CHECK(hitsReal == 0, "%s: %ld false listed matches with the real budget", row.id, hitsReal);
  }
  // learn-any detectors
  {
    const rfp::Proto *pr = rfp::at(rfp::find("esb1m"));
    rfdsp::Burst b = {m, (int)rfdsp::MBUF, 0, 16, 0};
    // per position k: alternating 8 preamble bits (2/256), edge (1/2), a length
    // field that is <= 32 and still fits the window, CRC (2^-16)
    long double perPhase = 0;
    long pos = 0;
    for (int k = 0; k + 73 <= b.nbits(); k++, pos += 16) {
      int fit = (b.nbits() - k - 73) / 8 + 1;
      perPhase += (long double)(fit > 33 ? 33 : fit) / 64;
    }
    long double p = (2.0L / 256) * 0.5L * powl(2.0L, -16);
    const int Nf = fast ? 20000 : 200000;
    long hits = 0;
    for (int t = 0; t < Nf; t++) {
      randomBurst(g, m, rfdsp::MBUF);
      uint8_t k[5]; int w;
      if (pr->findAny(b, k, &w)) hits++;
    }
    printf(" esb1m learn-any (CRC-valid frame): %d bursts x %ld positions\n", Nf, pos);
    poissonCheck("esb1m random CRC-valid frames", hits, Nf * 16 * perPhase * p);
  }
  {
    // btpage learn-any: P(window accepted) = 63 * 2^24 / 2^64 (any LAP, one wrong bit corrected)
    static btc::Checker c;
    c.init();
    Bits64 gg(777);
    const long W = fast ? 20000000L : 100000000L;
    long hits = 0;
    for (long i = 0; i < W; i++) { uint32_t lap; int e; if (c.check(gg.next(), lap, e)) hits++; }
    long double p = 63.0L / powl(2.0L, 40);
    printf(" btpage learn-any: %ld random 64-bit windows, accepted %ld, theory %.3Lf (p = %.2Le)\n", W, hits, W * p, p);
    CHECK(hits <= 2, "btpage: %ld random windows accepted", hits);
  }
}

// -------------------------------------------------------------- pipeline
static void pipeline(bool fast) {
  printf("2. whole pipeline over synthetic traffic, wake list loaded (3 dongles, 2 hosts)\n");
  Pipe pp;
  Rng r(31337);
  const uint8_t dk[3][5] = {{0xA1, 0xB2, 0xC3, 0xD4, 0xE5}, {0x5A, 0x6B, 0x7C, 0x8D, 0x9E}, {0x12, 0x34, 0x56, 0x78, 0x9A}};
  const uint8_t hk[2][5] = {{0x9A, 0x8B, 0x7C, 0, 0}, {0x4D, 0x5E, 0x6F, 0, 0}};
  std::vector<rfp::Listed> lk;
  for (auto &k : dk) lk.push_back(listedKey("esb1m", k));
  for (auto &k : hk) lk.push_back(listedKey("btpage", k));
  auto isListed = [&](const rfp::Hit &h) {
    for (auto &l : lk) if (l.proto == rfp::find(h.proto->id) && !memcmp(l.key, h.key, h.proto->keyLen)) return true;
    return false;
  };
  const char *names[] = {"quiet noise", "noise bursts", "random GFSK", "BLE-like packets", "other ESB devices (valid CRC)", "other Bluetooth hosts (valid ID)"};
  const int S = fast ? 300 : 1500;
  long bursts[6] = {0}, learnHits[6] = {0}, listedHits[6] = {0}, snaps[6] = {0};
  double w48 = 0, w64 = 0;
  for (int type = 0; type < 6; type++) {
    for (int s = 0; s < S; s++) {
      Snap sn;
      unsigned pos = 900;
      int nb = type == 0 ? 0 : r.range(1, 3);
      for (int b = 0; b < nb; b++) {
        Bits bits;
        double off = (r.range(0, 1) ? 1 : -1) * (1 + r.u() * 6) * 1e6;
        if (type == 1) {   // WiFi-like energy: strong noise for 50-250 us
          unsigned len = (unsigned)r.range(800, 4000);
          if (pos + len + 1500 > rfdsp::IQ_WORDS) break;
          for (unsigned i = 0; i < len; i++) { sn.I[pos + i] += (float)(90 * r.gauss()); sn.Q[pos + i] += (float)(90 * r.gauss()); }
          pos += len + 900;
          continue;
        }
        if (type == 2) bits = randomBits(r, r.range(60, 280));
        if (type == 3) bits = bleLike(r, r.range(8, 30));
        if (type == 4) {
          uint8_t a[5]; for (auto &v : a) v = (uint8_t)r.u32();
          if (a[0] == dk[0][0] && a[1] == dk[0][1]) a[1] ^= 1;
          bits = esbRandom(r, a, 5, r.range(4, 23));
        }
        if (type == 5) bits = btId(0x100000 + (r.u32() & 0x0FFFFF));
        unsigned len = (unsigned)bits.size() * 16;
        if (pos + len + 1500 > rfdsp::IQ_WORDS) break;
        sn.burst(pos, bits, 16, off, 120 + r.range(0, 200));
        pos += len + 700 + (unsigned)r.range(0, 700);
      }
      sn.noise(r, 5 + r.range(0, 10));
      auto w = sn.words();
      auto res = pp.run(w.data(), lk);
      snaps[type]++;
      bursts[type] += (long)res.bursts.size();
      for (auto &b : res.bursts) {
        unsigned d0 = b.sp.s > 256 ? b.sp.s - 256 : 0, d1 = std::min<unsigned>(rfdsp::IQ_WORDS, std::min(b.sp.e + 512, d0 + rfdsp::MBUF));
        int nbits = ((int)(d1 - d0) - 16) / 16 - 1;
        if (b.sp.blen >= 1200 && nbits > 47) w48 += 16.0 * (nbits - 47);
        if (b.sp.blen >= 640 && nbits > 63) w64 += 16.0 * (nbits - 63);
      }
      for (auto &h : res.hits) {
        if (isListed(h)) listedHits[type]++;
        else learnHits[type]++;
      }
    }
  }
  printf("  %-36s %9s %8s %22s %12s\n", "traffic", "snapshots", "bursts", "valid frame/ID (learn)", "LISTED hits");
  long totalListed = 0;
  for (int t = 0; t < 6; t++) {
    printf("  %-36s %9ld %8ld %22ld %12ld\n", names[t], snaps[t], bursts[t], learnHits[t], listedHits[t]);
    totalListed += listedHits[t];
  }
  long double e48 = w48 * 3 * tail(48, 2), e64 = w64 * 2 * tail(64, 3);
  printf("  windows examined (all phases): %.3g of 48 bits, %.3g of 64 bits\n", w48, w64);
  printf("  expected false listed matches by theory: esb1m %.2Le, btpage %.2Le; measured %ld\n", e48, e64, totalListed);
  CHECK(totalListed == 0, "%ld listed keys reported on traffic that is not theirs", totalListed);
  // other devices' valid frames/IDs are real packets: they must be seen as such (learn), never as the listed keys
  CHECK(learnHits[4] > S / 2, "other ESB devices seen in only %ld snapshots", learnHits[4]);
  CHECK(learnHits[5] > S / 2, "other hosts seen in only %ld snapshots", learnHits[5]);
  // random data must not look like a CRC-valid frame or a valid sync word
  long falseLearn = learnHits[0] + learnHits[1] + learnHits[2] + learnHits[3];
  printf("  valid frames/IDs found in noise, noise bursts, random GFSK and BLE-like packets: %ld\n", falseLearn);
  CHECK(falseLearn <= 2, "%ld false valid frames in non-ESB traffic", falseLearn);
}

int main(int argc, char **argv) {
  bool fast = argc > 1 && !strcmp(argv[1], "fast");
  rfp::initAll();
  monteCarlo(fast);
  pipeline(fast);
  printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
