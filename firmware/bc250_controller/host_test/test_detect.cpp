// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the detectors with synthetic GFSK I/Q at 16 MS/s, the
// registry, and an equivalence + timing comparison with the pipeline as it
// was before the modular split (legacy_ref.cpp). Deterministic (fixed seeds).
//   make test   (or: ./test_detect [--timing-only])
#include "pipe.h"
#include "synth.h"
#include "legacy_ref.h"
#include <chrono>
#include <stdio.h>

static int failures = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace synth;

static const uint8_t ADDR_A[5] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5};   // listed
static const uint8_t ADDR_B[5] = {0x5A, 0x6B, 0x7C, 0x8D, 0x9E};   // other
static const uint32_t LAP_A = 0x9A8B7C, LAP_B = 0x4D5E6F;
static uint8_t lapKey[5];

static bool sameKey(const rfp::Hit &h, const uint8_t *k, int n) { return memcmp(h.key, k, n) == 0; }

// ------------------------------------------------------------ registry
static void testRegistry() {
  printf("registry\n");
  CHECK(rfp::count() >= 2, "count %d", rfp::count());
  int lastRate = 0;
  bool seenRate[4000] = {false};
  for (int i = 0; i < rfp::count(); i++) {
    const rfp::Proto *p = rfp::at(i);
    CHECK(p->id && strlen(p->id) <= 7, "id %s too long for the 8-byte NVS field", p->id);
    CHECK(rfp::find(p->id) == i, "find(%s)", p->id);
    for (int j = 0; j < i; j++) CHECK(strcmp(rfp::at(j)->id, p->id) != 0, "duplicate id %s", p->id);
    CHECK(p->keyLen >= 3 && p->keyLen <= 5, "%s keyLen", p->id);
    CHECK(p->kbps == 1000 || p->kbps == 2000, "%s kbps", p->id);
    CHECK(p->pattern && p->findAny && p->matchListed, "%s functions", p->id);
    // protocols of one bit rate sit together (one demodulation per rate)
    if (p->kbps != lastRate) { CHECK(!seenRate[p->kbps], "%s: bit rate %d listed apart", p->id, p->kbps); seenRate[p->kbps] = true; }
    lastRate = p->kbps;
    uint8_t k[5] = {1, 2, 3, 4, 5};
    CHECK(p->pattern(k) != 0, "%s pattern 0", p->id);
    CHECK(rfp::readyMask() >> i & 1, "%s not ready", p->id);
  }
  CHECK(rfp::at(rfp::defaultFor(false))->host == false && strcmp(rfp::at(rfp::defaultFor(false))->id, "esb1m") == 0, "default dongle protocol");
  CHECK(strcmp(rfp::at(rfp::defaultFor(true))->id, "btpage") == 0, "default host protocol");
  CHECK(rfp::find("nope") < 0, "find unknown");
#ifdef RF_PROTO_ESB2M
  CHECK(rfp::find("esb2m") >= 0, "esb2m built in");
#else
  CHECK(rfp::find("esb2m") < 0, "esb2m must be off in the default build");
#endif
}

