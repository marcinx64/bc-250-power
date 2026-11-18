// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

// "Quiet" memory (2.4G build only): the lower 64 KiB of the radio capture
// bank, 0x3fca0000..0x3fcb0000. It is kept away from the heap because WiFi
// DMA or CPU traffic there during a ~1 ms capture would disturb it, but
// buffers that only the CPU touches, and never while a capture runs (the
// capture holds a critical section, so no other task runs), may live there.
// Never put DMA buffers, the rf task's own stack or anything an interrupt
// touches here.
//
// Only call during start-up (app_main): a simple bump allocator, zeroed memory,
// 16-byte aligned. Returns nullptr in Bluetooth mode or when full;
// callers then fall back to the heap.
void *quietAlloc(size_t n);
