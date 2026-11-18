// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>
#include <ArduinoJson.h>

void wifiInit();
// Threading: wifiInit() runs once in app_main (netLock not needed yet).
// wifiUpdate() runs in the main task with netLock() held (src/util.h), about
// every 5 ms, and never blocks. The other functions are called by web
// handlers (httpd task) with netLock() held, except wifiConnected() and
// wifiApUp(), which any task may call (rf and ble tasks): they only read
// flags. Start-up has already done nvs_flash_init(), esp_netif_init() and
// esp_event_loop_create_default().
void wifiUpdate();   // never blocks

bool wifiHaveCreds();
bool wifiConnected();
bool wifiApUp();
const char *wifiModeName();   // "sta", "ap", "ap+sta"
bool wifiSaveAndConnect(const std::string &ssid, const std::string &pass, std::string &err);
void wifiForget();            // deferred to wifiUpdate so the HTTP answer goes out first
void wifiStartScan();
bool wifiScanning();
void wifiNetworksToJson(JsonArray out);
void wifiStateToJson(JsonObject out);
std::string wifiStaIp();      // station address, "" when not connected

// Fixed address (section 20)
bool wifiNetSave(bool dhcp, const std::string &ip, const std::string &mask, const std::string &gw,
                 const std::string &dns1, const std::string &dns2, std::string &err);
void wifiNetToJson(JsonObject out);

// For the roaming work: BSSID ("aa:bb:cc:dd:ee:ff") and channel of the AP we
// are connected to; "" and 0 when not connected. Any task with netLock().
void wifiApInfo(std::string &bssid, int &channel);

// Roaming between the access points of the network (802.11k/v/r and the IDF
// roaming app). wifiUpdate() pauses it by itself during a firmware upload, 2.4G
// Learn and the WiFi network scan; wifiRoamAllow() is that switch (main task).
void wifiRoamAllow(bool on);
// True while a WiFi scan (ours, or a roaming scan expected after a weak-signal
// event) or a roam may be using the radio. Any task. The rf task stays off the
// channels meanwhile.
bool wifiRadioBusy();
