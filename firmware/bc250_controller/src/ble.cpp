// SPDX-License-Identifier: GPL-3.0-or-later
// Bluetooth wake. Scan only: the NimBLE host (observer role) of ESP-IDF, no
// Arduino BLE library (not used).
// Threads: the GAP callback (NimBLE host task), bleUpdate() (BLE task, ~25 ms)
// and web handlers (httpd task, under netLock) share the tables below. ALL
// access is under `bleMx` (a FreeRTOS mutex), held only for plain copies and
// compares. Handlers copy out, release, then serialise JSON / write NVS.
// Lock order: bleMx is never held while taking the control lock or calling
// the NimBLE API; it may be held while calling logEvent (leaf lock) but we
// avoid even that. powerWake()/powerStateSync() are called with bleMx free.
// The GAP callback only does short work under bleMx: it never waits for
// anything else.
#include "ble_bt.h"   // renames the public functions to bt* (wake.cpp dispatches)
#include "ble.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "web.h"
#include "power.h"
#include "settings.h"
#include "eventlog.h"
#include "util.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define WAKE_MAX 8
#define DISC_MAX 40
static const uint32_t SCAN_CHUNK_S = 5;
// The discovery runs until Stop or for the scan_s setting, the same time as 2.4G Learn.
static const uint32_t PRESENT_MS = 5000;

struct WakeEntry {
  bool used;
  uint8_t mac[6];     // native order (least significant byte first), as NimBLE gives it
  uint8_t type;       // 0 public, 1 random
  char name[32];
  // shared with the callback:
  bool seen;
  uint32_t lastSeen;
  int rssi;
  // BLE task only:
  bool armed;
  uint32_t armedAt;
  bool off;           // disabled: still heard and shown, never wakes the PC
};

struct DiscEntry {
  uint8_t mac[6];
  uint8_t type;
  int rssi;
  char name[32];
  uint16_t appearance;   // AD 0x19, merged over packets; 0 = none seen
  bool hid;              // 0x1812 seen in a service UUID list
};

// Section 14 kind table. Pure function of the two merged fields.
enum Kind { K_OTHER, K_GAMEPAD, K_JOYSTICK, K_KEYBOARD, K_MOUSE, K_HID };
static Kind kindOf(uint16_t app, bool hid) {
  switch (app) {
    case 0x03C4: return K_GAMEPAD;
    case 0x03C3: return K_JOYSTICK;
    case 0x03C1: return K_KEYBOARD;
    case 0x03C2: return K_MOUSE;
  }
  if ((app >= 0x03C0 && app <= 0x03FF) || hid) return K_HID;
  return K_OTHER;
}
static const char *kindName(Kind k) {
  static const char *n[] = {"other", "gamepad", "joystick", "keyboard", "mouse", "hid"};
  return n[k];
}
static bool controllerLike(Kind k) { return k == K_GAMEPAD || k == K_JOYSTICK || k == K_HID; }

static SemaphoreHandle_t bleMx = nullptr;
#define BLE_LOCK() xSemaphoreTake(bleMx, portMAX_DELAY)
#define BLE_UNLOCK() xSemaphoreGive(bleMx)
static WakeEntry *wake = nullptr;   // heap (bleInit)
static DiscEntry *disc = nullptr;
static int discCount = 0;
static bool discCollect = false;       // callback collects while true (ble lock)
static bool discActive = false;        // discovery running (ble lock)
static uint32_t discEndAt = 0;         // (ble lock)
static bool discRestart = false;       // request to the BLE task (ble lock)
static uint32_t adverts = 0;           // (ble lock for writes; word reads are atomic)
static volatile bool scanDone = false; // set by the GAP callback when a chunk ends, or by a host reset
static volatile bool hostSynced = false;
static uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;
static bool btReady = false;           // nimble_port_init succeeded

// BLE task only:
static bool scanRunning = false;
static bool scanActiveMode = false;
static PowerState lastState = PWR_OFF;
static uint32_t offSince = 0;
// While a device scan runs every entry is disarmed; when it ends,
// arming counts from then, as from a shutdown.
static uint32_t discEndedAt = 0;
static bool discEnded = false;

static std::string macStr(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[5], m[4], m[3], m[2], m[1], m[0]);
  return std::string(b);
}

