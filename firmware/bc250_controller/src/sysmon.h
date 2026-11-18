// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Health figures for the dashboard and the Firmware page, measured by the
// main task every 5 s:
// - chip temperature: the ESP32-C3's internal sensor, the die temperature,
//   not the room (about +-1..3 C in -10..80 C, Espressif). Never read during
//   a 2.4G capture (a critical section in the rf task).
// - CPU use: 100 % minus the idle task's share of the last 5 s (FreeRTOS
//   run-time statistics). A 2.4G capture counts as busy, which it is.
// The getters are safe from any task.
#include <stdint.h>
void sysmonInit();
void sysmonUpdate();         // main task, cheap when not due
bool chipTempValid();
float chipTempC();           // last reading; only meaningful when valid
int cpuLoadPct();            // 0..100, -1 until the first 5 s have passed
uint32_t heapTotal();        // size of the heap the free figures refer to
