// SPDX-License-Identifier: GPL-3.0-or-later
// See legacy_ref.cpp.
#pragma once
#include <stdint.h>
#include <vector>
namespace legacy {
struct Hit { bool bt; uint8_t key[5]; bool exact; };
typedef std::vector<Hit> Hits;
// The old analyseSnapshot with its inputs passed in: pats/addrs (ESB patterns
// and addresses, 0 = unused), sws/laps (Bluetooth host sync words and LAPs),
// btWant (host search on).
void analyse(const uint32_t *iq, const uint64_t *pats, const uint8_t (*addrs)[5], int npat, const uint64_t *sws,
             const uint32_t *laps, int nws, bool btWant, Hits &hits);
}