// "aa:bb:cc:dd:ee:ff" -> native (reversed) order. Strict.
static bool parseMac(const std::string &s, uint8_t *m) {
  if (s.length() != 17) return false;
  for (int i = 0; i < 6; i++) {
    int p = i * 3;
    if (i < 5 && s[p + 2] != ':') return false;
    char a = tolower((unsigned char)s[p]), b = tolower((unsigned char)s[p + 1]);
    if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b)) return false;
    auto hv = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    m[5 - i] = (hv(a) << 4) | hv(b);
  }
  return true;
}

static uint8_t normType(uint8_t t) { return (t == 1 || t == 3) ? 1 : 0; }
static const char *typeName(uint8_t t) { return t ? "random" : "public"; }

// 16-bit UUID 0x1812 as a 128-bit UUID (little endian): base UUID with the
// 16 bits in bytes 12..13. A 32-bit list entry 0x00001812 matches too, as in
// the Arduino BLE library's comparison.
static bool isHidUuid128(const uint8_t *u) {
  static const uint8_t base[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                   0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00};
  return memcmp(u, base, 16) == 0;
}

// GAP event callback (NimBLE host task).
static int gapEvent(struct ble_gap_event *ev, void *) {
  if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    scanDone = true;
    return 0;
  }
  if (ev->type != BLE_GAP_EVENT_DISC) return 0;

  const struct ble_gap_disc_desc &d = ev->disc;
  uint8_t mac[6];
  memcpy(mac, d.addr.val, 6);
  uint8_t type = normType(d.addr.type);
  int rssi = d.rssi;
  uint32_t now = nowMs();
  // Name, appearance and HID flag are only needed in discovery. Parse before
  // taking the lock again.
  BLE_LOCK();
  bool collect = discCollect;
  BLE_UNLOCK();
  char name[32] = {0};
  uint16_t app = 0;
  bool hid = false;
  if (collect && d.length_data > 0) {
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) == 0) {
      // complete name only (AD 0x09), as the Arduino BLE library does
      if (f.name != nullptr && f.name_len > 0 && f.name_is_complete) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
      }
      if (f.appearance_is_present) app = f.appearance;
      for (int i = 0; i < f.num_uuids16 && !hid; i++) hid = f.uuids16[i].value == 0x1812;
      for (int i = 0; i < f.num_uuids32 && !hid; i++) hid = f.uuids32[i].value == 0x1812;
      for (int i = 0; i < f.num_uuids128 && !hid; i++) hid = isHidUuid128(f.uuids128[i].value);
    }
  }
  BLE_LOCK();
  adverts++;
  collect = discCollect;
  for (int i = 0; i < WAKE_MAX; i++) {
    WakeEntry &w = wake[i];
    if (w.used && w.type == type && memcmp(w.mac, mac, 6) == 0) {
      w.seen = true; w.lastSeen = now; w.rssi = rssi;
    }
  }
  if (collect) {
    int idx = -1;
    for (int i = 0; i < discCount; i++)
      if (disc[i].type == type && memcmp(disc[i].mac, mac, 6) == 0) { idx = i; break; }
    if (idx < 0) {
      if (discCount < DISC_MAX) {
        idx = discCount++;
      } else if (controllerLike(kindOf(app, hid))) {
        // Table full: a controller-like device replaces the weakest entry
        // that is not controller-like (never another controller).
        int weakest = -1;
        for (int i = 0; i < DISC_MAX; i++)
          if (!controllerLike(kindOf(disc[i].appearance, disc[i].hid)) &&
              (weakest < 0 || disc[i].rssi < disc[weakest].rssi)) weakest = i;
        idx = weakest;
      }
      if (idx >= 0) {
        memcpy(disc[idx].mac, mac, 6);
        disc[idx].type = type;
        disc[idx].name[0] = 0;
        disc[idx].appearance = 0;
        disc[idx].hid = false;
      }
    }
    if (idx >= 0) {
      disc[idx].rssi = rssi;
      if (name[0] && !disc[idx].name[0]) strlcpy(disc[idx].name, name, sizeof(disc[idx].name));
      if (app) disc[idx].appearance = app;   // never cleared by a later packet
      if (hid) disc[idx].hid = true;
    }
  }
  BLE_UNLOCK();
  return 0;
}

