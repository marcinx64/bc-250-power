// SPDX-License-Identifier: GPL-3.0-or-later
// The detector: bursts -> demodulation -> registered protocols. See rf_proto.h.
#include "rf_proto.h"
#include <string.h>

namespace rfp {

int analyse(rfdsp::Iq iq, rfdsp::Work &w, const Query &q, HitFn on, void *ctx) {
  // Shortest burst of any active protocol.
  unsigned minBlen = ~0u;
  for (int p = 0; p < count(); p++)
    if (q.active >> p & 1) minBlen = at(p)->minBurst < minBlen ? at(p)->minBurst : minBlen;
  if (minBlen == ~0u) return 0;
  rfdsp::Span sp[BURST_MAX];
  rfdsp::Envelope env;
  int nb = rfdsp::findBursts(iq, w, minBlen, BURST_MAX, sp, &env);
  for (int b = 0; b < nb; b++) {
    int off = rfdsp::burstOffset(iq, w, sp[b].s, sp[b].e);
    rfdsp::Burst bu = {};
    int rate = 0;   // kbps of the demodulation in w.mbuf
    Hit hit;
    bool got = false;
    for (int p = 0; p < count() && !got; p++) {
      const Proto *pr = at(p);
      if (!(q.active >> p & 1) || sp[b].blen < pr->minBurst) continue;
      if (rate != pr->kbps) {   // protocols are listed grouped by bit rate: one demodulation each
        bu = rfdsp::demodulate(iq, w, sp[b], off, pr->kbps >= 2000 ? 8 : 16);
        rate = pr->kbps;
      }
      if (bu.len <= pr->minSamples) continue;
      memset(&hit, 0, sizeof(hit));
      hit.proto = pr;
      hit.offsetHz = off;
      hit.khz = q.chKhz + off / 1000;
      int wrong = 0;
      if (pr->findAny(bu, hit.key, &wrong)) {
        hit.exact = wrong == 0;
        hit.wrong = wrong;
        got = true;
      } else {
        // the listed keys of this protocol
        uint64_t pats[32];
        uint8_t map[32];
        int n = 0;
        for (int i = 0; i < q.nlisted && n < 32; i++)
          if (q.listed[i].proto == p && q.listed[i].pat) { pats[n] = q.listed[i].pat; map[n++] = (uint8_t)i; }
        int i = n ? pr->matchListed(bu, pats, n, q.overrideBudget ? q.budget : pr->budget, &wrong) : -1;
        if (i >= 0) {
          memcpy(hit.key, q.listed[map[i]].key, 5);
          hit.exact = false;
          hit.wrong = wrong;
          got = true;
        }
      }
      if (got) on(ctx, hit);
    }
    if (q.trace) q.trace(ctx, sp[b], off, env, got ? &hit : nullptr);
  }
  return nb;
}

}  // namespace rfp
