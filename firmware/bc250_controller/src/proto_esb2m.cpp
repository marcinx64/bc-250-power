// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL, off by default (build with -DRF_PROTO_ESB2M). Nordic Enhanced
// ShockBurst at 2 Mbit/s: the worked example of docs/ADDING_PROTOCOLS.md.
// Verified only with synthetic signals on the PC (host_test/); never seen on
// air. The frame format is the 1 Mbit/s one (proto_esb.h), the bit is 8
// samples long, and the demodulator filters for it (rf_dsp.cpp).
//
// The address length (3 to 5 bytes) is a template parameter of Esb<>. Only
// the 5-byte form is registered; for 4 bytes add a second descriptor with
// Esb<4> and another id ("esb2m4"). A 3-byte address has 6 hex digits like a
// Bluetooth LAP, so the API cannot tell it from a host by its text alone.
#ifdef RF_PROTO_ESB2M
#include "rf_proto.h"
#include "proto_esb.h"

typedef esb::Esb<5, 8> E5;

extern const rfp::Proto proto_esb2m;

// Same error budget as at 1 Mbit/s (2 of 48 bits); a 2 Mbit/s bit is only 8
// samples, so the demodulator is noisier: raise the budget only after
// measuring with rfreplay on real captures.
const rfp::Proto proto_esb2m = {
    "esb2m", "Nordic ESB 2 Mbit/s (experimental)", /*host*/ false, /*keyLen*/ 5, /*kbps*/ 2000,
    /*minBurst*/ 800,     // 50 us: a 2 Mbit/s frame with 3 payload bytes (a bare one is 37 us); untested guess
    /*minSamples*/ 400, /*budget*/ 2,
    /*init*/ nullptr, E5::pattern, E5::findAny, E5::matchListed};
#endif
