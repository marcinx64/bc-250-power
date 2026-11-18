// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Which wake method runs. Both radios are linked in; one is picked at boot
// from NVS ("radio"/"m"). Switching needs a restart, because each mode
// gives the other's memory back to the heap before anything else starts
// (radioBoot()).

bool radio2g4();          // true: 2.4G wake, false: Bluetooth wake
const char *radioName();  // "2g4" or "ble"
void radioBoot();         // first thing in app_main (after utilInit: it reads NVS)
bool radioSave(bool g4);  // stored for the next boot
