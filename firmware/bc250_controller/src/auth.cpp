// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "eventlog.h"
#include "util.h"

#define MAX_SESSIONS 4
static const int MAX_FAILS = 5;
static const uint32_t LOCK_MS = 30000;

static bool pwSet = false;
static uint8_t salt[16];
static uint8_t hashStored[32];

struct Session { bool used; char sid[33]; uint32_t seq; };
// Logins survive a software restart (web restart, firmware update, wake
// method switch) but not a crash or a power loss: the sessions live in
// RTC memory with a check value tied to the stored password, and are taken
// over at start only after esp_restart().
struct RtcAuth { uint32_t magic; uint32_t seqCounter; uint8_t pwTag[8]; Session s[MAX_SESSIONS]; uint32_t sum; };
static RTC_NOINIT_ATTR RtcAuth rtc;
static const uint32_t RTC_MAGIC = 0xA57E0221;
static Session (&sessions)[MAX_SESSIONS] = rtc.s;
static uint32_t &seqCounter = rtc.seqCounter;

static int fails = 0;
static bool lockOn = false;
static uint32_t lockUntil = 0;

static bool ctEq(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// SHA-256(salt || password) through the PSA API of mbedtls 4 (hardware SHA).
static void hashPw(const uint8_t *s, const std::string &pw, uint8_t *out) {
  psa_crypto_init();   // does nothing when done
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  size_t n = 0;
  if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS ||
      psa_hash_update(&op, s, 16) != PSA_SUCCESS ||
      psa_hash_update(&op, (const uint8_t *)pw.data(), pw.size()) != PSA_SUCCESS ||
      psa_hash_finish(&op, out, 32, &n) != PSA_SUCCESS || n != 32) {
    psa_hash_abort(&op);
    memset(out, 0, 32);   // never equals a stored hash by accident of a failure
  }
}

static uint32_t rtcSum() {
  const uint8_t *p = (const uint8_t *)&rtc;
  uint32_t h = 2166136261u;   // FNV-1a over everything but sum
  for (size_t i = 0; i < offsetof(RtcAuth, sum); i++) h = (h ^ p[i]) * 16777619u;
  return h;
}
// Call after every change to the sessions.
static void rtcSeal() { rtc.magic = RTC_MAGIC; memcpy(rtc.pwTag, hashStored, 8); rtc.sum = rtcSum(); }

void authInit() {
  Nvs p;
  pwSet = false;
  if (p.open("auth", false)) {
    pwSet = p.blobLen("salt") == 16 && p.blobLen("hash") == 32;
    if (pwSet) {
      p.getBlob("salt", salt, 16);
      p.getBlob("hash", hashStored, 32);
    }
  }
  bool keep = pwSet && esp_reset_reason() == ESP_RST_SW && rtc.magic == RTC_MAGIC &&
              rtc.sum == rtcSum() && memcmp(rtc.pwTag, hashStored, 8) == 0;
  if (!keep) memset(&rtc, 0, sizeof(rtc));
  for (auto &s : sessions) s.sid[32] = 0;
  rtcSeal();
}

bool authPasswordSet() { return pwSet; }

static void parseSid(const std::string &cookie, char *out) {
  out[0] = 0;
  size_t i = 0, n = cookie.size();
  while (i < n) {
    while (i < n && (cookie[i] == ' ' || cookie[i] == ';')) i++;
    size_t eq = cookie.find('=', i);
    if (eq == std::string::npos) return;
    size_t end = cookie.find(';', eq);
    if (end == std::string::npos) end = n;
    if (cookie.compare(i, eq - i, "sid") == 0 && eq - i == 3) {
      std::string v = cookie.substr(eq + 1, end - eq - 1);
      if (v.size() == 32) strlcpy(out, v.c_str(), 33);
      return;
    }
    i = end + 1;
  }
}

static Session *findSession(const std::string &cookie) {
  char sid[33];
  parseSid(cookie, sid);
  if (strlen(sid) != 32) return nullptr;
  Session *hit = nullptr;
  for (auto &s : sessions)   // no early exit: constant time per candidate
    if (s.used && ctEq((const uint8_t *)s.sid, (const uint8_t *)sid, 32)) hit = &s;
  return hit;
}

bool authValidCookie(const std::string &cookie) { return findSession(cookie) != nullptr; }

static void newSession(std::string &sidOut) {
  // free slot, else drop the oldest
  Session *slot = nullptr;
  for (auto &s : sessions) if (!s.used) { slot = &s; break; }
  if (!slot) {
    slot = &sessions[0];
    for (auto &s : sessions) if (s.seq < slot->seq) slot = &s;
  }
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  for (int i = 0; i < 16; i++) snprintf(slot->sid + i * 2, 3, "%02x", r[i]);
  slot->used = true;
  slot->seq = ++seqCounter;
  sidOut = slot->sid;
  rtcSeal();
}

uint32_t authLockedSeconds() {
  if (!lockOn) return 0;
  int32_t left = (int32_t)(lockUntil - nowMs());
  if (left <= 0) { lockOn = false; return 0; }
  return (left + 999) / 1000;
}

// Shared by login and password change: counts wrong attempts.
static int checkPw(const std::string &pw) {
  if (authLockedSeconds()) return 429;
  uint8_t h[32];
  hashPw(salt, pw, h);
  if (ctEq(h, hashStored, 32)) { fails = 0; return 200; }
  if (++fails >= MAX_FAILS) { fails = 0; lockOn = true; lockUntil = nowMs() + LOCK_MS; logEvent("login locked 30 s"); }
  return 401;
}

int authLogin(const std::string &password, std::string &sid) {
  if (!pwSet) { newSession(sid); return 200; }
  int c = checkPw(password);
  if (c == 200) newSession(sid);
  return c;
}

void authLogout(const std::string &cookie) {
  Session *s = findSession(cookie);
  if (s) s->used = false;
  rtcSeal();
}

int authSetPassword(const std::string &oldPw, const std::string &newPw, const std::string &cookie,
                    std::string &sid, std::string &err) {
  if (newPw.size() < 8 || newPw.size() > 64) { err = "password must be 8..64 characters"; return 400; }
  if (pwSet) {
    int c = checkPw(oldPw);
    if (c == 429) { err = "too many attempts, wait"; return 429; }
    if (c != 200) { err = "wrong current password"; return 401; }
  }
  uint8_t s2[16], h[32];
  esp_fill_random(s2, 16);
  hashPw(s2, newPw, h);
  Nvs p;
  p.open("auth", true);
  p.setBlob("salt", s2, 16);
  p.setBlob("hash", h, 32);
  memcpy(salt, s2, 16);
  memcpy(hashStored, h, 32);
  pwSet = true;
  // Drop every session except the caller's; give the caller one if it had none.
  Session *keep = findSession(cookie);
  for (auto &s : sessions) if (&s != keep) s.used = false;
  if (keep) sid = keep->sid; else newSession(sid);
  rtcSeal();   // new password tag
  logEvent("password changed");
  return 200;
}

void authErase() {
  Nvs p;
  if (p.open("auth", true)) p.clear();
  pwSet = false;
  for (auto &s : sessions) s.used = false;
  memset(hashStored, 0, sizeof(hashStored));
  rtcSeal();
  fails = 0; lockOn = false;
}