static void onReset(int) {
  hostSynced = false;
  scanDone = true;   // the BLE task restarts the scan once synced again
}

static void onSync() {
  uint8_t t = BLE_OWN_ADDR_PUBLIC;
  if (ble_hs_id_infer_auto(0, &t) != 0) t = BLE_OWN_ADDR_RANDOM;
  ownAddrType = t;
  hostSynced = true;
}

static void hostTask(void *) {
  nimble_port_run();   // returns only after nimble_port_stop()
  nimble_port_freertos_deinit();
}

// Listening time out of every 200 ms. While the PC is off the scan listens
// most of the time so a controller is noticed quickly; WiFi (same radio)
// has little to do then. Otherwise the proven 60 ms share is used, which
// keeps the web UI responsive.
static const uint16_t SCAN_INTERVAL_MS = 200;
static const uint16_t SCAN_WINDOW_MS = 60;
static const uint16_t SCAN_WINDOW_FAST_MS = 160;
static bool scanFast = false;

static void startChunk(bool active, bool fast) {
  struct ble_gap_disc_params p = {};
  p.itvl = (uint16_t)(SCAN_INTERVAL_MS / 0.625);                              // 320 = 200 ms
  p.window = (uint16_t)((fast ? SCAN_WINDOW_FAST_MS : SCAN_WINDOW_MS) / 0.625);   // 96 = 60 ms, 256 = 160 ms
  p.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  p.limited = 0;
  p.passive = active ? 0 : 1;
  p.filter_duplicates = 0;   // every advert counts: lastSeen must follow the device
  scanFast = fast;
  scanActiveMode = active;
  scanDone = false;
  int rc = hostSynced ? ble_gap_disc(ownAddrType, SCAN_CHUNK_S * 1000, &p, gapEvent, nullptr)
                      : BLE_HS_ENOTSYNCED;
  scanRunning = (rc == 0 || rc == BLE_HS_EALREADY);
}

static void stopScan() {
  ble_gap_disc_cancel();   // no event follows a cancel
  scanRunning = false;
  scanDone = false;
}

struct Rec { uint8_t mac[6]; uint8_t type; char name[32]; };

static void saveWakeList() {
  Rec recs[WAKE_MAX];
  uint8_t n = 0;
  uint32_t off = 0;   // bit k: the k-th saved entry is disabled (absent = all enabled)
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    if (!wake[i].used) continue;
    if (wake[i].off) off |= 1u << n;
    memcpy(recs[n].mac, wake[i].mac, 6);
    recs[n].type = wake[i].type;
    memcpy(recs[n].name, wake[i].name, 32);
    n++;
  }
  BLE_UNLOCK();
  Nvs p;   // flash write: outside the lock
  if (!p.open("ble", true)) return;
  p.setU8("n", n);
  p.setBlob("list", recs, sizeof(Rec) * n);
  p.setU32("off", off);
}

static void loadWakeList() {
  Rec recs[WAKE_MAX];
  uint8_t n = 0;
  uint32_t off = 0;
  {
    Nvs p;
    if (p.open("ble", false)) {
      n = p.getU8("n", 0);
      if (n > WAKE_MAX) n = 0;
      if (n && p.getBlob("list", recs, sizeof(Rec) * n) != sizeof(Rec) * n) n = 0;
      off = p.getU32("off", 0);
    }
  }
  BLE_LOCK();
  memset(wake, 0, sizeof(WakeEntry) * WAKE_MAX);
  for (int i = 0; i < n; i++) {
    wake[i].used = true;
    memcpy(wake[i].mac, recs[i].mac, 6);
    wake[i].type = normType(recs[i].type);
    memcpy(wake[i].name, recs[i].name, 32);
    wake[i].name[31] = 0;
    wake[i].off = off >> i & 1;
  }
  BLE_UNLOCK();
}

