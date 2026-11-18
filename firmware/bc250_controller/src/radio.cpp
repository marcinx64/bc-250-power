// SPDX-License-Identifier: GPL-3.0-or-later
#include "radio.h"

#include "util.h"
#include <esp_bt.h>
#include <esp_heap_caps_init.h>

static bool g4 = false;
bool radio2g4() { return g4; }

bool radioSave(bool on) {
  Nvs p;
  if (!p.open("radio", true)) return false;
  return p.setU8("m", on ? 1 : 0);
}

void radioBoot() {
  Nvs p;
  bool set = false;
  if (p.open("radio", false)) {
    set = p.has("m");
    g4 = p.getU8("m", 0) == 1;
    p.close();
  }
  // No choice stored yet (first start after an update from firmware that
  // had no wake-method setting): 2.4G if there is a 2.4G wake list.
  if (!set && p.open("rf", false)) { g4 = p.getU8("n", 0) > 0; p.close(); }
  if (g4) {
    // Bluetooth never starts in this boot: hand its controller and host
    // memory to the heap.
    esp_bt_mem_release(ESP_BT_MODE_BLE);
  } else {
    // No 2.4G capture in this boot: the 128 KiB bank reserved for it in
    // rf.cpp becomes ordinary heap.
    heap_caps_add_region(0x3fca0000, 0x3fcc0000);
  }
}

const char *radioName() { return radio2g4() ? "2g4" : "ble"; }
