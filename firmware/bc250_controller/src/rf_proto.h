// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol interface of the 2.4G/BT wake, and the detector that runs the
// registered protocols over a snapshot. No ESP-IDF dependencies. The
// developer guide is docs/ADDING_PROTOCOLS.md.
//
// A protocol module (proto_*.cpp) describes one packet format with a Proto
// and is listed once in rf_proto.cpp. The core (rf.cpp) knows nothing about
// the formats: it keeps the wake lists, hands the detector the keys to look
// for, and gets Hits back.
#pragma once
#include <stdint.h>
#include "rf_dsp.h"

namespace rfp {

struct Proto;

// One detection in a burst.
struct Hit {
  const Proto *proto;
  uint8_t key[5];     // the key as stored in the wake list (proto->keyLen bytes, rest 0)
  bool exact;         // an error-free frame or access code, which wakes alone; else approximate
  int wrong;          // wrong bits found (0 for a valid CRC)
  int offsetHz;       // carrier offset of the burst
  int khz;            // channel centre + offset, for information
};

struct Proto {
  const char *id;        // stable, stored in NVS and shown in the API: "esb1m", "btpage"
  const char *name;      // for people
  bool host;             // wake list: false = 2.4G dongle devices (kind "dongle"), true = Bluetooth hosts (kind "bt")
  uint8_t keyLen;        // bytes of the key (address, LAP)
  uint16_t kbps;         // bit rate: 1000 (16 samples per bit) or 2000 (8)
  uint16_t minBurst;     // shortest burst (samples at 16 MS/s, 16 = 1 us) worth analysing
  uint16_t minSamples;   // demodulated samples needed (the burst plus its margins)
  uint8_t budget;        // wrong bits tolerated in a listed match (see ADDING_PROTOCOLS.md)

  // Once, at start. false: the protocol stays off (e.g. out of memory).
  bool (*init)();
  // The pattern to look for in the bit stream for a listed key.
  uint64_t (*pattern)(const uint8_t *key);
  // Learn: any valid packet in the burst (CRC-valid frame, valid access code),
  // whatever its key. Sets key and wrong (0 = error-free). Scans all phases.
  bool (*findAny)(const rfdsp::Burst &b, uint8_t *key, int *wrong);
  // Listed keys: the first (or best) of the n patterns found with at most
  // `budget` wrong bits; returns its index or -1, wrong = its bit errors.
  int (*matchListed)(const rfdsp::Burst &b, const uint64_t *pats, int n, int budget, int *wrong);
};

// ---- registry (rf_proto.cpp)
int count();                       // protocols built in
const Proto *at(int i);            // registry order = detection order
int find(const char *id);          // index or -1
int defaultFor(bool host);         // index of the protocol an entry of that list gets when none is named
void initAll();                    // runs every init(); the ones that fail stay off
uint32_t readyMask();              // bit i: protocol i is initialised

// ---- detector (rf_detect.cpp)
// A key to look for: the pattern of a wake list entry (or, while learning,
// of an address already seen) and the protocol it belongs to.
struct Listed {
  uint64_t pat;
  uint8_t key[5];
  uint8_t proto;     // registry index
};

struct Query {
  const Listed *listed;
  int nlisted;
  uint32_t active;   // bit i: run protocol i (it has listed keys, or its list is being learnt)
  int chKhz;         // centre of the WiFi channel the snapshot was taken on
  bool overrideBudget;   // tests and rfreplay: use `budget` instead of each protocol's own
  int budget;
  // Optional, called for every burst that was analysed (debug output).
  void (*trace)(void *ctx, const rfdsp::Span &sp, int offsetHz, const rfdsp::Envelope &env, const Hit *hit);
};

typedef void (*HitFn)(void *ctx, const Hit &h);
static const int BURST_MAX = 8;    // bursts analysed per snapshot

// Analyses one snapshot: finds the bursts, demodulates each once per bit
// rate, and gives it to the active protocols in registry order; the first
// protocol that finds something in a burst ends the search in that burst
// (findAny first, then matchListed). Calls `on` for each hit. Returns the
// number of bursts analysed.
int analyse(rfdsp::Iq iq, rfdsp::Work &w, const Query &q, HitFn on, void *ctx);

}  // namespace rfp
