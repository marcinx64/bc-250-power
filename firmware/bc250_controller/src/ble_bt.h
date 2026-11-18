// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ble.cpp's public functions get a bt prefix, and wake.cpp provides the
// ble* API, which goes to Bluetooth or to the 2.4G stand-ins depending on
// the wake method chosen at boot.
#define bleInit btInit
#define bleUpdate btUpdate
#define bleWakeAdd btWakeAdd
#define bleWakeRemove btWakeRemove
#define bleWakeRename btWakeRename
#define bleWakeEnable btWakeEnable
#define bleWakeToJson btWakeToJson
#define bleStartDiscovery btStartDiscovery
#define bleDiscoveryActive btDiscoveryActive
#define bleStopDiscovery btStopDiscovery
#define bleDiscoveryLeftS btDiscoveryLeftS
#define bleDevicesToJson btDevicesToJson
#define bleAdverts btAdverts
#define bleWakeCounts btWakeCounts
