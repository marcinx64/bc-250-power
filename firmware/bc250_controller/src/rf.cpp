// SPDX-License-Identifier: GPL-3.0-or-later
// 2.4G dongle-mode wake, and classic Bluetooth controllers paging their
// host: the core. Radio, capture, channel sweep, wake lists in NVS, learning
// and the wake rules live here. Everything about packet formats is in the
// protocol modules (rf_proto.h, proto_*.cpp), the signal processing in
// rf_dsp.cpp; both compile on the PC too (host_test/).
// See rf.h, docs/WAKE_2G4.md, docs/WAKE_BT_CLASSIC.md, docs/ADDING_PROTOCOLS.md.
//
// The capture engine registers are undocumented (found by the esp-sdr
// project, GPL-3). The receiver is never tuned or calibrated here: the WiFi
// driver tunes it for each channel visit, so WiFi keeps owning the radio.
//
// Threads: the rf task (captures, detection, learning, arming, wake) and the
// web handlers (httpd task). Shared state is under rfMx, held only for short
// copies; never across a capture, a detection or a flash write.
#include "rf.h"
#include "rf_proto.h"
#include "ble.h"
#include "power.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "eventlog.h"
#include "quietmem.h"
#include "util.h"
#include "web.h"
#include <string>
#include <string.h>
#include <math.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <atomic>
#include "heap_memory_layout.h"
#include "soc/soc.h"

// The engine writes to 0x3fcb0000 (64 KiB); the whole 128 KiB bank is kept
// away from the heap, as esp-sdr does. tools/ram_guard.py checks after linking
// that static RAM ends below it.
SOC_RESERVE_MEMORY_REGION(0x3fca0000, 0x3fcc0000, rf_capture_bank);
#define IQ_WORDS rfdsp::IQ_WORDS
#define IQ_BUFFER ((volatile uint32_t *)0x3fcb0000)
#define SRAM_OWNER_REG 0x600c1020u
#define DUMP_CTRL 0x60033d5cu
#define DUMP_LANES 0x60033d90u

// Two lists: up to DEV_MAX 2.4G Dongle devices (NVS "rf") and HOST_MAX
// Bluetooth Classic hosts (NVS "bth"). One host covers every controller
// paired with it, so two are plenty. At run time both are held in one array
// of entries.
static const int DEV_MAX = 8, HOST_MAX = 2, WAKE_MAX = DEV_MAX + HOST_MAX;
static const char *NS_DEV = "rf", *NS_HOST = "bth";
static const uint32_t PERIOD_MS = 1000;     // task cycle
// Channel visits: one 1 ms snapshot each. About 23 a second when watching
// keeps WiFi ~90 % of the time on its own channel, and only while the PC is
// off; learning ~30/s. Measured with the Rii searching: 0.93 catches/s
// against 0.51 at half the rate, first catch median 0.7 s against 1.2 s.
// Visiting every third channel instead of each one did not catch more per
// snapshot, so every channel is visited.
static const uint32_t GAP_WATCH_MS = 25, GAP_LEARN_MS = 15;
static const uint32_t PRESENT_MS = 30000;   // "in range" after a sighting
// A wake needs one exact hit (CRC-valid frame, error-free access code), or
// two approximate ones (listed key with a few wrong bits; each protocol has
// its error budget, rf_proto.h) within CONFIRM_MS: noise alone then
// practically never wakes (docs/ADDING_PROTOCOLS.md).
static const uint32_t CONFIRM_MS = 5000;
// Arming as Bluetooth: a listed device arms after ble_rearm_s unheard,
// counted from the PC turning off, from the start, or from its last
// sighting. A searching controller is caught only now and then (gaps up to
// 17 s measured), so a Re-arm time shorter than that can arm it while it
// still searches. Learn runs until Stop or for the scan_s setting.

static const uint32_t RF_STACK = 6144;
static SemaphoreHandle_t rfMx;
#define RF_LOCK() xSemaphoreTake(rfMx, portMAX_DELAY)
#define RF_UNLOCK() xSemaphoreGive(rfMx)

// ------------------------------------------------------------ wake list
// As stored in NVS: a 2.4G Dongle device ("rf") and a Bluetooth Classic host
// ("bth"). Each namespace: n (count), list (records), off (bit k: the k-th
// entry is disabled; absent = all enabled), proto (the protocol ids of the
// entries, comma separated, in the order of the records; absent or empty =
// the list's first protocol, as stored by firmware without it).
struct Rec { uint8_t addr[5]; uint32_t khz; char name[32]; };
struct HostRec { uint8_t lap[3]; char name[32]; };
// At run time a host is an entry with its LAP in r.addr[0..2] (rest 0).
struct Entry {
  bool used;
  bool bt;              // Bluetooth host list (key = LAP), else the 2.4G dongle list
  Rec r;
  int8_t pi;            // registry index of its protocol; -1: stored by a firmware that has one this build lacks (kept, never matched)
  char pid[8];          // the protocol id as stored
  uint64_t pat;         // pattern of the key (rfp::Proto::pattern)
  bool seen, armed;
  uint32_t lastSeen, armedAt;
  bool off;             // disabled: still heard and shown, never wakes the PC
  bool pending;         // armed and matched once approximately: one more within CONFIRM_MS wakes
  uint32_t pendingAt;
};
static Entry *list;     // WAKE_MAX entries, heap (rfInit)
static uint32_t offSince = 0;
static PowerState lastState = PWR_OFF;

