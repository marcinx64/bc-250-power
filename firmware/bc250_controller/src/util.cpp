// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- locks
static StaticSemaphore_t netBuf;
static SemaphoreHandle_t netMx = nullptr;
void netLock() { if (netMx) xSemaphoreTakeRecursive(netMx, portMAX_DELAY); }
void netUnlock() { if (netMx) xSemaphoreGiveRecursive(netMx); }

// ---------------------------------------------------------------- start-up
void utilInit() {
  static bool done = false;
  if (done) return;
  done = true;
  netMx = xSemaphoreCreateRecursiveMutexStatic(&netBuf);

  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  esp_netif_init();
  esp_event_loop_create_default();   // fails harmlessly when it exists

  // Console through the driver with a small TX ring: writes with a zero wait
  // never block, whatever the host does (HWCDC with setTxTimeoutMs(0) before).
  usb_serial_jtag_driver_config_t uc = {};
  uc.tx_buffer_size = 512;
  uc.rx_buffer_size = 128;   // the driver needs more than 64
  if (usb_serial_jtag_driver_install(&uc) == ESP_OK) usb_serial_jtag_vfs_use_driver();
}

void conWrite(const char *s, size_t n) {
  if (!n || !usb_serial_jtag_is_driver_installed() || !usb_serial_jtag_is_connected()) return;
  usb_serial_jtag_write_bytes(s, n, 0);
}

void conPrintf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) conWrite(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

// ---------------------------------------------------------------- strings
std::string cleanText(const char *s, size_t maxBytes) {
  std::string out;
  if (!s) return out;
  size_t i = 0, n = strlen(s);
  while (i < n && out.length() < maxBytes) {
    uint8_t c = (uint8_t)s[i];
    int len = 1;
    bool ok = false;
    if (c >= 0x20 && c < 0x7f) ok = true;
    else if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    if (len > 1) {
      ok = (i + len <= n);
      for (int k = 1; ok && k < len; k++) ok = (((uint8_t)s[i + k]) & 0xC0) == 0x80;
    }
    if (out.length() + (ok ? len : 1) > maxBytes) break;
    if (ok) { for (int k = 0; k < len; k++) out += s[i + k]; i += len; }
    else { out += '?'; i += 1; }
  }
  return out;
}

std::string strPrintf(const char *fmt, ...) {
  char buf[128];
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::string out;
  if (n < 0) { va_end(ap2); return out; }
  if ((size_t)n < sizeof(buf)) out.assign(buf, n);
  else {
    out.resize(n);
    vsnprintf(&out[0], (size_t)n + 1, fmt, ap2);
  }
  va_end(ap2);
  return out;
}

std::string upperCase(std::string s) {
  for (auto &c : s) if (c >= 'a' && c <= 'z') c -= 32;
  return s;
}
std::string lowerCase(std::string s) {
  for (auto &c : s) if (c >= 'A' && c <= 'Z') c += 32;
  return s;
}

// ---------------------------------------------------------------- NVS
bool Nvs::open(const char *ns, bool rw) {
  close();
  nvs_handle_t h;
  if (nvs_open(ns, rw ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) return false;
  h_ = (uint32_t)h;
  rw_ = rw;
  return true;
}

void Nvs::close() {
  if (h_) nvs_close((nvs_handle_t)h_);
  h_ = 0;
}

uint8_t Nvs::getU8(const char *key, uint8_t def) {
  uint8_t v = def;
  if (h_ && nvs_get_u8((nvs_handle_t)h_, key, &v) != ESP_OK) v = def;
  return v;
}
bool Nvs::setU8(const char *key, uint8_t v) {
  return h_ && rw_ && nvs_set_u8((nvs_handle_t)h_, key, v) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}
uint32_t Nvs::getU32(const char *key, uint32_t def) {
  uint32_t v = def;
  if (h_ && nvs_get_u32((nvs_handle_t)h_, key, &v) != ESP_OK) v = def;
  return v;
}
bool Nvs::setU32(const char *key, uint32_t v) {
  return h_ && rw_ && nvs_set_u32((nvs_handle_t)h_, key, v) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}
int32_t Nvs::getI32(const char *key, int32_t def) {
  int32_t v = def;
  if (h_ && nvs_get_i32((nvs_handle_t)h_, key, &v) != ESP_OK) v = def;
  return v;
}
bool Nvs::setI32(const char *key, int32_t v) {
  return h_ && rw_ && nvs_set_i32((nvs_handle_t)h_, key, v) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}

std::string Nvs::getStr(const char *key, const std::string &def) {
  if (!h_) return def;
  size_t len = 0;
  if (nvs_get_str((nvs_handle_t)h_, key, nullptr, &len) != ESP_OK || len == 0) return def;
  std::string s(len, '\0');
  if (nvs_get_str((nvs_handle_t)h_, key, &s[0], &len) != ESP_OK) return def;
  s.resize(strlen(s.c_str()));   // drop the terminator
  return s;
}
bool Nvs::setStr(const char *key, const std::string &v) {
  return h_ && rw_ && nvs_set_str((nvs_handle_t)h_, key, v.c_str()) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}

size_t Nvs::blobLen(const char *key) {
  size_t len = 0;
  if (!h_ || nvs_get_blob((nvs_handle_t)h_, key, nullptr, &len) != ESP_OK) return 0;
  return len;
}
size_t Nvs::getBlob(const char *key, void *buf, size_t cap) {
  size_t len = blobLen(key);
  if (!len || len > cap) return 0;
  if (nvs_get_blob((nvs_handle_t)h_, key, buf, &len) != ESP_OK) return 0;
  return len;
}
bool Nvs::setBlob(const char *key, const void *buf, size_t n) {
  return h_ && rw_ && nvs_set_blob((nvs_handle_t)h_, key, buf, n) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}

bool Nvs::has(const char *key) {
  nvs_type_t t;
  return h_ && nvs_find_key((nvs_handle_t)h_, key, &t) == ESP_OK;
}

bool Nvs::remove(const char *key) {
  if (!h_ || !rw_) return false;
  esp_err_t e = nvs_erase_key((nvs_handle_t)h_, key);
  if (e == ESP_ERR_NVS_NOT_FOUND) return true;
  return e == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}

bool Nvs::clear() {
  return h_ && rw_ && nvs_erase_all((nvs_handle_t)h_) == ESP_OK && nvs_commit((nvs_handle_t)h_) == ESP_OK;
}
