// SPDX-License-Identifier: GPL-3.0-or-later
// HTTP API (spec section 9, 12, 13) on esp_http_server, firmware update on
// esp_ota_ops. Routes, answers and sequencing are the same as in the
// Arduino-based 2.1.20 release.
//
// Threading: requests are served one at a time by the esp_http_server task.
// Every handler runs under netLock() (see trampoline()), like the main task's
// wifiUpdate()/timeUpdate()/webUpdate(), so module state stays single-threaded.
// The current request is kept in `R` (only that task touches it).
#include "sysmon.h"
#include "web.h"
#include <atomic>
#include <string.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_attr.h>
#include <esp_flash.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <ArduinoJson.h>
#include "version.h"
#include "settings.h"
#include "power.h"
#include "ble.h"
#include "rf.h"
#include "wifi_mgr.h"
#include "eventlog.h"
#include "auth.h"
#include "util.h"
#include "time_mgr.h"
#include "buildinfo.h"
#include "radio.h"
#include "i18n_data.h"
#include "osupd.h"
#include "ssh.h"
#include <math.h>

// gzip-compressed pages (see touch_buildinfo.py); every browser accepts gzip.
// The ESP-IDF build embeds them through CMake, which names them by file name only.
extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_gz_end");
extern const uint8_t setup_html_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_gz_end");
// PC scripts of the automatic OS updates (pc_scripts/, embedded as text; the end
// symbol is one past the terminating NUL).
extern const char setup_sh_start[] asm("_binary_bc250_setup_sh_start");
extern const char setup_sh_end[] asm("_binary_bc250_setup_sh_end");
extern const char update_sh_start[] asm("_binary_bc250_update_sh_start");
extern const char update_sh_end[] asm("_binary_bc250_update_sh_end");

#define RADIO radioName()

static httpd_req_t *R = nullptr;     // request being served
static bool dropConn = false;        // handler wants the connection closed (upload lost)
static httpd_handle_t httpdH = nullptr;

// Restart bookkeeping (netLock)
static bool restartPending = false;
static bool factoryPending = false;   // erase all of NVS just before the restart
static uint32_t restartDeadline = 0;
static const uint32_t RESTART_SETTLE_MS = 120000;   // wait this long for starting/shutting_down to settle
static uint32_t restartAt = 0;

// Stack use of the server task: lowest free bytes seen at the end of a handler.
static uint32_t webStackMin = 0xFFFFFFFF;
static bool stackLogDue = false;     // log it once after the first firmware upload

// ---- helpers -------------------------------------------------------------

static const char *statusText(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 302: return "302 Found";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 408: return "408 Request Timeout";
    case 409: return "409 Conflict";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    default: return "500 Internal Server Error";
  }
}

static void sendJson(int code, const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  httpd_resp_set_status(R, statusText(code));
  httpd_resp_set_type(R, "application/json");
  httpd_resp_set_hdr(R, "Cache-Control", "no-store");
  httpd_resp_send(R, out.data(), out.size());
}

static void sendOk() {
  httpd_resp_set_status(R, "200 OK");
  httpd_resp_set_type(R, "application/json");
  httpd_resp_set_hdr(R, "Cache-Control", "no-store");
  httpd_resp_sendstr(R, "{\"ok\":true}");
}

static void sendErr(int code, const std::string &msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  sendJson(code, d);
}

static const char *senseModeName(SenseMode m) {
  static const char *n[] = {"real", "lit", "dark", "sim"};
  return n[m];
}

static std::string reqHeader(const char *name) {
  size_t n = httpd_req_get_hdr_value_len(R, name);
  if (n == 0) return "";
  std::string s(n + 1, '\0');
  if (httpd_req_get_hdr_value_str(R, name, &s[0], s.size()) != ESP_OK) return "";
  s.resize(strlen(s.c_str()));
  return s;
}

static std::string cookie() { return reqHeader("Cookie"); }

static bool authed() { return !authPasswordSet() || authValidCookie(cookie()); }

// Time of the last HTTP request, for webRecentlyActive() (any task).
static std::atomic<uint32_t> webLastAt{0};
static std::atomic<bool> webSeen{false};
static std::atomic<bool> otaRunning{false};
static void markActivity() { webLastAt.store(nowMs()); webSeen.store(true); }
bool webRecentlyActive() { return webSeen.load() && nowMs() - webLastAt.load() < 20000; }
bool webOtaRunning() { return otaRunning.load(); }

// Every /api handler except GET /api/auth and POST /api/login starts with this.
static bool requireAuth() {
  markActivity();
  if (authed()) return true;
  sendErr(401, "login required");
  return false;
}

// Parses the request body as a JSON object. Sends the error itself on failure.
static bool parseBody(JsonDocument &doc) {
  size_t len = R->content_len;
  if (len == 0 || len > 2048) {
    if (len > 2048) httpd_resp_set_hdr(R, "Connection", "close");   // body is not read
    sendErr(400, "body must be JSON, max 2 KB");
    return false;
  }
  std::string b(len, '\0');
  size_t got = 0;
  int timeouts = 0;
  while (got < len) {
    int n = httpd_req_recv(R, &b[got], len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) { dropConn = true; return false; }
    got += n;
  }
  if (deserializeJson(doc, b) || !doc.is<JsonObject>()) { sendErr(400, "invalid JSON"); return false; }
  return true;
}

static std::string strField(JsonObjectConst o, const char *k, bool &ok) {
  JsonVariantConst v = o[k];
  if (v.isNull()) return "";
  if (!v.is<const char *>()) { ok = false; return ""; }
  return std::string(v.as<const char *>());
}

