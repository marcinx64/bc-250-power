// SPDX-License-Identifier: GPL-3.0-or-later
// rfreplay: runs the firmware's detection pipeline (src/rf_dsp.cpp,
// rf_detect.cpp and every protocol module, the same sources) over recorded
// captures and prints every hit.
//
//   rfreplay [options] CAPTURE...          CAPTURE = name, name.sigmf-meta or name.sigmf-data
//     --key PROTO:HEX   look for a listed key as the wake list would (repeatable), e.g.
//                       esb1m:A1B2C3D4E5  btpage:9A8B7C   (adds the approximate matches)
//     --only PROTO      run only this protocol (repeatable); default: every protocol
//     --budget N        error budget override for the listed matches (default: each protocol's own)
//     --gain G          scale the samples (default: ci8 x4, ci16 as 10-bit counts, cf32 x511)
//     --conj            negate Q (a recording with a mirrored spectrum shows no hits)
//     --bursts          also print every burst analysed
//     --quiet           only the summary
//   Without --key it runs as Learn: every valid frame / access code is a hit.
//
// SigMF: datatype ci8, ci16_le or cf32_le, core:sample_rate exactly 16e6, one
// "captures" entry per snapshot (core:frequency in Hz, core:sample_start);
// a capture longer than 16380 samples is analysed in 16380-sample units.
// Exit status: 0 done, 1 bad input, 2 usage.
#include "pipe.h"
#include "sigmf.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>

static int usage() {
  fprintf(stderr,
          "usage: rfreplay [--key PROTO:HEX]... [--only PROTO]... [--budget N] [--gain G] [--conj] [--bursts] [--quiet] CAPTURE...\n"
          "protocols built in:");
  for (int i = 0; i < rfp::count(); i++) fprintf(stderr, " %s", rfp::at(i)->id);
  fprintf(stderr, "\n");
  return 2;
}

static std::string hex(const uint8_t *k, int n) {
  std::string s;
  char b[3];
  for (int i = 0; i < n; i++) { snprintf(b, sizeof(b), "%02X", k[i]); s += b; }
  return s;
}

int main(int argc, char **argv) {
  rfp::initAll();
  std::vector<rfp::Listed> listed;
  uint32_t only = 0;
  int budget = -1;
  double gain = 1;
  bool conj = false, bursts = false, quiet = false;
  std::vector<std::string> files;
  for (int a = 1; a < argc; a++) {
    std::string s = argv[a];
    auto arg = [&]() -> const char * { return a + 1 < argc ? argv[++a] : nullptr; };
    if (s == "--key") {
      const char *v = arg();
      if (!v) return usage();
      std::string kv = v;
      size_t c = kv.find(':');
      int p = c == std::string::npos ? -1 : rfp::find(kv.substr(0, c).c_str());
      if (p < 0) { fprintf(stderr, "--key: unknown protocol in \"%s\"\n", v); return usage(); }
      std::string h = kv.substr(c + 1);
      const rfp::Proto *pr = rfp::at(p);
      if (h.size() != (size_t)pr->keyLen * 2 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        fprintf(stderr, "--key: %s needs %d hex digits\n", pr->id, pr->keyLen * 2);
        return 2;
      }
      uint8_t k[5] = {0};
      for (int i = 0; i < pr->keyLen; i++) k[i] = (uint8_t)strtoul(h.substr(2 * i, 2).c_str(), nullptr, 16);
      listed.push_back(listedKey(pr->id, k));
    } else if (s == "--only") {
      const char *v = arg();
      int p = v ? rfp::find(v) : -1;
      if (p < 0) { fprintf(stderr, "--only: unknown protocol\n"); return usage(); }
      only |= 1u << p;
    } else if (s == "--budget") { const char *v = arg(); if (!v) return usage(); budget = atoi(v); }
    else if (s == "--gain") { const char *v = arg(); if (!v) return usage(); gain = atof(v); }
    else if (s == "--conj") conj = true;
    else if (s == "--bursts") bursts = true;
    else if (s == "--quiet") quiet = true;
    else if (s.size() > 1 && s[0] == '-') return usage();
    else files.push_back(s);
  }
  if (files.empty()) return usage();
  if (!only) only = ~0u;

  Pipe pp;
  int rc = 0;
  for (const std::string &f : files) {
    sigmf::Capture cap;
    std::string err;
    if (!sigmf::load(f, gain, conj, cap, err)) { fprintf(stderr, "rfreplay: %s: %s\n", f.c_str(), err.c_str()); rc = 1; continue; }
    long nb = 0, hitsExact = 0, hitsApprox = 0, listedHits = 0;
    printf("%s: %zu snapshots, %s\n", sigmf::base(f).c_str(), cap.snaps.size(), cap.datatype.c_str());
    for (size_t i = 0; i < cap.snaps.size(); i++) {
      const sigmf::Snapshot &sn = cap.snaps[i];
      if (sn.padded && !quiet) printf("  snapshot %zu is shorter than %u samples: padded with zeros\n", i, rfdsp::IQ_WORDS);
      rfp::Query q = {};
      q.listed = listed.data();
      q.nlisted = (int)listed.size();
      q.active = only & rfp::readyMask();
      q.chKhz = (int)(sn.freqHz / 1000);
      Pipe::Result res;
      q.trace = Pipe::onTrace;
      q.overrideBudget = budget >= 0;
      q.budget = budget;
      rfp::analyse(sn.words.data(), pp.w, q, Pipe::onHit, &res);
      nb += (long)res.bursts.size();
      if (bursts && !quiet)
        for (auto &b : res.bursts)
          printf("  snapshot %zu burst %u..%u (%u us) offset %+d kHz, median %u threshold %u%s\n", i, b.sp.s, b.sp.e, b.sp.blen / 16,
                 b.off / 1000, (unsigned)b.env.med, (unsigned)b.env.thr, b.hit ? " HIT" : "");
      for (const rfp::Hit &h : res.hits) {
        bool isListed = false;
        for (auto &l : listed) isListed |= l.proto == rfp::find(h.proto->id) && !memcmp(l.key, h.key, h.proto->keyLen);
        (h.exact ? hitsExact : hitsApprox)++;
        listedHits += isListed;
        if (!quiet)
          printf("snapshot %zu (sample %llu, %.3f MHz): %s key %s %s wrong %d offset %+d kHz at %.3f MHz%s\n", i,
                 (unsigned long long)sn.sampleStart, sn.freqHz / 1e6, h.proto->id, hex(h.key, h.proto->keyLen).c_str(),
                 h.exact ? "exact" : "approx", h.wrong, h.offsetHz / 1000, h.khz / 1000.0, isListed ? " [listed]" : "");
      }
    }
    printf("summary: %zu snapshots, %ld bursts, %ld exact hits, %ld approximate hits, %ld on listed keys\n", cap.snaps.size(), nb,
           hitsExact, hitsApprox, listedHits);
    if (!hitsExact && !hitsApprox && nb && !conj)
      printf("note: bursts but no hits; if the recording has a mirrored spectrum, try --conj\n");
  }
  return rc;
}
