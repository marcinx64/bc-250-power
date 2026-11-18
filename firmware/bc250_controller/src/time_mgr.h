// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

// Clock, timezone and the "quiet now" flag. Everything here runs with
// netLock() held (main task and web handlers; time/TZ functions take newlib
// locks: never call them from the control task). The control task only sees
// buzzerSetQuiet()'s flag.
void timeInit();                 // after settingsLoad()
void timeUpdate();               // main task: ~1 Hz housekeeping
void timeRefresh();              // run the housekeeping on the next timeUpdate()
void timeLinkUp();               // station link came up: re-check at once
bool timeSyncNow(std::string &err);   // POST /api/time/sync: one sync now
int32_t timeLastSyncS();         // seconds since the last SNTP answer, -1 none
bool timeSynced();               // clock plausible (>= 2024-01-01)
uint32_t timeEpoch();
void timeLocalString(char *out, size_t n);   // "" when not synced
bool timeQuietNow();
// POST /api/time. Returns true if applied, false if ignored (SNTP already synced).
bool timeSetManual(uint32_t epoch);
const char *timeSource();   // "ntp" | "browser" | "kept" | "none"