// ------------------------------------------------------------ ESB (both rates)
static void testEsb(Pipe &pp, Rng &r, const char *id, int spb, int minPct) {
  printf("%s: frames at carrier offsets -7..+7 MHz, payload 3..24 bytes\n", id);
  int tot = 0, ok = 0;
  std::vector<rfp::Listed> none;
  for (int offMHz = -7; offMHz <= 7; offMHz++) {
    if (offMHz == 0) continue;
    int n = 0, good = 0;
    for (int t = 0; t < 8; t++) {
      uint8_t a[5];
      for (auto &v : a) v = (uint8_t)r.u32();
      int plen = t == 0 ? 3 : t == 1 ? 24 : r.range(4, 23);   // the 5200-sample demodulation window holds about 25 bytes
      Bits b = esbRandom(r, a, 5, plen);
      auto iq = oneBurst(r, b, spb, offMHz * 1e6 + (r.u() - 0.5) * 1.2e5);
      auto res = pp.run(iq.data(), none, 1u << rfp::find(id));
      n++;
      if (res.hits.size() == 1 && res.hits[0].exact && sameKey(res.hits[0], a, 5) && res.hits[0].proto == rfp::at(rfp::find(id))) good++;
    }
    printf("  %+d MHz: %d/%d\n", offMHz, good, n);
    tot += n; ok += good;
  }
  printf("  total %d/%d\n", ok, tot);
  CHECK(ok >= tot * minPct / 100, "%s CRC-valid frames found %d/%d", id, ok, tot);

  printf("%s: listed address with wrong bits in preamble + address (budget %d)\n", id, rfp::at(rfp::find(id))->budget);
  auto lk = std::vector<rfp::Listed>{listedKey(id, ADDR_A)};
  for (int k = 0; k <= 4; k++) {
    int n = 0, hit = 0, approx = 0;
    for (int t = 0; t < 30; t++) {
      Bits b = esbRandom(r, ADDR_A, 5, r.range(4, 23));
      if (k) flipBits(b, r, 0, 48, k);
      auto iq = oneBurst(r, b, spb, (r.range(0, 1) ? 1 : -1) * r.range(2, 6) * 1e6);
      auto res = pp.run(iq.data(), lk, 1u << rfp::find(id));
      n++;
      for (auto &h : res.hits) if (sameKey(h, ADDR_A, 5)) {
        hit++;
        if (!h.exact) approx++;
      }
    }
    printf("  %d wrong bits: listed hit %d/%d (approximate %d)\n", k, hit, n, approx);
    if (k == 0) CHECK(hit >= n * 95 / 100, "%s exact hits", id);
    if (k == 1 || k == 2) CHECK(hit >= n * 85 / 100 && approx == hit, "%s %d wrong bits: hit %d/%d", id, k, hit, n);
    if (k >= 3) CHECK(hit <= n / 10, "%s %d wrong bits must not match: %d/%d", id, k, hit, n);
  }
  // a bit error in the payload: CRC fails, the address matches approximately
  int n = 0, hit = 0;
  for (int t = 0; t < 30; t++) {
    Bits b = esbRandom(r, ADDR_A, 5, 20);
    flipBits(b, r, 8 + 40 + 9, 20 * 8, 1);
    auto iq = oneBurst(r, b, spb, 3e6);
    auto res = pp.run(iq.data(), lk, 1u << rfp::find(id));
    n++;
    for (auto &h : res.hits) if (sameKey(h, ADDR_A, 5) && !h.exact && h.wrong == 0) hit++;
  }
  printf("  payload bit error: approximate hit %d/%d\n", hit, n);
  CHECK(hit >= n * 90 / 100, "%s payload error hits %d/%d", id, hit, n);
}