static std::string keyHex(const uint8_t *a, int len) {
  char s[11];
  for (int i = 0; i < len && i < 5; i++) snprintf(s + 2 * i, sizeof(s) - 2 * i, "%02X", a[i]);
  return std::string(s);
}
static std::string entryHex(const uint8_t *a, bool bt) { return keyHex(a, bt ? 3 : 5); }
static std::string entryHex(const Entry &e) {
  return keyHex(e.r.addr, e.bt ? 3 : (e.pi >= 0 ? rfp::at(e.pi)->keyLen : 5));
}
static bool parseAddr(const std::string &h, uint8_t *a) {
  if (h.length() != 10) return false;
  for (int i = 0; i < 10; i++) if (!isxdigit((unsigned char)h[i])) return false;
  for (int i = 0; i < 5; i++) a[i] = (uint8_t)strtoul(h.substr(2 * i, 2).c_str(), nullptr, 16);
  return true;
}

// Address text: 10 hex digits = a 2.4G device; 6 hex digits (a LAP) or a
// Bluetooth address (12 hex digits; ':', '-' and spaces allowed, the LAP is
// its lower 3 bytes) = a Bluetooth host.
static bool parseAny(const std::string &in, uint8_t *a, bool &bt) {
  std::string h;
  for (char c : in) if (c != ':' && c != '-' && c != ' ') h += c;
  h = upperCase(h);
  for (char c : h) if (!isxdigit((unsigned char)c)) return false;
  memset(a, 0, 5);
  if (h.size() == 10) { bt = false; return parseAddr(h, a); }
  if (h.size() == 12) h = h.substr(6);
  if (h.size() != 6) return false;
  bt = true;
  for (int i = 0; i < 3; i++) a[i] = (uint8_t)strtoul(h.substr(2 * i, 2).c_str(), nullptr, 16);
  return true;
}
static const char *BAD_ADDR = "address must be 10 hex digits (2.4G device) or a Bluetooth address";
// The used entry with this address, or -1. Call with the rf lock held.
static int findEntry(const uint8_t *a, bool bt) {
  for (int i = 0; i < WAKE_MAX; i++)
    if (list[i].used && list[i].bt == bt && memcmp(list[i].r.addr, a, bt ? 3 : 5) == 0) return i;
  return -1;
}
static void setProto(Entry &e, const char *id) {
  strlcpy(e.pid, id, sizeof(e.pid));
  e.pi = (int8_t)rfp::find(e.pid);
}
static void setKeys(Entry &e) {
  e.pat = e.pi >= 0 ? rfp::at(e.pi)->pattern(e.r.addr) : 0;
}

// The allowed WiFi channels (country setting; 12/13 only where permitted).
static void channelRange(int &lo, int &hi) {
  wifi_country_t c;
  lo = 1; hi = 11;
  if (esp_wifi_get_country(&c) == ESP_OK && c.nchan) { lo = c.schan; hi = c.schan + c.nchan - 1; }
  if (hi > 13) hi = 13;
}
static int chCentreKhz(int ch) { return 2407000 + 5000 * ch; }

// Writes both lists. Call without the rf lock.
static void saveList() {
  Rec dev[DEV_MAX];
  HostRec host[HOST_MAX];
  uint8_t nd = 0, nh = 0;
  uint32_t offD = 0, offH = 0;
  std::string protoD, protoH;
  RF_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    const Entry &e = list[i];
    if (!e.used) continue;
    if (e.bt && nh < HOST_MAX) {
      if (e.off) offH |= 1u << nh;
      memcpy(host[nh].lap, e.r.addr, 3);
      memcpy(host[nh].name, e.r.name, sizeof(host[nh].name));
      if (nh) protoH += ',';
      protoH += e.pid;
      nh++;
    } else if (!e.bt && nd < DEV_MAX) {
      if (e.off) offD |= 1u << nd;
      if (nd) protoD += ',';
      protoD += e.pid;
      dev[nd++] = e.r;
    }
  }
  RF_UNLOCK();
  Nvs p;   // flash writes: outside the lock
  p.open(NS_DEV, true);
  p.setU8("n", nd);
  p.setBlob("list", dev, sizeof(Rec) * nd);
  p.setU32("off", offD);
  p.setStr("proto", protoD);
  Nvs q;
  q.open(NS_HOST, true);
  q.setU8("n", nh);
  q.setBlob("list", host, sizeof(HostRec) * nh);
  q.setU32("off", offH);
  q.setStr("proto", protoH);
}

static int storedCount(const char *ns, int max) {
  Nvs p;
  if (!p.open(ns, false)) return 0;
  int n = p.getU8("n", 0);
  return n > max ? 0 : n;
}

// The k-th id of a comma separated list; the list's default protocol when
// there is none.
static void nthProto(const std::string &s, int k, bool host, char *out) {
  size_t from = 0;
  for (int i = 0; i < k && from != std::string::npos; i++) {
    from = s.find(',', from);
    if (from != std::string::npos) from++;
  }
  out[0] = 0;
  if (from != std::string::npos && from < s.size()) {
    size_t to = s.find(',', from);
    std::string id = s.substr(from, to == std::string::npos ? std::string::npos : to - from);
    strlcpy(out, id.c_str(), 8);
  }
  if (!out[0]) strlcpy(out, rfp::at(rfp::defaultFor(host))->id, 8);
}

