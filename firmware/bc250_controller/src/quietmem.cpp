// SPDX-License-Identifier: GPL-3.0-or-later
#include "quietmem.h"
#include "radio.h"
#include <string.h>
#include <stdint.h>

static uintptr_t next = 0x3fca0000;
static const uintptr_t END = 0x3fcb0000;   // the capture buffer starts here
void *quietAlloc(size_t n) {
  if (!radio2g4()) return nullptr;   // combined build in Bluetooth mode: the bank is heap
  n = (n + 15) & ~(size_t)15;
  if (next + n > END) return nullptr;
  void *p = (void *)next;
  next += n;
  memset(p, 0, n);   // reserved SRAM is not cleared at boot
  return p;
}