static void sendPage(const uint8_t *start, const uint8_t *end) {
  markActivity();
  httpd_resp_set_status(R, "200 OK");
  httpd_resp_set_type(R, "text/html; charset=utf-8");
  httpd_resp_set_hdr(R, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(R, "Content-Encoding", "gzip");
  httpd_resp_send(R, (const char *)start, end - start);
}

// Translation catalogs (ui/i18n, generated into i18n_data.cpp): gzip JSON, no login (the login
// page needs them), revalidated with the ETag so a reload costs a 304.
static void sendI18n(const I18nFile &f) {
  markActivity();
  httpd_resp_set_hdr(R, "ETag", f.etag);
  httpd_resp_set_hdr(R, "Cache-Control", "no-cache");
  if (reqHeader("If-None-Match") == f.etag) {
    httpd_resp_set_status(R, "304 Not Modified");
    httpd_resp_send(R, "", 0);
    return;
  }
  httpd_resp_set_status(R, "200 OK");
  httpd_resp_set_type(R, "application/json; charset=utf-8");
  httpd_resp_set_hdr(R, "Content-Encoding", "gzip");
  httpd_resp_send(R, (const char *)f.data, f.len);
}

// Local address of the request's socket is the AP's (192.168.4.1)?
static bool viaAp() {
  int fd = httpd_req_to_sockfd(R);
  struct sockaddr_in a;
  socklen_t l = sizeof(a);
  if (fd < 0 || getsockname(fd, (struct sockaddr *)&a, &l) != 0) return false;
  return a.sin_addr.s_addr == htonl(0xC0A80401u);
}

// Captive portal: while the AP is up, anything unknown that came in over
// the AP goes to the wizard. Requests via the LAN get a plain 404.
static void notFoundOrCaptive() {
  if (wifiApUp() && viaAp()) {
    httpd_resp_set_status(R, "302 Found");
    httpd_resp_set_hdr(R, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(R, "Cache-Control", "no-store");
    httpd_resp_set_type(R, "text/plain");
    httpd_resp_send(R, "", 0);
    return;
  }
  if (strncmp(R->uri, "/api/", 5) == 0) sendErr(404, "not found");
  else {
    httpd_resp_set_status(R, "404 Not Found");
    httpd_resp_set_type(R, "text/plain");
    httpd_resp_sendstr(R, "not found");
  }
}

// ---- handlers ------------------------------------------------------------

static void hRoot() {
  if (!wifiHaveCreds()) sendPage(setup_html_start, setup_html_end);
  else sendPage(index_html_start, index_html_end);
}

static void hSetup() { sendPage(setup_html_start, setup_html_end); }

static void hState() {
  if (!requireAuth()) return;
  PowerSnapshot p = powerSnapshot();   // control lock taken once, released here
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["state"] = powerStateName(p.state);
  d["sense"] = p.senseLit;
  d["sense_raw"] = p.senseRaw;
  d["pson"] = p.pson;
  d["pwr"] = p.pwr;
  d["rst"] = p.rst;
  d["button"] = p.button;
  d["button_held_ms"] = p.heldMs;
  d["button_live"] = p.pressLive;
  d["hold"] = cfg.pson_hold;
  d["resumed"] = powerResumed();
  d["resume"] = resumeResultName(powerResumeResult());
  d["error"] = p.error;
  // Section 25: how long the BC-250 has been running, or since it stopped.
  if (p.running) { d["on_s"] = p.sinceS; d["off_s"] = nullptr; }
  else { d["on_s"] = nullptr; d["off_s"] = p.sinceS; }
  d["off_since_start"] = p.stopAtStart;
  d["uptime_s"] = nowMs() / 1000;
  d["heap"] = (uint32_t)esp_get_free_heap_size();
  d["heap_total"] = heapTotal();
  if (chipTempValid()) d["chip_temp_c"] = chipTempC(); else d["chip_temp_c"] = nullptr;
  if (cpuLoadPct() >= 0) d["cpu"] = cpuLoadPct(); else d["cpu"] = nullptr;
  wifiStateToJson(d["wifi"].to<JsonObject>());
  JsonObject b = d["ble"].to<JsonObject>();
  b["radio"] = RADIO;   // the wake method running now
  b["max"] = 8;         // wake list size, both radios (2.4G: devices)
  b["host_max"] = 2;    // 2.4G: Bluetooth hosts
  b["wake"] = cfg.ble_wake;
  b["scanning"] = bleDiscoveryActive();   // ble lock, taken after the control lock was released
  b["adverts"] = bleAdverts();
  int wc, wp, wa;
  bleWakeCounts(wc, wp, wa);
  b["count"] = wc;      // wake list entries
  b["present"] = wp;    // of those, in range right now
  b["armed"] = wa;      // of those, able to start the PC
  if (radio2g4()) b["hosts"] = rfHostCount();   // of those, Bluetooth hosts
  d["debug"]["sense"] = senseModeName(p.mode);
  JsonObject t = d["time"].to<JsonObject>();
  char local[24];
  timeLocalString(local, sizeof(local));
  t["synced"] = timeSynced();
  t["source"] = timeSource();
  t["last_sync_s"] = timeLastSyncS();
  t["epoch"] = timeEpoch();   // uint32: exact in JSON
  t["local"] = local;
  t["tz_name"] = cfg.tz_name;
  t["quiet"] = timeQuietNow();
  sendJson(200, d);
}

static void hPower() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  std::string a = strField(d.as<JsonObjectConst>(), "action", ok);
  std::string err;
  if (!ok || !powerAction(a, err)) { sendErr(409, ok ? err : std::string("action must be a string")); return; }
  sendOk();
}

static void hConfigGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  settingsToJson(d.to<JsonObject>());
  sendJson(200, d);
}

// Settings back to their defaults: not the password, WiFi, IP
// settings, hostname, wake method, wake lists or the wake switch.
static void hConfigReset() {
  if (!requireAuth()) return;
  settingsResetDefaults();
  timeRefresh();
  logEvent("settings reset to defaults");
  sendOk();
}