void bleInit() {
  bleMx = xSemaphoreCreateMutex();   // before the scan can deliver a callback
  wake = (WakeEntry *)calloc(WAKE_MAX, sizeof(WakeEntry));
  disc = (DiscEntry *)calloc(DISC_MAX, sizeof(DiscEntry));
  loadWakeList();
  // Callbacks before the host starts. nimble_port_init() also brings up the
  // controller (VHCI transport).
  esp_err_t e = nimble_port_init();
  if (e == ESP_OK) {
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    nimble_port_freertos_init(hostTask);
    btReady = true;
  } else {
    logEvent("BLE init failed: %d", (int)e);
  }
  // Arming has one rule (same as 2.4G): a listed device arms after
  // ble_rearm_s unheard, counted from the PC turning off, from this start,
  // or from its last sighting. No grace period, no arming at once at start.
  offSince = nowMs();
  lastState = powerStateSync();
  // The BLE task (tasks.cpp) starts the first scan chunk.
}

// Another used entry (not `except`) already has this name, ignoring case.
// Empty names may repeat. Call with the ble lock held.
static bool nameTaken(const char *name, int except) {
  if (!name[0]) return false;
  for (int i = 0; i < WAKE_MAX; i++)
    if (i != except && wake[i].used && strcasecmp(wake[i].name, name) == 0) return true;
  return false;
}

bool bleWakeAdd(const std::string &addr, const std::string &type, const std::string &name, std::string &err) {
  uint8_t mac[6];
  if (!parseMac(addr, mac)) { err = "invalid address"; return false; }
  uint8_t t = 0;
  if (type == "random") t = 1;
  else if (type != "public" && type.length()) { err = "type must be public or random"; return false; }
  if (name.length() > 31) { err = "name too long"; return false; }
  int free = -1;
  bool ok = true;
  std::string clean = cleanText(name.c_str(), 31);
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    if (wake[i].used && wake[i].type == t && memcmp(wake[i].mac, mac, 6) == 0) { ok = false; free = -2; break; }
    if (!wake[i].used && free < 0) free = i;
  }
  if (ok && nameTaken(clean.c_str(), -1)) { ok = false; free = -3; }
  if (ok && free >= 0) {
    WakeEntry &w = wake[free];
    memset(&w, 0, sizeof(w));
    w.used = true;
    memcpy(w.mac, mac, 6);
    w.type = t;
    strlcpy(w.name, clean.c_str(), sizeof(w.name));
    // A device is normally added while it is on and advertising (it was
    // just found by a scan). Count it as seen right now, so it has to be
    // away for ble_rearm_s before it can wake the PC; otherwise adding a
    // controller would start the PC at its next advert.
    w.seen = true;
    w.lastSeen = nowMs();
  }
  BLE_UNLOCK();
  if (free == -2) { err = "already in list"; return false; }
  if (free == -3) { err = "that name is already used"; return false; }
  if (free < 0) { err = "list full (8 entries)"; return false; }
  saveWakeList();
  logEvent("BLE wake: added %s", addr.c_str());
  return true;
}

bool bleWakeRemove(const std::string &addr, std::string &err) {
  uint8_t mac[6];
  if (!parseMac(addr, mac)) { err = "invalid address"; return false; }
  bool found = false;
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++)
    if (wake[i].used && memcmp(wake[i].mac, mac, 6) == 0) { wake[i].used = false; found = true; }
  BLE_UNLOCK();
  if (!found) { err = "not in list"; return false; }
  saveWakeList();
  logEvent("BLE wake: removed %s", addr.c_str());
  return true;
}

// Empties the list (the running Bluetooth method; the other method's list
// is cleared in NVS by web.cpp).
void bleWakeClear() {
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) wake[i].used = false;
  BLE_UNLOCK();
  saveWakeList();
  logEvent("BLE wake list cleared");
}

bool bleWakeRename(const std::string &addr, const std::string &name, std::string &err) {
  uint8_t mac[6];
  if (!parseMac(addr, mac)) { err = "invalid address"; return false; }
  if (name.length() > 31) { err = "name too long"; return false; }
  std::string clean = cleanText(name.c_str(), 31);
  bool found = false, taken = false;
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++)
    if (wake[i].used && memcmp(wake[i].mac, mac, 6) == 0) {
      found = true;
      if (nameTaken(clean.c_str(), i)) { taken = true; break; }
      strlcpy(wake[i].name, clean.c_str(), sizeof(wake[i].name));
    }
  BLE_UNLOCK();
  if (!found) { err = "not in list"; return false; }
  if (taken) { err = "that name is already used"; return false; }
  saveWakeList();
  logEvent("BLE wake: renamed %s to %.31s", addr.c_str(), clean.c_str());
  return true;
}

