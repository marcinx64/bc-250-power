// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <ArduinoJson.h>

struct Settings {
  std::string hostname = "bc250";
  bool buzzer = true;
  int led_on = 255;
  int led_off_max = 120;
  int led_off_period_ms = 6000;
  int off_detect_ms = 3000;
  bool start_pwr_pulse = true;
  int start_pwr_delay_ms = 2000;
  int start_timeout_ms = 15000;
  int shutdown_timeout_ms = 90000;
  // Strings (hostname, ntp_server, tz, tz_name) are touched with netLock()
  // held only (main task, web handlers, wifi/time modules), never by the
  // control or BLE tasks.
  int ntp_check_h = 24;    // 0 = never again after the first answer
  std::string ntp_server = "pool.ntp.org";
  std::string tz = "UTC0";
  std::string tz_name = "";
  bool quiet_on = false;
  int quiet_from = 1320;   // 22:00, minutes after local midnight
  int quiet_to = 420;      // 07:00
  int hold_reset_s = 3;    // section 21
  int hold_force_s = 5;
  bool pson_hold = true;    // section 17: keep the PC on through a restart
  bool ble_wake = true;
  int ble_rearm_s = 30;    // seconds a listed device must be unheard before it arms
  int scan_s = 60;         // Bluetooth scan and 2.4G Learn run this long
};

extern Settings cfg;

void settingsLoad();
// Validates every key present in `in`; nothing is changed unless all are
// valid. On success saves to NVS and applies live. On failure `err` is set.
bool settingsApply(JsonObjectConst in, std::string &err);
void settingsToJson(JsonObject out);
// Every setting back to its default, except the hostname and the
// wake switch; saved and applied live.
void settingsResetDefaults();
