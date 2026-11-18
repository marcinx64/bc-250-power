// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// Ring buffer of the last 40 events, callable from any task.
//
// LOCK ORDER (whole firmware, outermost first):
//   control lock (power.cpp)  ->  log lock (this file)
//   ble lock (ble.cpp)        ->  log lock
// The log lock is a leaf: nothing else is ever taken while holding it.
// The control lock and the ble lock are never held at the same time.
#define EVENT_LOG_SIZE 40
#define EVENT_MSG_LEN 64

void logInit();   // call once, first thing in app_main (after utilInit)
void logEvent(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t logCount();
// i = 0 is the newest event. Copies the message into msg (EVENT_MSG_LEN bytes).
bool logGet(size_t i, uint32_t &t, char *msg);