static void loadList() {
  Rec dev[DEV_MAX];
  HostRec host[HOST_MAX];
  Nvs p;
  p.open(NS_DEV, false);
  int nd = p.getU8("n", 0);
  if (nd > DEV_MAX || (nd && p.getBlob("list", dev, sizeof(Rec) * nd) != sizeof(Rec) * nd)) nd = 0;
  uint32_t offD = p.getU32("off", 0);
  std::string protoD = p.getStr("proto");
  p.close();
  Nvs q;
  q.open(NS_HOST, false);
  int nh = q.getU8("n", 0);
  if (nh > HOST_MAX || (nh && q.getBlob("list", host, sizeof(HostRec) * nh) != sizeof(HostRec) * nh)) nh = 0;
  uint32_t offH = q.getU32("off", 0);
  std::string protoH = q.getStr("proto");
  q.close();
  RF_LOCK();
  for (int i = 0; i < nd + nh; i++) {
    Entry &e = list[i];
    e = Entry();
    e.used = true;
    if (i < nd) {
      e.r = dev[i];
      e.off = offD >> i & 1;
      char id[8];
      nthProto(protoD, i, false, id);
      setProto(e, id);
    } else {
      const HostRec &h = host[i - nd];
      e.bt = true;
      memcpy(e.r.addr, h.lap, 3);
      memcpy(e.r.name, h.name, sizeof(e.r.name));
      e.off = offH >> (i - nd) & 1;
      char id[8];
      nthProto(protoH, i - nd, true, id);
      setProto(e, id);
    }
    e.r.name[31] = 0;
    setKeys(e);
  }
  RF_UNLOCK();
}

// Another used entry (not `except`) already has this name, ignoring case.
// Call with the rf lock held.
static bool nameTaken(const char *name, int except) {
  for (int i = 0; i < WAKE_MAX; i++)
    if (i != except && list[i].used && strcasecmp(list[i].r.name, name) == 0) return true;
  return false;
}

// ---------------------------------------------------- learning (any address)
// Candidates found while learning, with the frequencies their packets were
// heard on (for information: the controllers hop, detection does not use it).
struct LFreq { int64_t khzSum; uint16_t n; uint32_t first, last; };
struct Cand { uint8_t addr[5]; bool bt; int8_t pi; uint16_t packets; uint8_t nf; LFreq f[6]; };   // bt: a Bluetooth host (LAP), no frequencies
static const int CAND_MAX = 6;
static Cand *cand;      // heap (rfInit)
static int ncand = 0;
static bool learnOn = false;
// While Learn runs every entry is disarmed: the user is switching devices on
// to find them. When it ends, arming counts from then, as from a shutdown.
static uint32_t learnEndAt = 0;
static bool learnEnded = false;
static bool learnBt = false;   // Learn collects Bluetooth hosts instead of 2.4G devices
static uint32_t learnUntil = 0;
static int learnCh = 0;
static const uint16_t REAL_PACKETS = 3;   // a frequency counts from this many packets

bool rfWakeAdd(const std::string &hex, float mhz, const std::string &name, std::string &err, const std::string &proto) {
  uint8_t a[5];
  bool bt;
  if (!parseAny(hex, a, bt)) { err = BAD_ADDR; return false; }
  int pi = -1;
  if (!proto.empty()) {
    pi = rfp::find(proto.c_str());
    if (pi < 0) { err = "unknown protocol"; return false; }
    if (rfp::at(pi)->host != bt) { err = "address does not fit that protocol"; return false; }
  }
  // a Bluetooth host has no channel (it hops): mhz is ignored
  if (!bt && !(mhz >= 2400.0f && mhz <= 2483.5f)) { err = "frequency must be 2400-2483.5 MHz"; return false; }
  if (name.size() == 0 || name.size() > 31) { err = "name must be 1-31 characters"; return false; }
  uint32_t khz = bt ? 0 : (uint32_t)lrintf(mhz * 1000.0f);
  int slot = -1;
  RF_LOCK();
  int same = 0;
  for (int i = 0; i < WAKE_MAX; i++) if (list[i].used && list[i].bt == bt) same++;
  if (pi < 0) {   // none named: the protocol Learn found this address with, else the list's first
    for (int i = 0; i < ncand; i++) if (cand[i].bt == bt && memcmp(cand[i].addr, a, bt ? 3 : 5) == 0) pi = cand[i].pi;
    if (pi < 0) pi = rfp::defaultFor(bt);
  }
  if (findEntry(a, bt) >= 0) slot = -2;
  else if (nameTaken(name.c_str(), -1)) slot = -3;
  else if (same < (bt ? HOST_MAX : DEV_MAX)) for (int i = 0; i < WAKE_MAX; i++) if (!list[i].used) { slot = i; break; }
  if (slot >= 0) {
    Entry &e = list[slot];
    e = Entry();
    e.used = true;
    e.bt = bt;
    memcpy(e.r.addr, a, 5);
    e.r.khz = khz;
    strlcpy(e.r.name, name.c_str(), sizeof(e.r.name));
    setProto(e, rfp::at(pi)->id);
    setKeys(e);
    // Counts as just heard: it is usually still on (just learnt), and must
    // be quiet for the re-arm time before it can start the PC.
    e.seen = true;
    e.lastSeen = nowMs();
  }
  RF_UNLOCK();
  if (slot == -2) { err = "already in the list"; return false; }
  if (slot == -3) { err = "that name is already used"; return false; }
  if (slot < 0) { err = "list full"; return false; }
  saveList();
  if (bt) logEvent("2.4G/BT wake: added %.20s (Bluetooth Classic host %s)", name.c_str(), entryHex(a, true).c_str());
  else logEvent("2.4G/BT wake: added %.20s (%.1f MHz)", name.c_str(), mhz);
  return true;
}

