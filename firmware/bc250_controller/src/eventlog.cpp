// SPDX-License-Identifier: GPL-3.0-or-later
#include "eventlog.h"
#include "quietmem.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct Entry { uint32_t t; char msg[EVENT_MSG_LEN]; };
static Entry *ring = nullptr;   // logInit(): quiet memory in 2.4G mode, else heap
static size_t head = 0;   // next slot to write
static size_t count = 0;
static SemaphoreHandle_t lk = nullptr;   // mutex: priority inheritance

static inline void lockLog() { if (lk) xSemaphoreTake(lk, portMAX_DELAY); }
static inline void unlockLog() { if (lk) xSemaphoreGive(lk); }

void logInit() {
  if (!lk) lk = xSemaphoreCreateMutex();
  if (!ring) ring = (Entry *)quietAlloc(EVENT_LOG_SIZE * sizeof(Entry));   // CPU only
  if (!ring) ring = (Entry *)calloc(EVENT_LOG_SIZE, sizeof(Entry));
}

void logEvent(const char *fmt, ...) {
  // Format outside the lock: the lock only covers a short copy.
  char msg[EVENT_MSG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  uint32_t t = nowMs();

  lockLog();
  if (ring) {
    Entry &e = ring[head];
    e.t = t;
    memcpy(e.msg, msg, sizeof(msg));
    head = (head + 1) % EVENT_LOG_SIZE;
    if (count < EVENT_LOG_SIZE) count++;
  }
  unlockLog();

  // One write per line, after the lock. conWrite() never waits: if another
  // task is mid-write, or the host does not read, this line is dropped from
  // the console (it stays in the ring), so lines cannot interleave and
  // nothing blocks.
  char line[EVENT_MSG_LEN + 16];
  int n = snprintf(line, sizeof(line), "[%lu] %s\n", (unsigned long)t, msg);
  if (n > 0) conWrite(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

size_t logCount() {
  lockLog();
  size_t c = count;
  unlockLog();
  return c;
}

bool logGet(size_t i, uint32_t &t, char *msg) {
  lockLog();
  bool ok = i < count;
  if (ok) {
    const Entry &e = ring[(head + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE];
    t = e.t;
    memcpy(msg, e.msg, EVENT_MSG_LEN);
  }
  unlockLog();
  return ok;
}
