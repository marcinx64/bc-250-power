// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>
#include <ArduinoJson.h>

void bleInit();
void bleUpdate();   // ble task: scan restarts, wake logic, discovery timing

bool bleWakeAdd(const std::string &addr, const std::string &type, const std::string &name, std::string &err);
bool bleWakeRemove(const std::string &addr, std::string &err);
void bleWakeClear();   // removes every entry
bool bleWakeRename(const std::string &addr, const std::string &name, std::string &err);
bool bleWakeEnable(const std::string &addr, bool on, std::string &err);   // off: never wakes the PC
void bleWakeToJson(JsonArray out);
void bleStartDiscovery();
bool bleDiscoveryActive();
void bleStopDiscovery();
int bleDiscoveryLeftS();
void bleDevicesToJson(JsonArray out);
uint32_t bleAdverts();
// Wake list summary for the dashboard: entries, how many are in range now,
// how many are armed (able to start the PC).
void bleWakeCounts(int &count, int &present, int &armed);
