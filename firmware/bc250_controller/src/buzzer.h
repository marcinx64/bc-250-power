// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// beeps(), beepLong() and buzzerUpdate() touch state owned by the control
// lock: call them only with that lock held (inside the control tick or inside
// a power.cpp function that took it). Nothing else calls them.
void buzzerInit();
void buzzerUpdate();
// n short beeps, 1 to 5 (250 ms on, 200 ms gap). Replaces any sequence in
// progress.
void beeps(int n);
// One long beep (error signal).
void beepLong(uint32_t ms);

// Main task -> control task: true while quiet hours are in effect.
void buzzerSetQuiet(bool quiet);
