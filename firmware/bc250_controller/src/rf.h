// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>
#include <ArduinoJson.h>

// 2.4G/BT wake (when the wake method is 2.4G; radio.h). See
// docs/WAKE_2G4.md, docs/WAKE_BT_CLASSIC.md and docs/ADDING_PROTOCOLS.md.
//
// While WiFi is connected and the PC is off, the rf task sweeps the WiFi
// channels (WiFi driver "remain on channel"), takes a 1 ms raw snapshot on
// each with the radio's capture engine, demodulates the bursts in it and
// runs the protocol detectors on them (rf_proto.h): Nordic ESB packets of
// the listed 2.4G devices (CRC-valid frame, or preamble + address with <= 2
// wrong bits), and the access codes of listed Bluetooth hosts: a classic
// Bluetooth controller (DualShock 4 ...) switched on pages the host it is
// paired with, and its ID packets carry that host's LAP (exact sync word, or
// <= 3 wrong bits of 64). "Learn" sweeps faster and collects every address
// seen in a valid frame and every LAP seen in a valid sync word.
//
// Addresses: 10 hex digits = a 2.4G device; 6 hex digits (LAP) or a full
// Bluetooth address (12 hex digits, ':' allowed) = a Bluetooth host. Each
// entry also has a protocol id (rfp::Proto::id: "esb1m", "btpage", ...)
// that selects the detector.
//
// The wake rules are the Bluetooth ones: entering `off` disarms every
// entry; an entry arms after it has not been heard for ble_rearm_s since
// then, or for max(ble_rearm_s, 60 s) after being heard while off (just
// added, still searching); hearing an armed entry starts the PC (when
// ble_wake is on). After a restart with the PC off, entries start armed.
void rfInit();   // loads the list, starts the rf task

// proto: the protocol id; empty = the one Learn found the address with, else
// the first protocol of the list the address belongs to.
bool rfWakeAdd(const std::string &addrHex, float mhz, const std::string &name, std::string &err,
               const std::string &proto = std::string());
bool rfWakeRemove(const std::string &addrHex, std::string &err);
void rfWakeClearKind(bool bt);   // empties the Bluetooth Classic hosts (bt) or 2.4G Dongle devices list; also with BLE running
void rfSavedCounts(int &dongles, int &hosts);   // stored entries of each list
bool rfWakeRename(const std::string &addrHex, const std::string &name, std::string &err);
bool rfWakeEnable(const std::string &addrHex, bool on, std::string &err);   // off: never wakes the PC
void rfWakeToJson(JsonArray out);   // items have "proto"
void rfWakeCounts(int &count, int &present, int &armed);
int rfHostCount();   // Bluetooth hosts in the list (counted in rfWakeCounts too)

void rfLearn(bool on, bool bt = false);   // starts (clears old results) or stops learning; bt: Bluetooth hosts, else 2.4G devices
bool rfLearnActive();
void rfLearnToJson(JsonObject out);   // found items have "proto"

// Raw snapshot export (GET /api/rf/snapshot). rfSnapBegin asks the rf task for
// one 1 ms snapshot on WiFi channel ch and waits up to 2 s: 0 = ok (buf holds
// RF_SNAP_BYTES bytes, valid until rfSnapEnd), else an HTTP status and err.
// Call rfSnapHold() between chunks; always finish with rfSnapEnd().
#define RF_SNAP_BYTES 65520
int rfSnapBegin(int ch, const uint8_t *&buf, std::string &err);
void rfSnapHold();
void rfSnapEnd();
