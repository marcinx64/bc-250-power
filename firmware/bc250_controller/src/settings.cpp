// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include <ctype.h>
#include <string.h>
#include "power.h"
#include "util.h"

Settings cfg;

struct IntKey { const char *name; int Settings::*field; int lo, hi; };
struct BoolKey { const char *name; bool Settings::*field; };

static const IntKey INT_KEYS[] = {
  {"led_on", &Settings::led_on, 0, 255},
  {"led_off_max", &Settings::led_off_max, 0, 255},
  {"led_off_period_ms", &Settings::led_off_period_ms, 1000, 20000},
  {"off_detect_ms", &Settings::off_detect_ms, 500, 30000},
  {"start_pwr_delay_ms", &Settings::start_pwr_delay_ms, 500, 10000},
  {"start_timeout_ms", &Settings::start_timeout_ms, 3000, 120000},
  {"shutdown_timeout_ms", &Settings::shutdown_timeout_ms, 10000, 600000},
  {"ble_rearm_s", &Settings::ble_rearm_s, 3, 600},
  {"scan_s", &Settings::scan_s, 30, 600},
  {"quiet_from", &Settings::quiet_from, 0, 1439},
  {"quiet_to", &Settings::quiet_to, 0, 1439},
  {"ntp_check_h", &Settings::ntp_check_h, 0, 720},
  {"hold_reset_s", &Settings::hold_reset_s, 2, 30},
  {"hold_force_s", &Settings::hold_force_s, 3, 60},
};
static const BoolKey BOOL_KEYS[] = {
  {"buzzer", &Settings::buzzer},
  {"start_pwr_pulse", &Settings::start_pwr_pulse},
  {"ble_wake", &Settings::ble_wake},
  {"pson_hold", &Settings::pson_hold},
  {"quiet_on", &Settings::quiet_on},
};