bool rfWakeRemove(const std::string &hex, std::string &err) {
  uint8_t a[5];
  bool bt;
  if (!parseAny(hex, a, bt)) { err = BAD_ADDR; return false; }
  RF_LOCK();
  int i = findEntry(a, bt);
  if (i >= 0) list[i].used = false;
  RF_UNLOCK();
  if (i < 0) { err = "not in the list"; return false; }
  saveList();
  logEvent("2.4G/BT wake: removed %s", entryHex(a, bt).c_str());
  return true;
}

// Stored entries of each list, whether or not 2.4G/BT runs.
void rfSavedCounts(int &dongles, int &hosts) {
  dongles = storedCount(NS_DEV, DEV_MAX);
  hosts = storedCount(NS_HOST, HOST_MAX);
}

// Empties one list: the running one, or, with BLE running, the stored one.
void rfWakeClearKind(bool bt) {
  if (list) {
    RF_LOCK();
    for (int i = 0; i < WAKE_MAX; i++) if (list[i].used && list[i].bt == bt) list[i].used = false;
    RF_UNLOCK();
    saveList();
  } else {
    Nvs p;
    if (p.open(bt ? NS_HOST : NS_DEV, true)) p.clear();
  }
  logEvent(bt ? "2.4G/BT: Bluetooth Classic hosts wake list cleared" : "2.4G/BT: 2.4G Dongle devices wake list cleared");
}

bool rfWakeRename(const std::string &hex, const std::string &name, std::string &err) {
  uint8_t a[5];
  bool bt;
  if (!parseAny(hex, a, bt)) { err = BAD_ADDR; return false; }
  if (name.size() == 0 || name.size() > 31) { err = "name must be 1-31 characters"; return false; }
  bool taken = false;
  RF_LOCK();
  int i = findEntry(a, bt);
  if (i >= 0) {
    if (nameTaken(name.c_str(), i)) taken = true;
    else strlcpy(list[i].r.name, name.c_str(), sizeof(list[i].r.name));
  }
  RF_UNLOCK();
  if (i < 0) { err = "not in the list"; return false; }
  if (taken) { err = "that name is already used"; return false; }
  saveList();
  logEvent("2.4G/BT wake: renamed %s to %.20s", entryHex(a, bt).c_str(), name.c_str());
  return true;
}

bool rfWakeEnable(const std::string &hex, bool on, std::string &err) {
  uint8_t a[5];
  bool bt;
  if (!parseAny(hex, a, bt)) { err = BAD_ADDR; return false; }
  char name[32] = {0};
  RF_LOCK();
  int i = findEntry(a, bt);
  if (i >= 0) { list[i].off = !on; memcpy(name, list[i].r.name, 32); }
  RF_UNLOCK();
  if (i < 0) { err = "not in the list"; return false; }
  saveList();
  logEvent("2.4G/BT wake: %s %.20s", on ? "enabled" : "disabled", name);
  return true;
}

void rfWakeToJson(JsonArray out) {
  Entry *copy = (Entry *)malloc(sizeof(Entry) * WAKE_MAX);   // ~1.5 KB: heap, not the web server's stack
  if (!copy) return;
  RF_LOCK();
  memcpy(copy, list, sizeof(Entry) * WAKE_MAX);
  RF_UNLOCK();
  uint32_t now = nowMs();
  for (int i = 0; i < WAKE_MAX; i++) {
    const Entry &e = copy[i];
    if (!e.used) continue;
    JsonObject o = out.add<JsonObject>();
    o["addr"] = entryHex(e);
    o["kind"] = e.bt ? "bt" : "dongle";
    o["proto"] = std::string(e.pid);
    o["name"] = cleanText(e.r.name, 31);
    if (!e.bt) o["mhz"] = roundf(e.r.khz / 100.0f) / 10.0f;
    o["present"] = e.seen && cfg.ble_wake && (int32_t)(now - e.lastSeen) < (int32_t)PRESENT_MS;   // as Bluetooth
    o["armed"] = e.armed;
    o["enabled"] = !e.off;
    o["last_s"] = e.seen ? (int32_t)((now - e.lastSeen) / 1000) : -1;
  }
  free(copy);
}

int rfHostCount() {
  int n = 0;
  RF_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) if (list[i].used && list[i].bt) n++;
  RF_UNLOCK();
  return n;
}

void rfWakeCounts(int &count, int &present, int &armed) {
  count = present = armed = 0;
  uint32_t now = nowMs();
  RF_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    if (!list[i].used) continue;
    count++;
    if (list[i].seen && cfg.ble_wake && (int32_t)(now - list[i].lastSeen) < (int32_t)PRESENT_MS) present++;
    if (list[i].armed && !list[i].off) armed++;
  }
  RF_UNLOCK();
}

// ------------------------------------------------------- channel visits
static SemaphoreHandle_t rocDone;
static void rocCb(uint32_t, uint8_t, wifi_roc_done_status_t) { xSemaphoreGive(rocDone); }
static int rocRx(uint8_t *, uint8_t *, size_t, uint8_t) { return 0; }

static bool rocBegin(int ch, uint32_t ms) {
  xSemaphoreTake(rocDone, 0);
  wifi_roc_req_t r = {};
  r.ifx = WIFI_IF_STA;
  r.type = WIFI_ROC_REQ;
  r.channel = ch;
  r.sec_channel = WIFI_SECOND_CHAN_NONE;
  r.wait_time_ms = ms;
  r.rx_cb = rocRx;
  r.done_cb = rocCb;
  if (esp_wifi_remain_on_channel(&r) != ESP_OK) return false;
  vTaskDelay(pdMS_TO_TICKS(4));   // let the driver retune
  return true;
}
static void rocEnd() {
  wifi_roc_req_t r = {};
  r.ifx = WIFI_IF_STA;
  r.type = WIFI_ROC_CANCEL;
  esp_wifi_remain_on_channel(&r);
  xSemaphoreTake(rocDone, pdMS_TO_TICKS(200));
}

