// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware pipeline on the PC: working buffers on the heap, the
// registry initialised, one call per snapshot. Shared by the tests and
// rfreplay.
#pragma once
#include "../src/rf_dsp.h"
#include "../src/rf_proto.h"
#include <stdlib.h>
#include <string.h>
#include <vector>

struct Pipe {
  rfdsp::Work w;
  Pipe() {
    w.sintab = (int16_t *)malloc(sizeof(int16_t) * 256);
    w.mbuf = (int32_t *)malloc(sizeof(int32_t) * rfdsp::MBUF);
    w.env = (uint16_t *)malloc(sizeof(uint16_t) * rfdsp::ENVN);
    w.envSorted = (uint16_t *)malloc(sizeof(uint16_t) * rfdsp::ENVN);
    rfdsp::fillSintab(w.sintab);
    rfp::initAll();
  }
  ~Pipe() { free(w.sintab); free(w.mbuf); free(w.env); free(w.envSorted); }

  struct Trace { rfdsp::Span sp; int off; rfdsp::Envelope env; bool hit; };
  struct Result { std::vector<rfp::Hit> hits; std::vector<Trace> bursts; };

  static void onHit(void *c, const rfp::Hit &h) { ((Result *)c)->hits.push_back(h); }
  static void onTrace(void *c, const rfdsp::Span &sp, int off, const rfdsp::Envelope &env, const rfp::Hit *hit) {
    ((Result *)c)->bursts.push_back({sp, off, env, hit != nullptr});
  }

  // active: bit per registry index (default: every ready protocol); listed: keys to look for
  Result run(const uint32_t *iq, const std::vector<rfp::Listed> &listed, uint32_t active = ~0u, int chKhz = 2437000) {
    Result r;
    rfp::Query q = {};
    q.listed = listed.data();
    q.nlisted = (int)listed.size();
    q.active = active & rfp::readyMask();
    q.chKhz = chKhz;
    q.trace = onTrace;
    rfp::analyse(iq, w, q, onHit, &r);
    return r;
  }
};

// A listed key of a protocol (by id) for Pipe::run.
static inline rfp::Listed listedKey(const char *id, const uint8_t *key) {
  rfp::Listed l = {};
  int p = rfp::find(id);
  l.proto = (uint8_t)p;
  memcpy(l.key, key, rfp::at(p)->keyLen);
  l.pat = rfp::at(p)->pattern(l.key);
  return l;
}