// ------------------------------------------------------------ Bluetooth paging
static void testBt(Pipe &pp, Rng &r) {
  printf("btpage: ID packets at carrier offsets -7..+7 MHz\n");
  int tot = 0, ok = 0;
  std::vector<rfp::Listed> none;
  int bt = 1u << rfp::find("btpage");
  for (int offMHz = -7; offMHz <= 7; offMHz++) {
    if (offMHz == 0) continue;
    int n = 0, good = 0;
    for (int t = 0; t < 8; t++) {
      uint32_t lap = r.u32() & 0xFFFFFF;
      auto iq = oneBurst(r, btId(lap), 16, offMHz * 1e6 + (r.u() - 0.5) * 1.2e5);
      auto res = pp.run(iq.data(), none, bt);
      n++;
      uint8_t k[5] = {(uint8_t)(lap >> 16), (uint8_t)(lap >> 8), (uint8_t)lap, 0, 0};
      if (res.hits.size() == 1 && res.hits[0].exact && sameKey(res.hits[0], k, 5)) good++;
    }
    tot += n; ok += good;
    printf("  %+d MHz: %d/%d\n", offMHz, good, n);
  }
  CHECK(ok >= tot * 85 / 100, "BT ID packets %d/%d", ok, tot);

  printf("btpage: sync word with wrong bits, listed (budget 3) and not listed\n");
  lapKey[0] = (uint8_t)(LAP_A >> 16); lapKey[1] = (uint8_t)(LAP_A >> 8); lapKey[2] = (uint8_t)LAP_A; lapKey[3] = lapKey[4] = 0;
  auto lk = std::vector<rfp::Listed>{listedKey("btpage", lapKey)};
  for (int k = 0; k <= 5; k++) {
    int n = 0, hitL = 0, hitU = 0;
    for (int t = 0; t < 30; t++) {
      Bits b = btId(LAP_A);
      if (k) flipBits(b, r, 4, 64, k);
      auto iq = oneBurst(r, b, 16, (r.range(0, 1) ? 1 : -1) * r.range(2, 6) * 1e6);
      n++;
      auto res = pp.run(iq.data(), lk, bt);
      for (auto &h : res.hits) if (sameKey(h, lapKey, 3)) hitL++;
      auto res2 = pp.run(iq.data(), std::vector<rfp::Listed>(), bt);
      hitU += res2.hits.size() > 0;
    }
    printf("  %d wrong bits: listed %d/%d, any-LAP (not listed) %d/%d\n", k, hitL, n, hitU, n);
    if (k == 0) CHECK(hitL >= n * 85 / 100 && hitU >= n * 85 / 100, "bt exact %d %d", hitL, hitU);
    if (k == 1) CHECK(hitL >= n * 75 / 100, "bt 1 wrong bit listed %d/%d", hitL, n);
    if (k >= 2 && k <= 3) CHECK(hitL >= n * 70 / 100, "bt %d wrong bits listed %d/%d", k, hitL, n);
    if (k == 4) CHECK(hitL <= n / 6, "bt 4 wrong bits (budget 3) matched %d/%d: only by demodulation errors undoing flips", hitL, n);
    if (k >= 5) CHECK(hitL == 0, "bt %d wrong bits must not match: %d/%d", k, hitL, n);
    if (k >= 2) CHECK(hitU <= n / 10, "bt %d wrong bits, not listed, must not be learnt: %d/%d", k, hitU, n);
  }
}

// ------------------------------------------------------------ equivalence with the old pipeline + timing
struct Corpus { std::vector<std::vector<uint32_t>> snaps; };

