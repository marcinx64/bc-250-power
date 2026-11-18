// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Shared conventions of the firmware (plain ESP-IDF, no Arduino core).
//
//  - Strings are std::string. Headers take `const std::string &` for input and
//    `std::string &` for output (error text). No Arduino String/WString.
//  - nowMs(): milliseconds since boot as uint32_t, wraps after 49.7 days
//    like Arduino's millis(). Use it instead of millis() and keep
//    the wrap-safe arithmetic: `now - then >= x`, `(int32_t)(now - due) >= 0`.
//  - Persistent storage: class Nvs below (NVS namespaces and key types are the
//    ones of Arduino's Preferences, so devices running 2.1.20 or later (the
//    Arduino-based releases) keep their settings when they update over WiFi).
//  - Threading: the main task (main.cpp) owns the network side and the strings
//    of `cfg`, the WiFi and time modules. It calls wifiUpdate(), timeUpdate()
//    and webUpdate() with netLock() held. HTTP handlers (esp_http_server task)
//    take netLock() for their whole body (use NetGuard), so they see the same
//    state the main task sees. netLock() is a recursive mutex. Never wait for
//    anything slow (network, flash erase) in the control task; it never takes
//    netLock(). Lock order: netLock -> control lock (power.cpp) -> log lock
//    (eventlog.cpp). The ble lock is never held together with netLock.
//  - Console: printf/ESP_LOG go to USB-Serial-JTAG. conWrite()/conPrintf()
//    below never block (a line is dropped when the host does not read).
//    logEvent() uses them.
//  - Start-up (main.cpp: app_main) has already done: NVS flash init,
//    esp_netif_init(), default event loop creation, USB console driver. The
//    WiFi/web/BLE modules must not repeat these (or must tolerate "already
//    done").
//  - Everything runs on one core (ESP32-C3).
#include <stdint.h>
#include <stddef.h>
#include <string>

// First call in app_main: network lock, NVS flash, esp_netif, default event
// loop, USB console. Safe to call twice.
void utilInit();

uint32_t nowMs();

void netLock();     // recursive
void netUnlock();
struct NetGuard {   // RAII
  NetGuard() { netLock(); }
  ~NetGuard() { netUnlock(); }
  NetGuard(const NetGuard &) = delete;
  NetGuard &operator=(const NetGuard &) = delete;
};

// Copy of `s` that is valid UTF-8 and free of control characters, at most
// maxBytes long. SSIDs and Bluetooth names are attacker-controlled bytes;
// this keeps the JSON we emit valid (ArduinoJson escapes quotes/backslash).
std::string cleanText(const char *s, size_t maxBytes = 32);

std::string strPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string upperCase(std::string s);   // ASCII only
std::string lowerCase(std::string s);

// Non-blocking write to the USB console (nothing is written, and nothing
// waits, when no host is connected or the buffer is full).
void conWrite(const char *s, size_t n);
void conPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// One NVS namespace. Same layout and types as Arduino's Preferences (needed
// for compatibility with 2.1.20 and later):
//   putBool/getBool -> u8    putUChar -> u8     putInt -> i32
//   putUInt -> u32           putString -> str   putBytes -> blob
// Getters return the default when the namespace is not open or the key is
// missing. Every set commits at once. Call from tasks that may block on a
// flash write (never from the control task).
class Nvs {
 public:
  Nvs() = default;
  ~Nvs() { close(); }
  Nvs(const Nvs &) = delete;
  Nvs &operator=(const Nvs &) = delete;

  // rw=false on a namespace that was never written fails (returns false),
  // like Preferences::begin(ns, true).
  bool open(const char *ns, bool rw);
  void close();
  bool isOpen() const { return h_ != 0; }

  uint8_t getU8(const char *key, uint8_t def = 0);
  bool setU8(const char *key, uint8_t v);
  uint32_t getU32(const char *key, uint32_t def = 0);
  bool setU32(const char *key, uint32_t v);
  int32_t getI32(const char *key, int32_t def = 0);
  bool setI32(const char *key, int32_t v);
  bool getBool(const char *key, bool def = false) { return getU8(key, def ? 1 : 0) != 0; }
  bool setBool(const char *key, bool v) { return setU8(key, v ? 1 : 0); }
  std::string getStr(const char *key, const std::string &def = std::string());
  bool setStr(const char *key, const std::string &v);
  // Length of a stored blob, 0 if there is none.
  size_t blobLen(const char *key);
  // Reads the blob into buf (cap bytes). Returns its length, or 0 if the key
  // is missing or the blob is longer than cap (as Preferences::getBytes).
  size_t getBlob(const char *key, void *buf, size_t cap);
  bool setBlob(const char *key, const void *buf, size_t n);
  bool has(const char *key);   // a key of any type (Preferences::isKey)
  bool remove(const char *key);   // erase one key (Preferences::remove); true if it is gone
  bool clear();                // erase every key of the namespace

 private:
  uint32_t h_ = 0;
  bool rw_ = false;
};
