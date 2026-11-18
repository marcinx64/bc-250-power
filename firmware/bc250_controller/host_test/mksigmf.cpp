// SPDX-License-Identifier: GPL-3.0-or-later
// mksigmf: writes a synthetic SigMF capture for testing rfreplay (and for
// trying the format): a few snapshots with known packets.
//   mksigmf OUT [--datatype ci8|ci16_le|cf32_le] [--rate HZ] [--esb2m]
// writes OUT.sigmf-meta and OUT.sigmf-data:
//   snapshot 0  2437 MHz  ESB 1 Mbit/s, address A1B2C3D4E5, +3 MHz
//   snapshot 1  2412 MHz  Bluetooth paging ID packet, LAP 9A8B7C, -2 MHz
//   snapshot 2  2442 MHz  noise only
//   snapshot 3  2462 MHz  ESB 1 Mbit/s, address 5A6B7C8D9E with one wrong address bit, -5 MHz
//   snapshot 4  2457 MHz  ESB 2 Mbit/s, address A1B2C3D4E5, +4 MHz   (--esb2m)
// --rate writes another core:sample_rate to test the error path.
#include "synth.h"
#include "sigmf.h"
#include <stdio.h>
#include <string.h>

using namespace synth;

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: mksigmf OUT [--datatype ci8|ci16_le|cf32_le] [--rate HZ] [--esb2m]\n"); return 2; }
  std::string out = argv[1], dt = "ci16_le";
  double rate = 16e6;
  bool with2m = false;
  for (int a = 2; a < argc; a++) {
    if (!strcmp(argv[a], "--datatype") && a + 1 < argc) dt = argv[++a];
    else if (!strcmp(argv[a], "--rate") && a + 1 < argc) rate = atof(argv[++a]);
    else if (!strcmp(argv[a], "--esb2m")) with2m = true;
    else { fprintf(stderr, "mksigmf: unknown option %s\n", argv[a]); return 2; }
  }
  const uint8_t A[5] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5}, B[5] = {0x5A, 0x6B, 0x7C, 0x8D, 0x9E};
  Rng r(4242);
  sigmf::Writer w;
  if (!w.open(out, dt, rate)) { fprintf(stderr, "mksigmf: cannot write %s\n", out.c_str()); return 1; }
  w.add(2437e6, oneBurst(r, esbRandom(r, A, 5, 12), 16, 3e6));
  w.add(2412e6, oneBurst(r, btId(0x9A8B7C), 16, -2e6));
  {
    Snap s;
    s.noise(r, 8);
    w.add(2442e6, s.words());
  }
  {
    Bits b = esbRandom(r, B, 5, 10);
    flipBits(b, r, 8, 40, 1);
    w.add(2462e6, oneBurst(r, b, 16, -5e6));
  }
  if (with2m) w.add(2457e6, oneBurst(r, esbRandom(r, A, 5, 12), 8, 4e6));
  if (!w.close("synthetic test capture of host_test/mksigmf (rfreplay end-to-end test)")) return 1;
  printf("wrote %s.sigmf-meta and .sigmf-data (%s, %zu snapshots)\n", out.c_str(), dt.c_str(), w.caps.size());
  return 0;
}