// --------------------------------------------------------------- capture
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// One snapshot of IQ_WORDS samples at 16 MS/s (~1 ms). Returns false on failure.
static bool capture() {
  for (unsigned j = 0; j < IQ_WORDS; j++) IQ_BUFFER[j] = 0xa5a0055au;
  taskENTER_CRITICAL(&mux);
  uint32_t owner = REG_READ(SRAM_OWNER_REG);
  int64_t t0 = esp_timer_get_time();
  REG_WRITE(DUMP_CTRL, 0);
  REG_WRITE(DUMP_LANES, (1u << 6) | (2u << 12) | (3u << 18));
  REG_WRITE(SRAM_OWNER_REG, (owner & ~7u) | 2u | 8u);
  __asm__ volatile("fence rw,rw" ::: "memory");
  uint32_t ctrl = 0x80000000u | IQ_WORDS | (1u << 16);   // /5: 16 MS/s
  REG_WRITE(DUMP_CTRL, ctrl);
  REG_WRITE(DUMP_CTRL, ctrl | (1u << 19));
  REG_WRITE(DUMP_CTRL, ctrl);
  while (!(REG_READ(DUMP_CTRL) & (1u << 18)) && esp_timer_get_time() - t0 < 20000) {}
  bool done = REG_READ(DUMP_CTRL) & (1u << 18);
  REG_WRITE(DUMP_CTRL, 0);
  REG_WRITE(SRAM_OWNER_REG, owner);
  __asm__ volatile("fence rw,rw" ::: "memory");
  taskEXIT_CRITICAL(&mux);
  if (!done) return false;
  for (unsigned j = 0; j < IQ_WORDS; j++)
    if (IQ_BUFFER[j] == 0xa5a0055au) return false;
  return true;
}

// Visit `ch`, take one snapshot, come back. Analysis happens after, on the
// home channel.
static uint32_t rocFailUntil = 0;   // rf task only: after a refused visit the radio is left alone for a while
static bool rocBackoff() {
  if (!rocFailUntil) return false;
  if ((int32_t)(nowMs() - rocFailUntil) < 0) return true;
  rocFailUntil = 0;   // expired (also keeps the wrap-around comparison valid)
  return false;
}

static bool visit(int ch) {
  // The driver refuses a visit while the station connects (a roam); the
  // sweep then waits 2 s instead of asking every 25 ms.
  if (!rocBegin(ch, 20)) { rocFailUntil = nowMs() + 2000; if (!rocFailUntil) rocFailUntil = 1; return false; }
  bool ok = capture();
  rocEnd();
  return ok;
}

// ---------------------------------------------------------- detection
// Working buffers in quiet memory (rfInit): only this task uses them, and
// only after the capture's critical section has ended.
static rfdsp::Work work;

static void learnRecord(const uint8_t *a, int khz, bool bt, int pi) {
  uint32_t now = nowMs();
  RF_LOCK();
  int i = 0;
  for (; i < ncand; i++) if (cand[i].bt == bt && cand[i].pi == pi && memcmp(cand[i].addr, a, 5) == 0) break;
  if (i == ncand) {
    if (ncand == CAND_MAX) { RF_UNLOCK(); return; }
    cand[i] = Cand();
    memcpy(cand[i].addr, a, 5);
    cand[i].bt = bt;
    cand[i].pi = (int8_t)pi;
    ncand++;
  }
  Cand &c = cand[i];
  c.packets++;
  if (bt) { RF_UNLOCK(); return; }   // a paging host hops over 32 channels: no frequency to keep
  int j = 0;
  for (; j < c.nf; j++) {
    int mean = (int)(c.f[j].khzSum / c.f[j].n);
    if (abs(mean - khz) <= 600) break;
  }
  if (j == c.nf) {
    if (c.nf == 6) { RF_UNLOCK(); return; }
    c.f[j] = LFreq{0, 0, now, now};
    c.nf++;
  }
  c.f[j].khzSum += khz;
  c.f[j].n++;
  c.f[j].last = now;
  RF_UNLOCK();
}

#ifdef MEMCHECK
static uint32_t dbgVisits, dbgBursts, dbgFrames, dbgMatched;
#endif

// The frequency with the most packets, once the address has REAL_PACKETS
// (detection does not depend on it; it is shown and kept for information).
static int mainFreq(const Cand &c) {
  if (c.packets < REAL_PACKETS) return -1;
  int best = -1;
  for (int j = 0; j < c.nf; j++) if (best < 0 || c.f[j].n > c.f[best].n) best = j;
  return best;
}

static void wakeRules(PowerState st, int hitIdx, bool exact = false);

// A hit of a protocol: recorded while its list is being learnt; a listed key
// of that protocol is a sighting for the wake rules.
static void hitFound(void *, const rfp::Hit &h) {
  bool host = h.proto->host;
  int pi = rfp::find(h.proto->id);
  RF_LOCK();
  bool learning = learnOn && learnBt == host;
  int idx = findEntry(h.key, host);
  if (idx >= 0 && list[idx].pi != pi) idx = -1;
  bool wasPresent = idx >= 0 && list[idx].seen && nowMs() - list[idx].lastSeen < PRESENT_MS;
  char name[32] = {0};
  if (idx >= 0) memcpy(name, list[idx].r.name, sizeof(name));
  RF_UNLOCK();
  if (learning) learnRecord(h.key, h.khz, host, pi);
#ifdef MEMCHECK
  if (host) conPrintf("BTC %02X%02X%02X %s %.1f\n", h.key[0], h.key[1], h.key[2], h.exact ? "exact" : "match", h.khz / 1000.0f);
#endif
  if (idx < 0) return;
  if (!wasPresent) {
    if (host) logEvent("2.4G/BT: %.30s calling (Bluetooth Classic)", name);
    else logEvent("2.4G/BT: %.30s seen (%.1f MHz)", name, h.khz / 1000.0f);
  }
#ifdef MEMCHECK
  if (!host) conPrintf("SEEN %s %.1f\n", name, h.khz / 1000.0f);
#endif
  wakeRules(powerStateSync(), idx, h.exact);
}

