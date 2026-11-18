// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// Task layout (ESP32-C3 is single core, so priorities decide everything):
//   control  prio 5  every 5 ms   powerUpdate + buzzerUpdate + ledUpdate
//   ble      prio 2  every 25 ms  bleUpdate (scan chunks, arming, wake)
//   main     prio 1               wifiUpdate + timeUpdate + webUpdate (main.cpp)
void controlTaskStart();   // right after powerInit()
void bleTaskStart();       // after bleInit()
// Stack high-water marks in bytes still free (for a one-off log line).
uint32_t controlStackFree();
uint32_t bleStackFree();