static Corpus makeCorpus(Rng &r, int count) {
  Corpus c;
  for (int s = 0; s < count; s++) {
    Snap sn;
    int nb = r.range(0, 3);
    unsigned pos = 700;
    for (int b = 0; b < nb; b++) {
      int kind = r.range(0, 5);
      Bits bits;
      switch (kind) {
        case 0: bits = esbRandom(r, ADDR_A, 5, r.range(0, 32)); break;   // incl. frames longer than the window
        case 1: bits = esbRandom(r, ADDR_B, 5, r.range(0, 32)); break;
        case 2: bits = btId(LAP_A); break;
        case 3: bits = btId(LAP_B); break;
        case 4: bits = bleLike(r, r.range(8, 30)); break;
        default: bits = randomBits(r, r.range(60, 250)); break;
      }
      if (kind <= 1 && r.range(0, 2) == 0) flipBits(bits, r, 0, 48, r.range(1, 4));
      if (kind <= 1 && r.range(0, 3) == 0) flipBits(bits, r, 48, (int)bits.size() - 48, r.range(1, 2));
      if (kind >= 2 && kind <= 3 && r.range(0, 2) == 0) flipBits(bits, r, 4, 64, r.range(1, 5));
      unsigned len = (unsigned)bits.size() * 16;
      if (pos + len + 1500 > rfdsp::IQ_WORDS) break;
      sn.burst(pos, bits, 16, (r.range(0, 1) ? 1 : -1) * r.range(10, 70) * 1e5, 120 + r.range(0, 200));
      pos += len + 700 + (unsigned)r.range(0, 800);
    }
    sn.noise(r, 6 + r.range(0, 8));
    c.snaps.push_back(sn.words());
  }
  return c;
}

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void testEquivalence(Pipe &pp, const Corpus &c, bool timingOnly) {
  printf("equivalence with the pre-split pipeline on %zu snapshots, and timing\n", c.snaps.size());
  // wake list: two dongle entries (A and a decoy address), one host (LAP A)
  const uint8_t decoy[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
  struct Cfg { const char *name; bool dongle, host; };
  Cfg cfgs[] = {{"dongles + host listed", true, true}, {"dongles only", true, false}, {"host only", false, true}};
  for (const Cfg &cf : cfgs) {
    // new
    std::vector<rfp::Listed> lk;
    uint32_t active = 0;
    if (cf.dongle) { lk.push_back(listedKey("esb1m", ADDR_A)); lk.push_back(listedKey("esb1m", decoy)); active |= 1u << rfp::find("esb1m"); }
    if (cf.host) { lk.push_back(listedKey("btpage", lapKey)); active |= 1u << rfp::find("btpage"); }
    // old
    uint64_t pats[16] = {0}, sws[10] = {0};
    uint8_t addrs[16][5] = {{0}};
    uint32_t laps[10] = {0};
    if (cf.dongle) {
      memcpy(addrs[0], ADDR_A, 5); pats[0] = rfp::at(rfp::find("esb1m"))->pattern(ADDR_A);
      memcpy(addrs[1], decoy, 5); pats[1] = rfp::at(rfp::find("esb1m"))->pattern(decoy);
    }
    if (cf.host) { sws[0] = btc::syncWord(LAP_A); laps[0] = LAP_A; }
    int diff = 0, hitsTotal = 0;
    if (!timingOnly) {
      for (size_t i = 0; i < c.snaps.size(); i++) {
        legacy::Hits old;
        legacy::analyse(c.snaps[i].data(), pats, addrs, 16, sws, laps, 10, cf.host, old);
        auto res = pp.run(c.snaps[i].data(), lk, active);
        // The old pipeline reported every valid frame; the new one runs a protocol only when it has listed keys
        // (or is learnt), so compare the hits of the protocols that are active in both.
        std::vector<legacy::Hit> a;
        for (auto &h : old) if (h.bt ? cf.host : true) a.push_back(h);
        std::vector<legacy::Hit> b;
        for (auto &h : res.hits) b.push_back({h.proto->host, {h.key[0], h.key[1], h.key[2], h.key[3], h.key[4]}, h.exact});
        if (!cf.dongle) { std::vector<legacy::Hit> a2; for (auto &h : a) if (h.bt) a2.push_back(h); a = a2; }
        bool same = a.size() == b.size();
        for (size_t j = 0; same && j < a.size(); j++) same = a[j].bt == b[j].bt && a[j].exact == b[j].exact && memcmp(a[j].key, b[j].key, a[j].bt ? 3 : 5) == 0;
        if (!same) { diff++; if (diff <= 3) printf("  snapshot %zu differs: old %zu hits, new %zu hits\n", i, a.size(), b.size()); }
        hitsTotal += (int)b.size();
      }
      printf("  [%s] %d hits, %d snapshots differ\n", cf.name, hitsTotal, diff);
      CHECK(diff == 0, "%s: pipeline differs from the pre-split one in %d snapshots", cf.name, diff);
    }
    // timing: best of 3 passes
    double bestOld = 1e9, bestNew = 1e9;
    for (int rep = 0; rep < 3; rep++) {
      double t0 = now();
      for (auto &s : c.snaps) { legacy::Hits o; legacy::analyse(s.data(), pats, addrs, 16, sws, laps, 10, cf.host, o); }
      double t1 = now();
      for (auto &s : c.snaps) pp.run(s.data(), lk, active);
      double t2 = now();
      bestOld = std::min(bestOld, t1 - t0);
      bestNew = std::min(bestNew, t2 - t1);
    }
    printf("  [%s] per snapshot: pre-split %.1f us, modular %.1f us (%.2fx)\n", cf.name, bestOld / c.snaps.size() * 1e6,
           bestNew / c.snaps.size() * 1e6, bestNew / bestOld);
  }
}

int main(int argc, char **argv) {
  bool timingOnly = argc > 1 && !strcmp(argv[1], "--timing-only");
  Pipe pp;
  Rng r(20261117);
  lapKey[0] = (uint8_t)(LAP_A >> 16); lapKey[1] = (uint8_t)(LAP_A >> 8); lapKey[2] = (uint8_t)LAP_A;
  if (!timingOnly) {
    testRegistry();
    testEsb(pp, r, "esb1m", 16, 95);
    testBt(pp, r);
#ifdef RF_PROTO_ESB2M
    testEsb(pp, r, "esb2m", 8, 75);   // experimental: synthetic only
#endif
  }
  Corpus c = makeCorpus(r, 300);
  testEquivalence(pp, c, timingOnly);
  printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? 1 : 0;
}