#ifdef MEMCHECK
static void traceBurst(void *, const rfdsp::Span &sp, int off, const rfdsp::Envelope &env, const rfp::Hit *hit) {
  dbgBursts++;
  if (hit && !hit->proto->host) { dbgFrames++; if (!hit->exact) dbgMatched++; }
  conPrintf("LB len=%u off=%d thr=%u med=%u %s\n", sp.e - sp.s, off / 1000, (unsigned)env.thr, (unsigned)env.med,
            hit ? keyHex(hit->key, hit->proto->keyLen).c_str() : "-");
}
#endif

// Analyse one snapshot taken on WiFi channel `ch`.
static void analyseSnapshot(int ch) {
  // The keys to look for: the wake list and, while a 2.4G Learn runs, every
  // address it has already found with a valid CRC (a long packet often has a
  // bit error somewhere in its payload). A protocol runs when it has a
  // listed key or its list is being learnt.
  rfp::Listed lst[WAKE_MAX + CAND_MAX];
  rfp::Query q = {};
  int n = 0;
  RF_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    if (!list[i].used || list[i].pi < 0) continue;
    lst[n].pat = list[i].pat;
    memcpy(lst[n].key, list[i].r.addr, 5);
    lst[n].proto = (uint8_t)list[i].pi;
    q.active |= 1u << list[i].pi;
    n++;
  }
  if (learnOn)
    for (int p = 0; p < rfp::count(); p++) if (rfp::at(p)->host == learnBt) q.active |= 1u << p;
  if (learnOn && !learnBt)
    for (int i = 0; i < ncand; i++) {
      if (cand[i].bt || cand[i].pi < 0) continue;
      lst[n].pat = rfp::at(cand[i].pi)->pattern(cand[i].addr);
      memcpy(lst[n].key, cand[i].addr, 5);
      lst[n].proto = (uint8_t)cand[i].pi;
      n++;
    }
  RF_UNLOCK();
  q.listed = lst;
  q.nlisted = n;
  q.active &= rfp::readyMask();
  q.chKhz = chCentreKhz(ch);
#ifdef MEMCHECK
  q.trace = traceBurst;
#endif
  rfp::analyse(IQ_BUFFER, work, q, hitFound, nullptr);
}

void rfLearn(bool on, bool bt) {
  RF_LOCK();
  if (on) { ncand = 0; learnUntil = nowMs() + (uint32_t)cfg.scan_s * 1000UL; learnBt = bt; }
  if (learnOn && !on) { learnEndAt = nowMs(); learnEnded = true; }
  learnOn = on;
  RF_UNLOCK();
  if (on) logEvent(bt ? "2.4G/BT: learning Bluetooth Classic hosts" : "2.4G/BT: learning 2.4G Dongle devices");
  else logEvent("2.4G/BT: learning stopped");
}

bool rfLearnActive() {
  if (!rfMx) return false;   // Bluetooth mode: the 2.4G module never started
  RF_LOCK();
  bool a = learnOn;
  RF_UNLOCK();
  return a;
}

void rfLearnToJson(JsonObject out) {
  Cand copy[CAND_MAX];
  uint8_t inList[CAND_MAX] = {0};
  RF_LOCK();
  int n = ncand;
  memcpy(copy, cand, sizeof(Cand) * n);
  bool on = learnOn;
  int32_t ms = (int32_t)(learnUntil - nowMs());
  uint32_t left = on && ms > 0 ? (uint32_t)ms / 1000 : 0;
  for (int i = 0; i < n; i++) inList[i] = findEntry(copy[i].addr, copy[i].bt) >= 0;
  RF_UNLOCK();
  out["active"] = on;
  out["kind"] = learnBt ? "bt" : "dongle";
  out["left_s"] = left;
  JsonArray arr = out["found"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    const Cand &c = copy[i];
    JsonObject o = arr.add<JsonObject>();
    o["addr"] = keyHex(c.addr, c.bt ? 3 : (c.pi >= 0 ? rfp::at(c.pi)->keyLen : 5));
    o["kind"] = c.bt ? "bt" : "dongle";
    if (c.pi >= 0) o["proto"] = rfp::at(c.pi)->id;
    o["packets"] = c.packets;
    if (c.bt) o["ready"] = c.packets >= REAL_PACKETS;   // no frequency: ready by packet count
    int fx = mainFreq(c);
    o["sessions"] = 1;
    o["in_list"] = inList[i] != 0;
    JsonArray fa = o["freqs"].to<JsonArray>();
    for (int j = 0; j < c.nf; j++) {
      JsonObject f = fa.add<JsonObject>();
      f["mhz"] = roundf((float)(c.f[j].khzSum / c.f[j].n) / 100.0f) / 10.0f;
      f["packets"] = c.f[j].n;
      f["sessions"] = 1;
    }
    if (fx >= 0) o["fixed_mhz"] = roundf((float)(c.f[fx].khzSum / c.f[fx].n) / 100.0f) / 10.0f;
    else o["fixed_mhz"] = nullptr;
  }
}

