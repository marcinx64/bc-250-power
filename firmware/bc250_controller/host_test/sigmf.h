// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal SigMF reader/writer for rfreplay and mksigmf (header only).
//
// <name>.sigmf-meta (JSON) + <name>.sigmf-data (raw interleaved I/Q).
// Supported: global "core:datatype" ci8, ci16_le, cf32_le; "core:sample_rate"
// must be 16e6. Every entry of "captures" is one snapshot (its own
// "core:frequency" in Hz and "core:sample_start"); a capture is analysed in
// units of rfdsp::IQ_WORDS samples, a shorter one is padded with zeros.
#pragma once
#include "../src/rf_dsp.h"
#include <algorithm>
#include <map>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace sigmf {

// ---- tiny JSON
struct Json {
  enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
  double num = 0;
  bool b = false;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;
  const Json *get(const std::string &k) const { auto i = obj.find(k); return t == OBJ && i != obj.end() ? &i->second : nullptr; }
};

struct Parser {
  const std::string &s;
  size_t i = 0;
  std::string err;
  explicit Parser(const std::string &text) : s(text) {}
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
  bool fail(const char *m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }
  bool str_(std::string &out) {
    if (i >= s.size() || s[i] != '"') return fail("string expected");
    i++;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        i++;
        char c = s[i];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'u' ? '?' : c;
        if (c == 'u') i += 4;
      } else out += s[i];
      i++;
    }
    if (i >= s.size()) return fail("unterminated string");
    i++;
    return true;
  }
  bool value(Json &v) {
    ws();
    if (i >= s.size()) return fail("value expected");
    char c = s[i];
    if (c == '{') {
      v.t = Json::OBJ; i++; ws();
      if (i < s.size() && s[i] == '}') { i++; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str_(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("':' expected");
        i++;
        if (!value(v.obj[k])) return false;
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; return true; }
        return fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      v.t = Json::ARR; i++; ws();
      if (i < s.size() && s[i] == ']') { i++; return true; }
      for (;;) {
        v.arr.emplace_back();
        if (!value(v.arr.back())) return false;
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; return true; }
        return fail("',' or ']' expected");
      }
    }
    if (c == '"') { v.t = Json::STR; return str_(v.str); }
    if (s.compare(i, 4, "true") == 0) { v.t = Json::BOOL; v.b = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { v.t = Json::BOOL; v.b = false; i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { v.t = Json::NUL; i += 4; return true; }
    char *end;
    v.num = strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return fail("value expected");
    v.t = Json::NUM;
    i = (size_t)(end - s.c_str());
    return true;
  }
};

// ---- reading
struct Snapshot {
  double freqHz;                // centre frequency of the capture (0 = not given)
  uint64_t sampleStart;
  std::vector<uint32_t> words;  // exactly IQ_WORDS packed samples
  bool padded;                  // the capture was shorter than a snapshot
};
struct Capture {
  std::vector<Snapshot> snaps;
  std::string datatype;
  int skippedTail = 0;          // samples beyond the last whole snapshot unit
};

static inline std::string base(const std::string &path) {
  for (const char *ext : {".sigmf-meta", ".sigmf-data"}) {
    size_t n = strlen(ext);
    if (path.size() > n && path.compare(path.size() - n, n, ext) == 0) return path.substr(0, path.size() - n);
  }
  return path;
}

static inline bool readFile(const std::string &p, std::string &out) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

static inline int clip10(double v) { return v > 511 ? 511 : v < -512 ? -512 : (int)(v < 0 ? v - 0.5 : v + 0.5); }

// gain: multiplies the samples after the default scaling (ci8 x4, ci16 as is,
// cf32 x511: the engine's 10-bit counts); conj: negate Q (a mirrored spectrum).
static inline bool load(const std::string &path, double gain, bool conj, Capture &cap, std::string &err) {
  std::string b = base(path), meta, data;
  if (!readFile(b + ".sigmf-meta", meta)) { err = "cannot read " + b + ".sigmf-meta"; return false; }
  Json j;
  Parser p(meta);
  if (!p.value(j)) { err = b + ".sigmf-meta: " + p.err; return false; }
  const Json *g = j.get("global");
  if (!g) { err = "no \"global\" object in the metadata"; return false; }
  const Json *dt = g->get("core:datatype"), *sr = g->get("core:sample_rate");
  if (!dt || dt->t != Json::STR) { err = "core:datatype missing"; return false; }
  if (!sr || sr->t != Json::NUM) { err = "core:sample_rate missing (must be 16e6)"; return false; }
  if (sr->num != 16e6) {
    char m[160];
    snprintf(m, sizeof(m), "core:sample_rate is %.0f, but the detectors only work at exactly 16000000 samples/s: resample first", sr->num);
    err = m;
    return false;
  }
  cap.datatype = dt->str;
  int bytes = cap.datatype == "ci8" ? 2 : cap.datatype == "ci16_le" ? 4 : cap.datatype == "cf32_le" ? 8 : 0;
  if (!bytes) { err = "core:datatype \"" + cap.datatype + "\" is not supported (use ci8, ci16_le or cf32_le)"; return false; }
  if (!readFile(b + ".sigmf-data", data)) { err = "cannot read " + b + ".sigmf-data"; return false; }
  uint64_t total = data.size() / bytes;
  double gfreq = 0;
  if (const Json *f = g->get("core:frequency")) gfreq = f->num;
  // capture segments
  std::vector<std::pair<uint64_t, double>> segs;
  const Json *caps = j.get("captures");
  if (caps && caps->t == Json::ARR)
    for (const Json &c : caps->arr) {
      const Json *s0 = c.get("core:sample_start"), *fr = c.get("core:frequency");
      segs.push_back({s0 ? (uint64_t)s0->num : 0, fr ? fr->num : gfreq});
    }
  if (segs.empty()) segs.push_back({0, gfreq});
  for (size_t s = 0; s < segs.size(); s++) {
    uint64_t from = segs[s].first, to = s + 1 < segs.size() ? segs[s + 1].first : total;
    if (to > total) to = total;
    if (from >= to) continue;
    for (uint64_t u = from; u < to; u += rfdsp::IQ_WORDS) {
      Snapshot sn;
      sn.freqHz = segs[s].second;
      sn.sampleStart = u;
      sn.padded = to - u < rfdsp::IQ_WORDS;
      sn.words.assign(rfdsp::IQ_WORDS, rfdsp::packIQ(0, 0));
      uint64_t n = std::min<uint64_t>(rfdsp::IQ_WORDS, to - u);
      for (uint64_t k = 0; k < n; k++) {
        const char *q = data.data() + (u + k) * bytes;
        double i, qq;
        if (bytes == 2) { i = (int8_t)q[0] * 4.0; qq = (int8_t)q[1] * 4.0; }
        else if (bytes == 4) { int16_t v[2]; memcpy(v, q, 4); i = v[0]; qq = v[1]; }
        else { float v[2]; memcpy(v, q, 8); i = v[0] * 511.0; qq = v[1] * 511.0; }
        i *= gain; qq *= gain;
        if (conj) qq = -qq;
        sn.words[k] = rfdsp::packIQ(clip10(i), clip10(qq));
      }
      cap.snaps.push_back(std::move(sn));
    }
  }
  return true;
}

// ---- writing (mksigmf)
struct Writer {
  std::string base, datatype;
  double rate;
  std::vector<std::pair<uint64_t, double>> caps;
  FILE *data = nullptr;
  uint64_t n = 0;
  bool open(const std::string &b, const std::string &dt, double sampleRate) {
    base = b; datatype = dt; rate = sampleRate;
    data = fopen((b + ".sigmf-data").c_str(), "wb");
    return data != nullptr;
  }
  // one snapshot of packed words (10-bit I and Q)
  void add(double freqHz, const std::vector<uint32_t> &w) {
    caps.push_back({n, freqHz});
    for (uint32_t v : w) {
      int i = rfdsp::sI(v), q = rfdsp::sQ(v);
      if (datatype == "ci8") { int8_t o[2] = {(int8_t)(i / 4), (int8_t)(q / 4)}; fwrite(o, 1, 2, data); }
      else if (datatype == "ci16_le") { int16_t o[2] = {(int16_t)i, (int16_t)q}; fwrite(o, 2, 2, data); }
      else { float o[2] = {i / 511.0f, q / 511.0f}; fwrite(o, 4, 2, data); }
      n++;
    }
  }
  bool close(const char *description) {
    if (data) fclose(data);
    FILE *f = fopen((base + ".sigmf-meta").c_str(), "w");
    if (!f) return false;
    fprintf(f, "{\n  \"global\": {\"core:datatype\": \"%s\", \"core:sample_rate\": %.0f, \"core:version\": \"1.0.0\",\n"
               "             \"core:description\": \"%s\"},\n  \"captures\": [\n", datatype.c_str(), rate, description);
    for (size_t k = 0; k < caps.size(); k++)
      fprintf(f, "    {\"core:sample_start\": %llu, \"core:frequency\": %.0f}%s\n", (unsigned long long)caps[k].first,
              caps[k].second, k + 1 < caps.size() ? "," : "");
    fprintf(f, "  ],\n  \"annotations\": []\n}\n");
    fclose(f);
    return true;
  }
};

}  // namespace sigmf