static void hConfigPost() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  std::string err;
  if (!settingsApply(d.as<JsonObjectConst>(), err)) { sendErr(400, err); return; }
  timeRefresh();   // tz / quiet hours take effect now
  sendOk();
}

// Why we asked for a restart, kept across the restart in RTC memory (which a
// software reset does not clear) so the UI can say more than "restart".
static const uint32_t WHY_MAGIC = 0xBC250000;
enum { WHY_WEB = 1, WHY_UPDATE = 2 };
static RTC_NOINIT_ATTR uint32_t rtcRestartWhy;
static uint32_t bootWhy = 0;   // captured once in webInit()

static const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power connected";
    case ESP_RST_SW:
      if (bootWhy == WHY_UPDATE) return "firmware update";
      if (bootWhy == WHY_WEB) return "restart from the web UI";
      return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_USB: return "reset over USB";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

// Size of the running firmware image: header, segments, checksum pad, SHA-256
// (what Arduino's ESP.getSketchSize() reported). Reads a few bytes of flash.
static uint32_t runningImageSize(const esp_partition_t *part) {
  if (!part) return 0;
  uint8_t h[24];
  if (esp_partition_read(part, 0, h, sizeof(h)) != ESP_OK || h[0] != 0xE9) return 0;
  uint32_t pos = 24;
  for (int i = 0; i < h[1]; i++) {
    uint32_t sh[2];
    if (esp_partition_read(part, pos, sh, sizeof(sh)) != ESP_OK) return 0;
    pos += 8 + sh[1];
  }
  pos = (pos + 1 + 15) & ~15u;   // checksum byte, 16-byte padding
  if (h[23]) pos += 32;          // appended SHA-256
  return pos;
}

// Static facts about this build and chip, for the Firmware page.
static void hInfo() {
  if (!requireAuth()) return;
  // Measuring the app size reads the image header from flash: do it once.
  static uint32_t appSize = 0, appSlot = 0;
  const esp_partition_t *part = esp_ota_get_running_partition();
  if (!appSize) {
    appSize = runningImageSize(part);
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    appSlot = next ? (next->size & ~0xFFFu) : 0;
  }
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macs[18];
  snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["build"] = buildTime();
  d["marker"] = fwMarker();   // also keeps the marker string in the image
  d["idf"] = esp_get_idf_version();
  d["chip"] = "ESP32-C3";
  d["chip_rev"] = ci.revision;
  d["cpu_mhz"] = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  d["mac"] = macs;
  d["flash_size"] = flashSize;
  d["app_size"] = appSize;
  d["app_slot"] = appSlot;
  d["partition"] = part ? part->label : "";
  d["heap"] = (uint32_t)esp_get_free_heap_size();
  d["heap_total"] = heapTotal();
  if (chipTempValid()) d["chip_temp_c"] = chipTempC(); else d["chip_temp_c"] = nullptr;
  if (cpuLoadPct() >= 0) d["cpu"] = cpuLoadPct(); else d["cpu"] = nullptr;
  d["heap_min"] = (uint32_t)esp_get_minimum_free_heap_size();
  d["uptime_s"] = (uint32_t)(nowMs() / 1000);
  d["reset_reason"] = resetReasonName();
  d["resumed"] = powerResumed();
  d["resume"] = resumeResultName(powerResumeResult());
  d["radio"] = RADIO;
  sendJson(200, d);
}

static void hLog() {
  if (!requireAuth()) return;
  JsonDocument d;
  d["now"] = nowMs();
  JsonArray ev = d["events"].to<JsonArray>();
  uint32_t t; char m[EVENT_MSG_LEN];
  for (size_t i = 0; logGet(i, t, m); i++) {
    JsonObject o = ev.add<JsonObject>();
    o["t"] = t;
    o["msg"] = cleanText(m, 63);
  }
  sendJson(200, d);
}

static void hWakeGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  bleWakeToJson(d["list"].to<JsonArray>());
  sendJson(200, d);
}