bool bleWakeEnable(const std::string &addr, bool on, std::string &err) {
  uint8_t mac[6];
  if (!parseMac(addr, mac)) { err = "invalid address"; return false; }
  bool found = false;
  char name[32] = {0};
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++)
    if (wake[i].used && memcmp(wake[i].mac, mac, 6) == 0) { found = true; wake[i].off = !on; memcpy(name, wake[i].name, 32); }
  BLE_UNLOCK();
  if (!found) { err = "not in list"; return false; }
  saveWakeList();
  logEvent("BLE wake: %s %.31s", on ? "enabled" : "disabled", name[0] ? name : addr.c_str());
  return true;
}

void bleWakeToJson(JsonArray out) {
  static_assert(sizeof(WakeEntry) < 100, "copy lives on the caller's stack");
  WakeEntry copy[WAKE_MAX];
  BLE_LOCK();
  memcpy(copy, wake, sizeof(copy));
  BLE_UNLOCK();
  uint32_t now = nowMs();
  bool wakeOn = cfg.ble_wake;
  for (int i = 0; i < WAKE_MAX; i++) {
    if (!copy[i].used) continue;
    JsonObject o = out.add<JsonObject>();
    o["addr"] = macStr(copy[i].mac);
    o["type"] = typeName(copy[i].type);
    o["name"] = cleanText(copy[i].name, 31);
    o["present"] = copy[i].seen && wakeOn && (int32_t)(now - copy[i].lastSeen) < (int32_t)PRESENT_MS;
    o["rssi"] = copy[i].seen ? copy[i].rssi : 0;
    o["armed"] = copy[i].armed;
    o["enabled"] = !copy[i].off;
    o["last_s"] = copy[i].seen ? (int32_t)((now - copy[i].lastSeen) / 1000) : -1;   // as 2.4G
  }
}

void bleWakeCounts(int &count, int &present, int &armed) {
  count = present = armed = 0;
  uint32_t now = nowMs();
  bool wakeOn = cfg.ble_wake;
  BLE_LOCK();
  for (int i = 0; i < WAKE_MAX; i++) {
    if (!wake[i].used) continue;
    count++;
    if (wake[i].seen && wakeOn && (int32_t)(now - wake[i].lastSeen) < (int32_t)PRESENT_MS) present++;
    if (wake[i].armed && !wake[i].off) armed++;
  }
  BLE_UNLOCK();
}

void bleStartDiscovery() {
  // Only records the request; the BLE task restarts the scan in active mode.
  BLE_LOCK();
  discCount = 0;
  discCollect = true;
  discActive = true;
  discEndAt = nowMs() + (uint32_t)cfg.scan_s * 1000UL;
  discRestart = true;
  BLE_UNLOCK();
}

void bleStopDiscovery() {
  BLE_LOCK();
  if (discActive) discEndAt = nowMs();   // the BLE task ends it on its next run
  BLE_UNLOCK();
}

int bleDiscoveryLeftS() {
  BLE_LOCK();
  int32_t ms = discActive ? (int32_t)(discEndAt - nowMs()) : 0;
  BLE_UNLOCK();
  return ms > 0 ? (ms + 999) / 1000 : 0;
}

bool bleDiscoveryActive() {
  BLE_LOCK();
  bool a = discActive;
  BLE_UNLOCK();
  return a;
}

uint32_t bleAdverts() { return adverts; }