static bool validHostname(const std::string &h) {
  if (h.size() < 1 || h.size() > 31) return false;
  for (size_t i = 0; i < h.size(); i++) {
    char c = h[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

static bool validNtp(const std::string &h) {
  if (h.size() < 1 || h.size() > 63) return false;
  for (size_t i = 0; i < h.size(); i++) {
    char c = h[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
  }
  return true;
}

// POSIX TZ: 1..47 chars from A-Z a-z 0-9 + - , . / : < >. Stricter on top:
// must start with a letter or '<' and contain a digit, so junk never reaches setenv.
static bool validTz(const std::string &t) {
  if (t.size() < 1 || t.size() > 47) return false;
  bool digit = false;
  for (size_t i = 0; i < t.size(); i++) {
    char c = t[i];
    if (isdigit((unsigned char)c)) digit = true;
    else if (!(isalpha((unsigned char)c) || strchr("+-,./:<>", c))) return false;
  }
  return digit && (isalpha((unsigned char)t[0]) || t[0] == '<');
}

static bool validTzName(const std::string &t) {
  if (t.size() > 39) return false;
  for (size_t i = 0; i < t.size(); i++)
    if ((uint8_t)t[i] < 0x20 || (uint8_t)t[i] > 0x7e) return false;   // printable ASCII keeps JSON safe
  return true;
}

void settingsLoad() {
  Nvs p;
  p.open("cfg", false);
  for (auto &k : INT_KEYS) {
    int v = p.getI32(k.name, cfg.*k.field);
    // A corrupt stored value falls back to the default.
    if (v >= k.lo && v <= k.hi) cfg.*k.field = v;
  }
  for (auto &k : BOOL_KEYS) cfg.*k.field = p.getBool(k.name, cfg.*k.field);
  if (cfg.hold_force_s < cfg.hold_reset_s + 1) { Settings d; cfg.hold_reset_s = d.hold_reset_s; cfg.hold_force_s = d.hold_force_s; }
  std::string h = p.getStr("hostname", cfg.hostname);
  if (validHostname(h)) cfg.hostname = h;
  std::string v = p.getStr("ntp_server", cfg.ntp_server);
  if (validNtp(v)) cfg.ntp_server = v;
  v = p.getStr("tz", cfg.tz);
  if (validTz(v)) cfg.tz = v;
  v = p.getStr("tz_name", cfg.tz_name);
  if (validTzName(v)) cfg.tz_name = v;
}

static void commit(const Settings &n);

bool settingsApply(JsonObjectConst in, std::string &err) {
  Settings n = cfg;
  for (auto &k : INT_KEYS) {
    JsonVariantConst v = in[k.name];
    if (v.isNull()) continue;
    if (!v.is<int>()) { err = std::string(k.name) + " must be an integer"; return false; }
    int x = v.as<int>();
    if (x < k.lo || x > k.hi) {
      err = strPrintf("%s out of range (%d..%d)", k.name, k.lo, k.hi);
      return false;
    }
    n.*k.field = x;
  }
  for (auto &k : BOOL_KEYS) {
    JsonVariantConst v = in[k.name];
    if (v.isNull()) continue;
    if (!v.is<bool>()) { err = std::string(k.name) + " must be true or false"; return false; }
    n.*k.field = v.as<bool>();
  }
  JsonVariantConst h = in["hostname"];
  if (!h.isNull()) {
    if (!h.is<const char *>()) { err = "hostname must be a string"; return false; }
    std::string s = h.as<const char *>();
    if (!validHostname(s)) { err = "hostname: 1..31 chars of a-z 0-9 -"; return false; }
    n.hostname = s;
  }
  // Apply live under the control lock (the control task reads these every
  // tick); only plain ints/bools are copied there. The strings are touched
  // with netLock() held only (web handlers + WiFi/time), never by control/BLE.
  if (n.hold_force_s < n.hold_reset_s + 1) {
    err = strPrintf("hold_force_s (%d) must be at least 1 more than hold_reset_s (%d)", n.hold_force_s, n.hold_reset_s);
    return false;
  }
  JsonVariantConst nv = in["ntp_server"];
  if (!nv.isNull()) {
    if (!nv.is<const char *>()) { err = "ntp_server must be a string"; return false; }
    std::string x = nv.as<const char *>();
    if (!validNtp(x)) { err = "ntp_server: 1..63 chars of letters, digits, . -"; return false; }
    n.ntp_server = x;
  }
  JsonVariantConst tv = in["tz"];
  if (!tv.isNull()) {
    if (!tv.is<const char *>()) { err = "tz must be a string"; return false; }
    std::string x = tv.as<const char *>();
    if (!validTz(x)) { err = "tz: POSIX TZ string, 1..47 chars from A-Z a-z 0-9 + - , . / : < >"; return false; }
    n.tz = x;
  }
  JsonVariantConst zv = in["tz_name"];
  if (!zv.isNull()) {
    if (!zv.is<const char *>()) { err = "tz_name must be a string"; return false; }
    std::string x = zv.as<const char *>();
    if (!validTzName(x)) { err = "tz_name: at most 39 printable ASCII chars"; return false; }
    n.tz_name = x;
  }
  commit(n);
  return true;
}

// Makes `n` the settings: live (ints and bools under the control lock), then
// in NVS.
static void commit(const Settings &n) {
  ctlLock();
  for (auto &k : INT_KEYS) cfg.*k.field = n.*k.field;
  for (auto &k : BOOL_KEYS) cfg.*k.field = n.*k.field;
  ctlUnlock();
  cfg.hostname = n.hostname;
  cfg.ntp_server = n.ntp_server; cfg.tz = n.tz; cfg.tz_name = n.tz_name;
  // NVS (flash) write happens outside any lock.
  Nvs p;
  p.open("cfg", true);
  for (auto &k : INT_KEYS) p.setI32(k.name, n.*k.field);
  for (auto &k : BOOL_KEYS) p.setBool(k.name, n.*k.field);
  p.setStr("hostname", n.hostname);
  p.setStr("ntp_server", n.ntp_server);
  p.setStr("tz", n.tz);
  p.setStr("tz_name", n.tz_name);
}

void settingsResetDefaults() {
  Settings d;
  d.hostname = cfg.hostname;   // the network name stays (the page would lose the controller)
  d.ble_wake = cfg.ble_wake;   // the wake switch is not a setting
  commit(d);
}

void settingsToJson(JsonObject out) {
  out["hostname"] = cfg.hostname;
  out["ntp_server"] = cfg.ntp_server;
  out["tz"] = cfg.tz;
  out["tz_name"] = cfg.tz_name;
  for (auto &k : BOOL_KEYS) out[k.name] = cfg.*k.field;
  for (auto &k : INT_KEYS) out[k.name] = cfg.*k.field;
}