// ------------------------------------------------------------------ task
#ifdef MEMCHECK
static volatile uint32_t *canary;
static uint32_t canaryN, canaryBad;
static void canaryCheck() {
  for (uint32_t j = 0; j < canaryN * 256; j++)
    if (canary[j] != (0x5a5a0000u ^ j)) { canaryBad++; canary[j] = 0x5a5a0000u ^ j; }
}
#else
static inline void canaryCheck() {}
#endif

// Arming and wake decision (rf lock), the wake call after releasing it.
static void wakeRules(PowerState st, int hitIdx, bool exact) {
  uint32_t now = nowMs();
  bool entered = (st != lastState) && st == PWR_OFF;
  if (st != lastState) {
    if (st == PWR_OFF) offSince = now;
    lastState = st;
  }
  char why[48] = {0};
  bool doWake = false;
  uint32_t rearmMs = (uint32_t)cfg.ble_rearm_s * 1000UL;
  RF_LOCK();
  if (entered || learnOn) for (int i = 0; i < WAKE_MAX; i++) list[i].armed = list[i].pending = false;
  if (hitIdx >= 0 && list[hitIdx].used) {
    Entry &e = list[hitIdx];
    if (!learnOn && e.armed && !e.off && cfg.ble_wake && st == PWR_OFF) {
      if (exact || (e.pending && now - e.pendingAt <= CONFIRM_MS)) {
        snprintf(why, sizeof(why), "2.4G/BT: %.30s", e.r.name);
        doWake = true;
        e.pending = false;
      } else {
        e.pending = true;     // first approximate match: wait for a second one
        e.pendingAt = now;
      }
    }
    e.seen = true;
    e.lastSeen = now;
  }
  if (cfg.ble_wake && st == PWR_OFF && !learnOn) {
    for (int i = 0; i < WAKE_MAX; i++) {
      Entry &e = list[i];
      if (!e.used || e.armed) continue;
      // Unheard for ble_rearm_s since entering off (or the start), or since
      // its last sighting: the same rule as Bluetooth.
      uint32_t ref = offSince;
      if (e.seen && (int32_t)(e.lastSeen - ref) > 0) ref = e.lastSeen;
      if (learnEnded && (int32_t)(learnEndAt - ref) > 0) ref = learnEndAt;
      if ((int32_t)(now - ref) >= (int32_t)rearmMs) { e.armed = true; e.armedAt = now; e.pending = false; }   // signed, as in ble.cpp
    }
  }
  RF_UNLOCK();
  if (doWake) powerWake(why);
}

// ----------------------------------------------------- snapshot export
// GET /api/rf/snapshot: the web handler (httpd task) asks, this task takes
// one snapshot on the wanted channel and keeps the buffer untouched until
// the handler has sent it (or SNAP_HOLD_MS after the last chunk). Nothing
// here needs the net lock, so the handler may wait for us while holding it.
enum { SN_IDLE, SN_REQ, SN_BUSY, SN_READY, SN_FAIL };
static std::atomic<int> snState{SN_IDLE};
static std::atomic<int> snCh{0};
static std::atomic<uint32_t> snHoldUntil{0};
static TaskHandle_t rfTaskH;
static const uint32_t SNAP_HOLD_MS = 3000, SNAP_WAIT_MS = 2000;

static bool snapHeld() {   // rf task: the buffer still belongs to the web handler
  if (snState.load() != SN_READY) return false;
  if ((int32_t)(nowMs() - snHoldUntil.load()) < 0) return true;
  snState.store(SN_IDLE);   // the client vanished
  return false;
}
static bool radioFree() {
  return wifiConnected() && !wifiApUp() && !webOtaRunning() && !wifiRadioBusy();
}
static void snapService() {
  int e = SN_REQ;
  if (!snState.compare_exchange_strong(e, SN_BUSY)) return;
  bool ok = radioFree() && !rocBackoff() && visit(snCh.load());
  if (ok) snHoldUntil.store(nowMs() + SNAP_HOLD_MS);
  snState.store(ok ? SN_READY : SN_FAIL);
}