void bleDevicesToJson(JsonArray out) {
  static DiscEntry *copy = nullptr;  // heap, once: keeps ~1.9 KB off the stack (one caller at a time under netLock)
  if (!copy) copy = (DiscEntry *)calloc(DISC_MAX, sizeof(DiscEntry));
  if (!copy || !disc) return;
  uint8_t listMac[WAKE_MAX][6], listType[WAKE_MAX];
  bool listUsed[WAKE_MAX];
  int n;
  BLE_LOCK();
  n = discActive ? discCount : 0;   // the results go when the scan ends (as 2.4G Learn)
  memcpy(copy, disc, sizeof(DiscEntry) * n);
  for (int k = 0; k < WAKE_MAX; k++) {
    listUsed[k] = wake[k].used; listType[k] = wake[k].type; memcpy(listMac[k], wake[k].mac, 6);
  }
  BLE_UNLOCK();
  // strongest first (insertion sort, n <= 40)
  for (int i = 1; i < n; i++) {
    DiscEntry t = copy[i];
    int j = i - 1;
    while (j >= 0 && copy[j].rssi < t.rssi) { copy[j + 1] = copy[j]; j--; }
    copy[j + 1] = t;
  }
  for (int i = 0; i < n; i++) {
    JsonObject o = out.add<JsonObject>();
    o["addr"] = macStr(copy[i].mac);
    o["type"] = typeName(copy[i].type);
    o["name"] = cleanText(copy[i].name, 31);
    o["rssi"] = copy[i].rssi;
    bool inList = false;
    for (int k = 0; k < WAKE_MAX; k++)
      if (listUsed[k] && listType[k] == copy[i].type && memcmp(listMac[k], copy[i].mac, 6) == 0) inList = true;
    o["in_list"] = inList;
    o["kind"] = kindName(kindOf(copy[i].appearance, copy[i].hid));
    o["appearance"] = copy[i].appearance;
    o["hid"] = copy[i].hid;
  }
}

// Runs in the BLE task every ~25 ms.
void bleUpdate() {
  uint32_t now = nowMs();

  // 1. Discovery bookkeeping (ble lock, plain variables only).
  BLE_LOCK();
  if (discActive && (int32_t)(now - discEndAt) >= 0) {
    discActive = false;
    discEndedAt = now; discEnded = true;
    discCollect = false;
    discCount = 0;
    discRestart = true;     // back to passive
  }
  bool wantActive = discActive;
  bool restart = discRestart;
  discRestart = false;
  BLE_UNLOCK();

  // 2. Scan control. NimBLE calls happen with no lock of ours held.
  PowerState st = powerStateSync();   // control lock taken and released inside
  bool want = btReady && (cfg.ble_wake || wantActive);
  // Fast only for wake listening, and never while the web UI is in use or
  // a firmware upload runs: WiFi needs the radio then.
  bool wantFast = !wantActive && st == PWR_OFF && !webRecentlyActive();
  if (!want) {
    if (scanRunning) stopScan();
  } else if (!scanRunning || scanDone || restart || scanActiveMode != wantActive || scanFast != wantFast) {
    if (scanRunning && (restart || scanActiveMode != wantActive || scanFast != wantFast)) ble_gap_disc_cancel();
    startChunk(wantActive, wantFast);
  }

  // 3. Entering `off`: disarm everything.
  bool entered = (st != lastState) && st == PWR_OFF;
  if (st != lastState) {
    if (st == PWR_OFF) offSince = now;
    lastState = st;
  }

  // 4. Arming and wake decision, all under the ble lock; the actual wake
  // call is made after releasing it.
  bool doWake = false;
  char why[40] = {0};
  bool wakeOn = cfg.ble_wake;
  uint32_t rearmMs = (uint32_t)cfg.ble_rearm_s * 1000UL;
  BLE_LOCK();
  if (entered || wantActive) for (int i = 0; i < WAKE_MAX; i++) wake[i].armed = false;
  if (wakeOn && st == PWR_OFF && !wantActive) {
    for (int i = 0; i < WAKE_MAX && !doWake; i++) {
      WakeEntry &w = wake[i];
      if (!w.used) continue;
      if (!w.armed) {
        // Unseen since entering off (or since the last sighting) for ble_rearm_s.
        uint32_t ref = offSince;
        if (w.seen && (int32_t)(w.lastSeen - ref) > 0) ref = w.lastSeen;
        if (discEnded && (int32_t)(discEndedAt - ref) > 0) ref = discEndedAt;
        // Signed: the host task may record a sighting after `now` was read;
        // unsigned, that wraps and would arm a device that is being heard.
        if ((int32_t)(now - ref) >= (int32_t)rearmMs) { w.armed = true; w.armedAt = now; }
      } else if (!w.off && w.seen && (int32_t)(w.lastSeen - w.armedAt) > 0) {
        snprintf(why, sizeof(why), "BLE: %.20s", w.name[0] ? w.name : "device");
        doWake = true;
      }
    }
  }
  BLE_UNLOCK();
  if (doWake) powerWake(why);
}
