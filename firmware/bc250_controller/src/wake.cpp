// SPDX-License-Identifier: GPL-3.0-or-later
// The ble* API used by the web server and the state goes to Bluetooth
// (ble.cpp, its functions renamed bt* by ble_bt.h) or, in 2.4G mode, to
// 2.4G stand-ins (empty Bluetooth lists, counts from rf.cpp).
#include "radio.h"
#include "rf.h"
#include <string>
#include <ArduinoJson.h>
#include "ble_bt.h"
#include "ble.h"   // declares the bt* names
#undef bleInit
#undef bleUpdate
#undef bleWakeAdd
#undef bleWakeRemove
#undef bleWakeRename
#undef bleWakeEnable
#undef bleWakeToJson
#undef bleStartDiscovery
#undef bleDiscoveryActive
#undef bleStopDiscovery
#undef bleDiscoveryLeftS
#undef bleDevicesToJson
#undef bleAdverts
#undef bleWakeCounts
#include "ble.h"   // pragma once: declare the ble* names by hand
void bleInit();
void bleUpdate();
bool bleWakeAdd(const std::string &, const std::string &, const std::string &, std::string &);
bool bleWakeRemove(const std::string &, std::string &);
bool bleWakeRename(const std::string &, const std::string &, std::string &);
bool bleWakeEnable(const std::string &, bool, std::string &);
void bleWakeToJson(JsonArray);
void bleStartDiscovery();
bool bleDiscoveryActive();
void bleStopDiscovery();
int bleDiscoveryLeftS();
void bleDevicesToJson(JsonArray);
uint32_t bleAdverts();
void bleWakeCounts(int &, int &, int &);

static const char *NO_BLE = "BLE is off: the wake method is 2.4G/BT";
void bleInit() { if (!radio2g4()) btInit(); }
void bleUpdate() { if (!radio2g4()) btUpdate(); }
bool bleWakeAdd(const std::string &a, const std::string &t, const std::string &n, std::string &err) {
  if (radio2g4()) { err = NO_BLE; return false; }
  return btWakeAdd(a, t, n, err);
}
bool bleWakeRemove(const std::string &a, std::string &err) {
  if (radio2g4()) { err = NO_BLE; return false; }
  return btWakeRemove(a, err);
}
bool bleWakeRename(const std::string &a, const std::string &n, std::string &err) {
  if (radio2g4()) { err = NO_BLE; return false; }
  return btWakeRename(a, n, err);
}
bool bleWakeEnable(const std::string &a, bool on, std::string &err) {
  if (radio2g4()) { err = NO_BLE; return false; }
  return btWakeEnable(a, on, err);
}
void bleWakeToJson(JsonArray o) { if (!radio2g4()) btWakeToJson(o); }
void bleStartDiscovery() { if (!radio2g4()) btStartDiscovery(); }
bool bleDiscoveryActive() { return radio2g4() ? rfLearnActive() : btDiscoveryActive(); }
void bleStopDiscovery() { if (!radio2g4()) btStopDiscovery(); }
int bleDiscoveryLeftS() { return radio2g4() ? 0 : btDiscoveryLeftS(); }
void bleDevicesToJson(JsonArray o) { if (!radio2g4()) btDevicesToJson(o); }
uint32_t bleAdverts() { return radio2g4() ? 0 : btAdverts(); }
void bleWakeCounts(int &c, int &p, int &a) {
  if (radio2g4()) rfWakeCounts(c, p, a); else btWakeCounts(c, p, a);
}