int rfSnapBegin(int ch, const uint8_t *&buf, std::string &err) {
  int lo, hi;
  channelRange(lo, hi);
  if (ch < lo || ch > hi) { err = strPrintf("ch must be %d to %d", lo, hi); return 400; }
  if (!rfTaskH) { err = "2.4G/BT wake is not running"; return 409; }
  if (!wifiConnected() || wifiApUp()) { err = "WiFi is not connected as a station"; return 409; }
  if (webOtaRunning()) { err = "a firmware upload is running"; return 409; }
  if (wifiRadioBusy()) { err = "WiFi is scanning or roaming, try again"; return 409; }
  int e = SN_IDLE;
  if (!snState.compare_exchange_strong(e, SN_REQ)) { err = "another snapshot is in progress"; return 409; }
  snCh.store(ch);
  xTaskNotifyGive(rfTaskH);
  for (uint32_t t = 0; t < SNAP_WAIT_MS; t += 5) {
    int s = snState.load();
    if (s == SN_READY) { buf = (const uint8_t *)IQ_BUFFER; snHoldUntil.store(nowMs() + SNAP_HOLD_MS); return 0; }
    if (s == SN_FAIL) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  int r = SN_REQ;
  if (snState.compare_exchange_strong(r, SN_IDLE)) { err = "radio busy, try again"; return 409; }   // never started
  while (snState.load() == SN_BUSY) vTaskDelay(pdMS_TO_TICKS(5));   // a capture is under way (<= 25 ms)
  if (snState.load() == SN_READY) { buf = (const uint8_t *)IQ_BUFFER; snHoldUntil.store(nowMs() + SNAP_HOLD_MS); return 0; }
  snState.store(SN_IDLE);
  err = "snapshot failed, try again";
  return 409;
}
void rfSnapHold() { snHoldUntil.store(nowMs() + SNAP_HOLD_MS); }
void rfSnapEnd() { snState.store(SN_IDLE); }

// About a second of channel visits, sweeping every allowed channel. Every
// snapshot is decoded the same way when learning and when watching.
static void sweepCycle(bool learning) {
  int lo, hi;
  channelRange(lo, hi);
  uint32_t t0 = nowMs();
  while (nowMs() - t0 < 900) {   // ~1 s of sweeping, then back to the bookkeeping
    if (webOtaRunning()) break;   // an upload needs WiFi (and its memory) at once
    snapService();
    if (snapHeld()) break;
    // A WiFi scan (ours, or a roaming scan after a weak-signal event) or a
    // roam is under way: the driver serves a channel visit and a scan one
    // after the other, so a visit would only delay it (or be answered with
    // the wrong channel's snapshot). Stand aside.
    if (wifiRadioBusy() || rocBackoff()) break;
    if (learnCh < lo || learnCh > hi) learnCh = lo;
    int ch = learnCh++;
    bool ok = visit(ch);
    canaryCheck();
    if (ok) analyseSnapshot(ch);
#ifdef MEMCHECK
    dbgVisits++;
#endif
    vTaskDelay(pdMS_TO_TICKS(learning ? GAP_LEARN_MS : GAP_WATCH_MS));   // WiFi's time
  }
#ifdef MEMCHECK
  conPrintf("LEARN visits=%lu bursts=%lu frames=%lu matched=%lu disturbed=%lu\n", (unsigned long)dbgVisits,
                (unsigned long)dbgBursts, (unsigned long)dbgFrames, (unsigned long)dbgMatched, (unsigned long)canaryBad);
#endif
}

static void rfTask(void *) {
#ifdef MEMCHECK
  // Test: once app_main has taken its quiet memory (task stacks), fill the
  // unused rest with a pattern; it is checked after every capture.
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (canaryN = 0; quietAlloc(1024); canaryN++) {}
  canary = (volatile uint32_t *)(0x3fcb0000 - canaryN * 1024);
  for (uint32_t j = 0; j < canaryN * 256; j++) canary[j] = 0x5a5a0000u ^ j;
  conPrintf("QUIET left %lu KB\n", (unsigned long)canaryN);
#endif
  for (;;) {
    uint32_t cyc0 = nowMs();
    PowerState st = powerStateSync();
    wakeRules(st, -1);   // notes the switch to off (offSince) before any sweep
    // Only with WiFi connected as a station (the visit is a station
    // feature) and not while a firmware upload runs.
    bool radioOk = wifiConnected() && !wifiApUp() && !webOtaRunning() && !wifiRadioBusy() && !rocBackoff();
    if (radioOk) {
      wifi_ps_type_t ps;
      if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE) esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (radioOk) snapService();
    RF_LOCK();
    if (learnOn && (int32_t)(nowMs() - learnUntil) >= 0) { learnOn = false; learnEndAt = nowMs(); learnEnded = true; }
    bool learning = learnOn;
    RF_UNLOCK();
    int n, p, a;
    rfWakeCounts(n, p, a);
    if (radioOk && !snapHeld() && (learning || (st == PWR_OFF && cfg.ble_wake && n > 0))) sweepCycle(learning);
    wakeRules(powerStateSync(), -1);
    uint32_t used = nowMs() - cyc0;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(used < PERIOD_MS - 10 ? PERIOD_MS - used : 10));   // a snapshot request wakes it
  }
}

void rfInit() {
  rfMx = xSemaphoreCreateMutex();
  list = (Entry *)calloc(WAKE_MAX, sizeof(Entry));
  cand = (Cand *)calloc(CAND_MAX, sizeof(Cand));
  work.sintab = (int16_t *)quietAlloc(sizeof(int16_t) * 256);
  work.mbuf = (int32_t *)quietAlloc(sizeof(int32_t) * rfdsp::MBUF);
  work.env = (uint16_t *)quietAlloc(sizeof(uint16_t) * rfdsp::ENVN);
  work.envSorted = (uint16_t *)quietAlloc(sizeof(uint16_t) * rfdsp::ENVN);
  if (!list || !cand || !work.sintab || !work.mbuf || !work.env || !work.envSorted) {
    logEvent("2.4G/BT: out of memory, 2.4G/BT wake off");
    return;
  }
  rfdsp::fillSintab(work.sintab);
  rfp::initAll();   // a protocol that cannot get its memory stays off
  for (int i = 0; i < rfp::count(); i++)
    if (!(rfp::readyMask() >> i & 1)) logEvent("2.4G/BT: out of memory for %.30s", rfp::at(i)->name);
  loadList();
  offSince = nowMs();   // arming counts from the start too
  lastState = powerStateSync();
  rocDone = xSemaphoreCreateBinary();
  // Heap stack, never quiet memory: this task runs the capture, and while
  // the engine owns the bank its own stack there breaks the capture (tested:
  // no burst found at all).
  xTaskCreate(rfTask, "rf", RF_STACK, nullptr, 2, &rfTaskH);
  int n, p, a;
  rfWakeCounts(n, p, a);
  logEvent("2.4G/BT wake, %d entries in the list", n);
}