static void hWakeAdd() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string addr = strField(o, "addr", ok), type = strField(o, "type", ok), name = strField(o, "name", ok);
  std::string err;
  if (!ok) { sendErr(400, "fields must be strings"); return; }
  addr = lowerCase(addr);
  if (!bleWakeAdd(addr, type, name, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hWakeRemove() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  std::string addr = strField(d.as<JsonObjectConst>(), "addr", ok), err;
  if (!ok) { sendErr(400, "addr must be a string"); return; }
  addr = lowerCase(addr);
  if (!bleWakeRemove(addr, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hWakeRename() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string addr = strField(o, "addr", ok), name = strField(o, "name", ok), err;
  if (!ok) { sendErr(400, "addr and name must be strings"); return; }
  addr = lowerCase(addr);
  if (!bleWakeRename(addr, name, err)) { sendErr(400, err); return; }
  sendOk();
}

// {"addr":"..","enabled":bool}: a disabled device never wakes the PC
static bool enableBody(std::string &addr, bool &on) {
  JsonDocument d;
  if (!parseBody(d)) return false;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  addr = strField(o, "addr", ok);
  if (!ok || !o["enabled"].is<bool>()) { sendErr(400, "addr must be a string and enabled true or false"); return false; }
  on = o["enabled"].as<bool>();
  return true;
}

static void hWakeEnable() {
  if (!requireAuth()) return;
  std::string addr, err;
  bool on;
  if (!enableBody(addr, on)) return;
  addr = lowerCase(addr);
  if (!bleWakeEnable(addr, on, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hBleScan() {
  if (!requireAuth()) return;
  bleStartDiscovery();
  sendOk();
}

static void hBleScanStop() {
  if (!requireAuth()) return;
  bleStopDiscovery();
  sendOk();
}

static void hBleDevices() {
  if (!requireAuth()) return;
  JsonDocument d;
  d["scanning"] = bleDiscoveryActive();
  d["left_s"] = bleDiscoveryLeftS();
  bleDevicesToJson(d["devices"].to<JsonArray>());
  sendJson(200, d);
}

// ---- 2.4G wake (rf.cpp) ----
static void hRfWakeGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  rfWakeToJson(d["list"].to<JsonArray>());
  sendJson(200, d);
}

static void hRfWakeAdd() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string addr = strField(o, "addr", ok), name = strField(o, "name", ok);
  if (!ok) { sendErr(400, "addr and name must be strings"); return; }
  // mhz: needed for a 2.4G device; a Bluetooth host has none
  if (!o["mhz"].isNull() && !o["mhz"].is<float>()) { sendErr(400, "mhz must be a number"); return; }
  std::string err;
  if (!rfWakeAdd(addr, o["mhz"].is<float>() ? o["mhz"].as<float>() : NAN, name, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hRfWakeRemove() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  std::string addr = strField(d.as<JsonObjectConst>(), "addr", ok), err;
  if (!ok) { sendErr(400, "addr must be a string"); return; }
  if (!rfWakeRemove(addr, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hRfWakeRename() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string addr = strField(o, "addr", ok), name = strField(o, "name", ok), err;
  if (!ok) { sendErr(400, "addr and name must be strings"); return; }
  if (!rfWakeRename(addr, name, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hRfWakeEnable() {
  if (!requireAuth()) return;
  std::string addr, err;
  bool on;
  if (!enableBody(addr, on)) return;
  if (!rfWakeEnable(addr, on, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hRfLearnGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  rfLearnToJson(d.to<JsonObject>());
  sendJson(200, d);
}

static void hRfLearnPost() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  if (!d["on"].is<bool>()) { sendErr(400, "on must be true or false"); return; }
  // kind: "bt" learns Bluetooth hosts; absent or "dongle", 2.4G devices
  std::string kind = d["kind"].is<const char *>() ? d["kind"].as<const char *>() : "dongle";
  if (kind != "bt" && kind != "dongle") { sendErr(400, "kind must be dongle or bt"); return; }
  rfLearn(d["on"].as<bool>(), kind == "bt");
  sendOk();
}

static void hRfSnapshot() {
  if (!requireAuth()) return;
  char q[24], v[8];
  int ch = 0;
  if (httpd_req_get_url_query_str(R, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "ch", v, sizeof v) == ESP_OK) ch = atoi(v);
  const uint8_t *buf;
  std::string err;
  int code = rfSnapBegin(ch, buf, err);
  if (code) { sendErr(code, err); return; }
  static char centre[16], chs[8];
  snprintf(centre, sizeof centre, "%ld", 2407000000L + 5000000L * ch);
  snprintf(chs, sizeof chs, "%d", ch);
  httpd_resp_set_type(R, "application/octet-stream");
  httpd_resp_set_hdr(R, "Cache-Control", "no-store");
  httpd_resp_set_hdr(R, "X-Centre-Hz", centre);
  httpd_resp_set_hdr(R, "X-Sample-Rate", "16000000");
  httpd_resp_set_hdr(R, "X-Format", "c3-raw-10bit");
  httpd_resp_set_hdr(R, "X-Channel", chs);
  for (size_t o = 0; o < RF_SNAP_BYTES; o += 4380) {
    if (httpd_resp_send_chunk(R, (const char *)buf + o, std::min<size_t>(4380, RF_SNAP_BYTES - o)) != ESP_OK) { dropConn = true; break; }
    rfSnapHold();
  }
  if (!dropConn) httpd_resp_send_chunk(R, nullptr, 0);
  rfSnapEnd();
}

static void hWifiScan() {
  if (!requireAuth()) return;
  wifiStartScan();
  sendOk();
}

static void hWifiNetworks() {
  if (!requireAuth()) return;
  JsonDocument d;
  d["scanning"] = wifiScanning();
  wifiNetworksToJson(d["networks"].to<JsonArray>());
  sendJson(200, d);
}

static void hWifiSave() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string ssid = strField(o, "ssid", ok), pass = strField(o, "pass", ok), err;
  if (!ok) { sendErr(400, "ssid and pass must be strings"); return; }
  if (!wifiSaveAndConnect(ssid, pass, err)) { sendErr(400, err); return; }
  sendOk();
}

static void hWifiForget() {
  if (!requireAuth()) return;
  sendOk();
  wifiForget();   // applied in wifiUpdate(), after this answer is out
}

static void hTime() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  JsonVariantConst e = d["epoch"];
  if (!e.is<double>() || e.as<double>() < 1704067200.0 || e.as<double>() > 4102444800.0) {
    sendErr(400, "epoch must be UTC seconds between 2024-01-01 and 2100-01-01");
    return;
  }
  timeSetManual((uint32_t)e.as<double>());   // ignored (still ok) when SNTP has synced
  sendOk();
}

static void hTimeSync() {
  if (!requireAuth()) return;
  std::string err;
  if (!timeSyncNow(err)) { sendErr(409, err); return; }
  sendOk();
}

static void hNetGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  wifiNetToJson(d.to<JsonObject>());
  sendJson(200, d);
}

static void hNetPost() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  JsonObjectConst o = d.as<JsonObjectConst>();
  if (!o["dhcp"].is<bool>()) { sendErr(400, "dhcp must be true or false"); return; }
  bool ok = true;
  std::string ip = strField(o, "ip", ok), mask = strField(o, "mask", ok), gw = strField(o, "gw", ok);
  std::string d1 = strField(o, "dns1", ok), d2 = strField(o, "dns2", ok), err;
  if (!ok) { sendErr(400, "addresses must be strings"); return; }
  if (!wifiNetSave(o["dhcp"].as<bool>(), ip, mask, gw, d1, d2, err)) { sendErr(400, err); return; }
  sendOk();   // the station reconnects from wifiUpdate() after this answer
}

static void scheduleRestart(uint32_t delayMs, uint32_t why) {
  rtcRestartWhy = WHY_MAGIC | why;
  restartPending = true;
  restartAt = nowMs() + delayMs;
  restartDeadline = restartAt + RESTART_SETTLE_MS;
}

static void hRestart() {
  if (!requireAuth()) return;
  std::string err;
  if (!powerLockForRestart(err)) { sendErr(409, err); return; }
  sendOk();
  scheduleRestart(500, WHY_WEB);
}

// Factory reset: erases the whole NVS partition (settings, password,
// WiFi, both wake lists, wake method) just before the restart, which goes
// through the same gate as /api/restart, so an `on` BC-250 is kept on with
// pson_hold. The controller comes back in setup mode (BC250-Setup).
static void hFactoryReset() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  if (!(d["confirm"] | false)) { sendErr(400, "confirm must be true"); return; }
  std::string err;
  if (!powerLockForRestart(err)) { sendErr(409, err); return; }
  logEvent("factory reset requested");
  sendOk();
  factoryPending = true;
  scheduleRestart(500, WHY_WEB);
}

// Choose the wake method; it applies after a restart. Both wake lists are
// kept (/api/wake/clear empties one).
static void hRadio() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  const char *r = d["radio"] | "";
  bool g4 = strcmp(r, "2g4") == 0;
  if (!g4 && strcmp(r, "ble") != 0) { sendErr(400, "radio must be ble or 2g4"); return; }
  if (g4 == radio2g4()) { sendOk(); return; }
  std::string err;
  if (!powerLockForRestart(err)) { sendErr(409, err); return; }
  if (!radioSave(g4)) { sendErr(500, "could not save"); return; }
  logEvent("wake method set to %s, restarting", g4 ? "2.4G/BT" : "BLE");
  sendOk();
  scheduleRestart(500, WHY_WEB);
}

// Both wake lists, whichever method runs. Each is stored in its own
// NVS namespace ("ble"; 2.4G/BT: "rf" and "bth"); the running method keeps its copy in sync.
static int savedWakeCount(const char *ns) {
  Nvs p;
  if (!p.open(ns, false)) return 0;
  int n = p.getU8("n", 0);
  return n > 8 ? 0 : n;
}

static void hWakeLists() {
  if (!requireAuth()) return;
  JsonDocument d;
  d["ble"] = savedWakeCount("ble");
  int dn, hn;
  rfSavedCounts(dn, hn);
  d["dongle"] = dn;   // 2.4G Dongle devices
  d["bt"] = hn;       // Bluetooth Classic hosts
  d["radio"] = radioName();
  sendJson(200, d);
}

// Empties one of the three wake lists ("ble", "dongle" or "bt"), whichever
// method runs.
static void hWakeClear() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  const char *l = d["list"] | "";
  if (strcmp(l, "dongle") == 0 || strcmp(l, "bt") == 0) rfWakeClearKind(l[0] == 'b');
  else if (strcmp(l, "ble") != 0) { sendErr(400, "list must be ble, dongle or bt"); return; }
  else if (!radio2g4()) bleWakeClear();
  else {
    Nvs p;
    if (p.open("ble", true)) p.clear();
    logEvent("BLE wake list cleared");
  }
  sendOk();
}

static void hDebug() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  std::string m = strField(d.as<JsonObjectConst>(), "sense", ok), err;
  if (!ok || !powerSetSenseMode(m, err)) { sendErr(400, ok ? err : std::string("sense must be a string")); return; }
  sendOk();
}

// ---- auth (section 12) ---------------------------------------------------

static void setSidCookie(const std::string &sid) {
  static std::string v;   // header value must outlive the send (single server task)
  v = "sid=" + sid + "; Path=/; HttpOnly; SameSite=Strict";
  httpd_resp_set_hdr(R, "Set-Cookie", v.c_str());
}

static void hAuthGet() {
  JsonDocument d;
  d["password_set"] = authPasswordSet();
  d["authed"] = authed();
  d["locked_s"] = authLockedSeconds();
  sendJson(200, d);
}

static void hLogin() {
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  std::string pw = strField(d.as<JsonObjectConst>(), "password", ok), sid;
  if (!ok) { sendErr(400, "password must be a string"); return; }
  int c = authLogin(pw, sid);
  if (c == 200) { setSidCookie(sid); sendOk(); }
  else if (c == 429) sendErr(429, "too many attempts, try again later");
  else sendErr(401, "wrong password");
}

static void hLogout() {
  if (!requireAuth()) return;
  authLogout(cookie());
  httpd_resp_set_hdr(R, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  sendOk();
}

static void hPassword() {
  // With a password set: needs a session (+ right `old`). With none: open.
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  bool ok = true;
  JsonObjectConst o = d.as<JsonObjectConst>();
  std::string oldPw = strField(o, "old", ok), newPw = strField(o, "new", ok), sid, err;
  if (!ok) { sendErr(400, "old and new must be strings"); return; }
  int c = authSetPassword(oldPw, newPw, cookie(), sid, err);
  if (c != 200) { sendErr(c, err); return; }
  setSidCookie(sid);
  sendOk();
}

// ---- OTA -----------------------------------------------------------------
//
// POST /api/update is multipart/form-data with one file field named
// "firmware" (the UI's FormData). The body is parsed as a stream: 4 KB heap
// buffer, bytes of the file go straight to esp_ota_write. Auth and power
// state are checked when the file part starts, before a byte reaches flash.
// After an error the rest of the body is read and dropped (so the browser
// gets the answer), as the Arduino server did; a lost connection aborts.

static const char *findBytes(const uint8_t *h, size_t hn, const char *n, size_t nn) {
  return (const char *)memmem(h, hn, n, nn);
}

// Value of `key="..."` in a header line (key preceded by space or ';').
static bool dispParam(const std::string &s, const char *key, std::string &out) {
  std::string k = std::string(key) + "=\"";
  size_t p = 0;
  while ((p = s.find(k, p)) != std::string::npos) {
    if (p == 0 || s[p - 1] == ' ' || s[p - 1] == ';') {
      size_t b = p + k.size(), e = s.find('"', b);
      if (e == std::string::npos) return false;
      out = s.substr(b, e - b);
      return true;
    }
    p += k.size();
  }
  return false;
}

static std::string multipartBoundary() {
  std::string ct = reqHeader("Content-Type");
  if (lowerCase(ct).find("multipart/form-data") == std::string::npos) return "";
  size_t p = lowerCase(ct).find("boundary=");
  if (p == std::string::npos) return "";
  std::string b = ct.substr(p + 9);
  if (!b.empty() && b[0] == '"') { size_t e = b.find('"', 1); b = b.substr(1, e == std::string::npos ? e : e - 1); }
  else { size_t e = b.find(';'); if (e != std::string::npos) b.resize(e); }
  if (b.size() > 70) return "";
  return b;
}

// Returns false when the connection is lost (no answer possible).
static bool runUpload(const std::string &boundary, int &status, std::string &error, bool &done) {
  enum { BODY, AFTER, HEAD, DONE } st = BODY;
  const size_t CAP = 4096;
  uint8_t *buf = (uint8_t *)malloc(CAP);
  if (!buf) { status = 500; error = "out of memory"; return true; }
  std::string delim = "\r\n--" + boundary;
  const size_t dl = delim.size();
  size_t have = 0, remaining = R->content_len;
  bool failed = false, inFile = false, fileSeen = false, locked = false, started = false, first = true;
  esp_ota_handle_t h = 0;
  bool connOk = true;

  auto abortOta = [&]() {
    if (started) { esp_ota_abort(h); started = false; }
    if (locked) { powerLock(false); locked = false; }
    otaRunning.store(false);
  };
  auto fail = [&](int code, const char *msg) {
    if (failed) return;
    failed = true; status = code; error = msg;
    abortOta();
    inFile = false;
  };
  auto consume = [&](size_t n) { memmove(buf, buf + n, have - n); have -= n; };

  // virtual CRLF in front: the first boundary line then matches like the others
  buf[0] = '\r'; buf[1] = '\n'; have = 2;
  int timeouts = 0;
  for (;;) {
    bool more = false;
    while (!more && st != DONE) {
      if (failed) { have = 0; more = true; break; }
      switch (st) {
        case BODY: {
          const char *p = findBytes(buf, have, delim.data(), dl);
          if (p) {
            size_t i = p - (const char *)buf;
            if (inFile && i && esp_ota_write(h, buf, i) != ESP_OK) { fail(500, "flash write failed"); break; }
            if (inFile) {
              inFile = false;
              esp_err_t e = esp_ota_end(h);
              started = false;   // the handle is gone either way
              if (e == ESP_OK) e = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(nullptr));
              if (e != ESP_OK) fail(500, "invalid firmware image");
              else { done = true; otaRunning.store(false); logEvent("update done"); }
              if (failed) break;
            }
            consume(i + dl);
            st = AFTER;
          } else {
            size_t n = have > dl - 1 ? have - (dl - 1) : 0;
            if (n) {
              if (inFile && esp_ota_write(h, buf, n) != ESP_OK) { fail(500, "flash write failed"); break; }
              consume(n);
            }
            more = true;
          }
          break;
        }
        case AFTER:
          if (have < 2) { more = true; break; }
          if (buf[0] == '-' && buf[1] == '-') st = DONE;
          else if (buf[0] == '\r' && buf[1] == '\n') { consume(2); st = HEAD; }
          else fail(400, "upload aborted");
          break;
        case HEAD: {
          const char *p = findBytes(buf, have, "\r\n\r\n", 4);
          if (!p) { if (have >= 1024) fail(400, "upload aborted"); else more = true; break; }
          size_t hl = p - (const char *)buf;
          std::string hdr((const char *)buf, hl);
          consume(hl + 4);
          st = BODY;
          // the disposition line
          std::string line = hdr, name, fname;
          size_t cd = lowerCase(hdr).find("content-disposition");
          if (cd != std::string::npos) { size_t e = hdr.find("\r\n", cd); line = hdr.substr(cd, e == std::string::npos ? e : e - cd); }
          dispParam(line, "name", name);
          dispParam(line, "filename", fname);
          if (fname.empty() || fileSeen) break;   // an ordinary form field: ignored
          fileSeen = true;
          if (!authed()) { fail(401, "login required"); break; }
          if (name != "firmware") { fail(400, "field must be named firmware"); break; }
          // No firmware update during an OS update job (section 26). A job
          // cannot start once the upload is streaming (webOtaRunning).
          if (osupdBusy()) { fail(409, "an OS update is running"); break; }
          // Atomic "is off, and from now on no power-on" until the update ends
          // (or the board reboots). The control task keeps running meanwhile.
          if (!powerLockForRestart(error)) { failed = true; status = 409; otaRunning.store(false); break; }
          locked = true;
          const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
          if (!next || esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) { fail(500, "update begin failed"); break; }
          started = true;
          inFile = true;
          logEvent("update started");
          break;
        }
        default: break;
      }
    }
    if (st == DONE) break;
    if (remaining == 0) {   // body ended inside the stream
      if (!failed) fail(400, "upload aborted");
      break;
    }
    size_t want = CAP - have;
    if (want > remaining) want = remaining;
    int n = httpd_req_recv(R, (char *)buf + have, want);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts >= 2) { connOk = false; break; }
      continue;
    }
    if (n <= 0) { connOk = false; break; }
    timeouts = 0;
    if (first) { first = false; otaRunning.store(true); markActivity(); }
    remaining -= n;
    have += n;
    markActivity();   // keeps the Bluetooth scan light for the whole upload
  }
  if (!connOk) abortOta();
  // read and drop whatever is left of the body (trailing CRLF, or all of it after an error)
  timeouts = 0;
  while (connOk && remaining > 0) {
    size_t want = remaining < CAP ? remaining : CAP;
    int n = httpd_req_recv(R, (char *)buf, want);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 2) continue;
    if (n <= 0) { connOk = false; break; }
    timeouts = 0;
    remaining -= n;
    markActivity();
  }
  abortOta();   // no-op after success: the lock stays until the restart
  free(buf);
  stackLogDue = true;
  return connOk;
}

// ---- Automatic OS updates (osupd.cpp, section 26) ----
static void hOsGet() {
  if (!requireAuth()) return;
  JsonDocument d;
  osupdToJson(d.to<JsonObject>());
  sendJson(200, d);
}

static void hOsPost() {
  if (!requireAuth()) return;
  JsonDocument d;
  if (!parseBody(d)) return;
  std::string err;
  int code = osupdConfigure(d.as<JsonObjectConst>(), err);
  if (code) { sendErr(code, err); return; }
  sendOk();
}

static void hOsRun() {
  if (!requireAuth()) return;
  std::string err;
  if (!osupdRun(err)) { sendErr(409, err); return; }
  sendOk();
}

static void hOsTest() {
  if (!requireAuth()) return;
  std::string err;
  if (!osupdTest(err)) { sendErr(409, err); return; }
  sendOk();
}

static void hOsStop() {
  if (!requireAuth()) return;
  osupdStop();
  sendOk();
}

static void hOsForgetHost() {
  if (!requireAuth()) return;
  std::string err;
  if (!osupdForgetHost(err)) { sendErr(409, err); return; }
  sendOk();
}

static void sendText(int code, const char *p, size_t n) {
  httpd_resp_set_status(R, statusText(code));
  httpd_resp_set_type(R, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(R, "Cache-Control", "no-store");
  httpd_resp_send(R, p, n);
}

// The PC fetches the scripts with curl: no login, but only while the feature is on.
static void hScriptUpdate() {
  markActivity();
  if (!osupdEnabled()) { sendText(404, "not found", 9); return; }
  sendText(200, update_sh_start, update_sh_end - update_sh_start - 1);
}

static void hScriptSetup() {
  markActivity();
  std::string ctl = osupdControllerAddress(), key, fp;
  if (!osupdEnabled()) { sendText(404, "not found", 9); return; }
  if (ctl.empty() || !sshKeyInfo(key, fp)) { sendText(409, "not ready", 9); return; }
  std::string t(setup_sh_start, setup_sh_end - setup_sh_start - 1);
  for (int pass = 0; pass < 2; pass++) {
    const char *tag = pass ? "@KEY@" : "@CTL@";
    const std::string &val = pass ? key : ctl;
    for (size_t i = t.find(tag); i != std::string::npos; i = t.find(tag, i + val.size())) t.replace(i, 5, val);
  }
  sendText(200, t.data(), t.size());
}

static void hUpdate() {
  int status = 0;
  std::string error;
  bool ok = false;
  std::string b = multipartBoundary();
  if (!b.empty() && R->content_len > 0) {
    if (!runUpload(b, status, error, ok)) { dropConn = true; return; }
  }
  if (ok) {
    sendOk();
    scheduleRestart(800, WHY_UPDATE);
    return;
  }
  if (!status) {
    // No file part: auth first, then state, then "no file".
    if (!authed()) { sendErr(401, "login required"); return; }
    std::string err;
    if (!powerRestartPossible(err)) { sendErr(409, err); return; }
    sendErr(400, "no firmware file");
    return;
  }
  sendErr(status, error);
}

// ---- setup ---------------------------------------------------------------

typedef void (*Handler)();
struct Route { const char *uri; httpd_method_t method; Handler fn; };

// Every handler runs under netLock (like the main task's housekeeping).
static esp_err_t trampoline(httpd_req_t *req) {
  NetGuard g;
  R = req;
  dropConn = false;
  ((Handler)req->user_ctx)();
  uint32_t hw = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  if (hw < webStackMin) webStackMin = hw;
  R = nullptr;
  return dropConn ? ESP_FAIL : ESP_OK;
}

static esp_err_t i18nTrampoline(httpd_req_t *req) {
  NetGuard g;
  R = req;
  dropConn = false;
  sendI18n(*(const I18nFile *)req->user_ctx);
  R = nullptr;
  return ESP_OK;
}

// Unknown path or wrong method: JSON/plain 404 or the captive-portal redirect.
static esp_err_t errTrampoline(httpd_req_t *req, httpd_err_code_t) {
  NetGuard g;
  R = req;
  dropConn = false;
  notFoundOrCaptive();
  R = nullptr;
  return ESP_OK;
}

static const Route routes[] = {
  {"/", HTTP_GET, hRoot},
  {"/setup", HTTP_GET, hSetup},
  {"/api/state", HTTP_GET, hState},
  {"/api/power", HTTP_POST, hPower},
  {"/api/config", HTTP_GET, hConfigGet},
  {"/api/config", HTTP_POST, hConfigPost},
  {"/api/config/reset", HTTP_POST, hConfigReset},
  {"/api/log", HTTP_GET, hLog},
  {"/api/info", HTTP_GET, hInfo},
  {"/api/ble/wake", HTTP_GET, hWakeGet},
  {"/api/ble/wake/add", HTTP_POST, hWakeAdd},
  {"/api/ble/wake/remove", HTTP_POST, hWakeRemove},
  {"/api/ble/wake/rename", HTTP_POST, hWakeRename},
  {"/api/ble/wake/enable", HTTP_POST, hWakeEnable},
  {"/api/ble/scan", HTTP_POST, hBleScan},
  {"/api/ble/scan/stop", HTTP_POST, hBleScanStop},
  {"/api/ble/devices", HTTP_GET, hBleDevices},
  {"/api/radio", HTTP_POST, hRadio},
  {"/api/wake/lists", HTTP_GET, hWakeLists},
  {"/api/wake/clear", HTTP_POST, hWakeClear},
  {"/api/wifi/scan", HTTP_POST, hWifiScan},
  {"/api/wifi/networks", HTTP_GET, hWifiNetworks},
  {"/api/wifi", HTTP_POST, hWifiSave},
  {"/api/wifi/forget", HTTP_POST, hWifiForget},
  {"/api/update", HTTP_POST, hUpdate},
  {"/api/restart", HTTP_POST, hRestart},
  {"/api/factory_reset", HTTP_POST, hFactoryReset},
  {"/api/time", HTTP_POST, hTime},
  {"/api/time/sync", HTTP_POST, hTimeSync},
  {"/api/net", HTTP_GET, hNetGet},
  {"/api/net", HTTP_POST, hNetPost},
  {"/api/debug", HTTP_POST, hDebug},
  {"/api/auth", HTTP_GET, hAuthGet},
  {"/api/login", HTTP_POST, hLogin},
  {"/api/logout", HTTP_POST, hLogout},
  {"/api/auth/password", HTTP_POST, hPassword},
  {"/api/osupdate", HTTP_GET, hOsGet},
  {"/api/osupdate", HTTP_POST, hOsPost},
  {"/api/osupdate/run", HTTP_POST, hOsRun},
  {"/api/osupdate/test", HTTP_POST, hOsTest},
  {"/api/osupdate/stop", HTTP_POST, hOsStop},
  {"/api/osupdate/forget_host", HTTP_POST, hOsForgetHost},
  {"/bc250-setup.sh", HTTP_GET, hScriptSetup},
  {"/bc250-update.sh", HTTP_GET, hScriptUpdate},
};
// only when the 2.4G wake method runs (as before)
static const Route rfRoutes[] = {
  {"/api/rf/wake", HTTP_GET, hRfWakeGet},
  {"/api/rf/wake/add", HTTP_POST, hRfWakeAdd},
  {"/api/rf/wake/remove", HTTP_POST, hRfWakeRemove},
  {"/api/rf/wake/rename", HTTP_POST, hRfWakeRename},
  {"/api/rf/wake/enable", HTTP_POST, hRfWakeEnable},
  {"/api/rf/learn", HTTP_GET, hRfLearnGet},
  {"/api/rf/learn", HTTP_POST, hRfLearnPost},
  {"/api/rf/snapshot", HTTP_GET, hRfSnapshot},
};

static void addRoutes(const Route *rs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    httpd_uri_t u = {};
    u.uri = rs[i].uri;
    u.method = rs[i].method;
    u.handler = trampoline;
    u.user_ctx = (void *)rs[i].fn;
    httpd_register_uri_handler(httpdH, &u);
  }
}

void webInit() {
  // Only trust the RTC word after a software restart; then clear it so a
  // later crash or power cycle is not mislabelled.
  if (esp_reset_reason() == ESP_RST_SW && (rtcRestartWhy & 0xFFFF0000) == WHY_MAGIC) bootWhy = rtcRestartWhy & 0xFFFF;
  rtcRestartWhy = 0;

  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 6144;
  c.task_priority = 3;            // below the control task (5), above BLE/rf (2) and main (1)
  c.max_open_sockets = 4;         // LWIP_MAX_SOCKETS is 10; the server takes this + 3
  c.lru_purge_enable = true;      // a fifth browser connection evicts the oldest idle one
  c.max_uri_handlers = 72;       // routes plus one per translation catalog
  c.recv_wait_timeout = 10;       // s; an upload over slow WiFi may stall a while
  c.send_wait_timeout = 10;
  if (httpd_start(&httpdH, &c) != ESP_OK) { logEvent("web server did not start"); return; }
  addRoutes(routes, sizeof(routes) / sizeof(routes[0]));
  static std::string i18nUris[16];   // httpd keeps the pointers
  for (size_t i = 0; i < I18N_FILE_COUNT && i < 16; i++) {
    i18nUris[i] = std::string("/i18n/") + I18N_FILES[i].name + ".json";
    httpd_uri_t u = {};
    u.uri = i18nUris[i].c_str();
    u.method = HTTP_GET;
    u.handler = i18nTrampoline;
    u.user_ctx = (void *)&I18N_FILES[i];
    httpd_register_uri_handler(httpdH, &u);
  }
  if (radio2g4()) addRoutes(rfRoutes, sizeof(rfRoutes) / sizeof(rfRoutes[0]));
  // Unknown path / wrong method (OS connectivity probes too) -> 404 or wizard.
  httpd_register_err_handler(httpdH, HTTPD_404_NOT_FOUND, errTrampoline);
  httpd_register_err_handler(httpdH, HTTPD_405_METHOD_NOT_ALLOWED, errTrampoline);
}

void webUpdate() {
  static bool logged = false;
  if ((stackLogDue || (!logged && nowMs() > 150000)) && webStackMin != 0xFFFFFFFF) {
    stackLogDue = false;
    logged = true;
    logEvent("web stack free: %u", (unsigned)webStackMin);
  }
  if (restartPending && (int32_t)(nowMs() - restartAt) >= 0) {
    // Decision and PSON latch are one atomic step inside the power module.
    RestartMode m = powerPrepareRestart();
    if (m == RESTART_HELD || m == RESTART_PLAIN) {
      if (factoryPending) {
        esp_wifi_stop();      // the driver keeps its own NVS handle
        nvs_flash_erase();    // de-initialises NVS first
      }
      esp_restart();   // does not return
    } else if (m == RESTART_WAIT && (int32_t)(restartDeadline - nowMs()) > 0) {
      restartAt = nowMs() + 200;   // starting / shutting_down: wait for it to settle
    } else {
      // Never restart unlatched while the PC is on: cancel instead.
      restartPending = false;
      factoryPending = false;
      powerLock(false);
      logEvent("restart cancelled: power state did not allow it");
    }
  }
}
