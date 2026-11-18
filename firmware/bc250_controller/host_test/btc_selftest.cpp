// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of src/btc.h against known values
// (make test; or: g++ -O2 -std=c++17 btc_selftest.cpp -o /tmp/btc && /tmp/btc [LAP...]).
#include "../src/btc.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  int bad = 0;
  if (btc::syncWord(0x9E8B33) != 0x475C58CC73345E72ull) { printf("FAIL GIAC %016llx\n", (unsigned long long)btc::syncWord(0x9E8B33)); bad++; }
  static btc::Checker c; c.init();
  srand(7);
  int n0 = 0, n1 = 0, inv = 0, rnd = 0;
  for (int t = 0; t < 20000; t++) {
    uint32_t lap = ((uint32_t)rand() << 8 ^ rand()) & 0xFFFFFF;
    uint64_t w = btc::syncWord(lap);
    uint32_t got; int e;
    if (c.check(w, got, e) && got == lap && e == 0) n0++;
    int b = 1 + rand() % 62;
    if (c.check(w ^ (1ull << b), got, e) && got == lap && e == 1) n1++;
    if (c.syndrome(~w) == (c.syndrome(w) ^ c.synAll)) inv++;
    uint64_t r = (uint64_t)rand() << 42 ^ (uint64_t)rand() << 21 ^ rand();
    if (c.check(r, got, e)) rnd++;
  }
  printf("exact %d/20000, one wrong bit corrected %d/20000, inverted syndrome rule %d/20000, random windows accepted %d/20000\n", n0, n1, inv, rnd);
  if (n0 != 20000 || n1 != 20000 || inv != 20000) bad++;
  for (int i = 1; i < argc; i++) { uint32_t lap = strtoul(argv[i], 0, 16); printf("%06X %016llx\n", lap, (unsigned long long)btc::syncWord(lap)); }
  printf(bad ? "FAILED\n" : "ALL PASSED\n");
  return bad;
}
