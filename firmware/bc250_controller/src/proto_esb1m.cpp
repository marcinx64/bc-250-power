// SPDX-License-Identifier: GPL-3.0-or-later
// Nordic Enhanced ShockBurst at 1 Mbit/s, 5-byte address, CRC-16: 8BitDo
// controllers, Rii keyboards and other 2.4G dongle devices. Entries live in
// the "dongle" wake list (NVS "rf"). See docs/WAKE_2G4.md.
#include "rf_proto.h"
#include "proto_esb.h"

typedef esb::Esb<5, 16> E5;

extern const rfp::Proto proto_esb1m;   // (const has internal linkage unless declared extern first)

// Listed address, no valid CRC: preamble + address (48 bits) with at most 2
// wrong bits. Random data: 4.2e-12 per window (docs/ADDING_PROTOCOLS.md).
const rfp::Proto proto_esb1m = {
    "esb1m", "Nordic ESB 1 Mbit/s", /*host*/ false, /*keyLen*/ 5, /*kbps*/ 1000,
    /*minBurst*/ 1200,    // 75 us: shorter than any ESB frame with a payload
    /*minSamples*/ 400, /*budget*/ 2,
    /*init*/ nullptr, E5::pattern, E5::findAny, E5::matchListed};
