// SPDX-License-Identifier: GPL-3.0-or-later
#include "time_mgr.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "settings.h"
#include "buzzer.h"
#include "wifi_mgr.h"
#include "esp_timer.h"
#include "eventlog.h"
#include "util.h"

static const time_t PLAUSIBLE = 1704067200;   // 2024-01-01 UTC

// lwIP keeps the pointer it is given, so the server name needs storage that
// outlives the call (and is only rewritten while SNTP is being restarted).
static char ntpBuf[64];
static std::string tzApplied, ntpApplied;
static volatile bool sntpSynced = false;   // set from the tcpip thread: NTP confirmed this boot
static bool manualSet = false;             // set by POST /api/time
static bool quiet = false;
static bool force = true;
static uint32_t lastAt = 0;

// SNTP is not left running (the rule, spec 18 "When SNTP is used"): it is
// started for a reason, stopped after the first answer.
static const uint32_t SNTP_TIMEOUT_MS = 60000;    // non-urgent run gives up after this
static const uint32_t SNTP_RETRY_MS = 600000;     // then asks again after 10 min
static volatile bool answerPending = false;       // set from the tcpip thread
static bool running = false;
static uint32_t runStart = 0;
static bool retryPending = false;   // last run timed out: use the short wait
static uint32_t refAt = 0;          // reference for "due": last answer, or boot
static int64_t lastAnswerUs = -1;   // esp_timer time of the last answer, -1 = none

// SNTP notification: runs in the lwIP thread, only sets flags.
static void onSync(struct timeval *) { sntpSynced = true; answerPending = true; }

static bool computeQuiet();

static void applyTz() {
  setenv("TZ", cfg.tz.c_str(), 1);
  tzset();
  tzApplied = cfg.tz;
}

// The clock normally survives a restart: this SDK build uses
// CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT (sdkconfig.h, esp32c3 libs), i.e. system
// time = RTC timer + a boot-time offset kept in RTC memory, and startup calls
// esp_set_time_from_rtc() (newlib/platform_include/esp_newlib.h). RTC memory and
// the RTC timer survive software, crash and watchdog resets but not a power-on
// reset, after which time(nullptr) is 1970 and the clock counts as unknown.
// So at boot a plausible time is simply "kept"; NTP corrects it later.
void timeInit() {
  applyTz();                       // TZ first: before any local-time computation
  ntpApplied = cfg.ntp_server;
  refAt = nowMs();                // a kept clock is checked <ntp_check_h> after boot
  quiet = computeQuiet();          // quiet hours apply straight away on a kept clock
  buzzerSetQuiet(quiet);
}

// "ntp": confirmed by NTP since this boot; "browser": set by POST /api/time;
// "kept": carried over a restart, not confirmed yet; "none": unknown.
const char *timeSource() {
  if (sntpSynced) return "ntp";
  if (!timeSynced()) return "none";
  return manualSet ? "browser" : "kept";
}

static void stopSntp() {
  // esp_netif_sntp takes the lwIP core lock itself; TZ and the clock are untouched
  if (running) esp_netif_sntp_deinit();
  running = false;
}

static void startSntp() {
  if (running) stopSntp();   // server change: stop, set the name, start again
  strlcpy(ntpBuf, cfg.ntp_server.c_str(), sizeof(ntpBuf));
  ntpApplied = cfg.ntp_server;
  applyTz();
  esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(1, ESP_SNTP_SERVER_LIST(ntpBuf));
  c.wait_for_sync = false;
  c.sync_cb = onSync;
  running = esp_netif_sntp_init(&c) == ESP_OK;
  runStart = nowMs();
}

void timeLinkUp() { force = true; }

bool timeSyncNow(std::string &err) {
  if (!wifiConnected()) { err = "the station is not connected to a network"; return false; }
  if (!running) startSntp();
  return true;
}

int32_t timeLastSyncS() {
  if (lastAnswerUs < 0) return -1;
  return (int32_t)((esp_timer_get_time() - lastAnswerUs) / 1000000);
}

// Runs once a second in the main task. Interval checks use unsigned
// subtraction from a reference (valid up to 49 days; the largest wait is
// 720 h = 30 days), never an absolute due time.
static void sntpSchedule(uint32_t now) {
  if (answerPending) {
    answerPending = false;
    lastAnswerUs = esp_timer_get_time();
    refAt = now;
    retryPending = false;
    if (running) stopSntp();
    logEvent("clock: NTP answer");
  }
  const char *src = timeSource();
  bool urgent = strcmp(src, "none") == 0 || strcmp(src, "browser") == 0;
  bool up = wifiConnected();
  if (running) {
    // Urgent runs continue until the first answer. Others give up after 60 s.
    if (!urgent && now - runStart >= SNTP_TIMEOUT_MS) {
      stopSntp();
      retryPending = true;
      refAt = now;
    }
    return;
  }
  if (!up) return;
  if (urgent) { startSntp(); return; }
  uint32_t wait = retryPending ? SNTP_RETRY_MS : (uint32_t)cfg.ntp_check_h * 3600000UL;
  if (wait == 0) return;   // ntp_check_h = 0: never again
  if (now - refAt >= wait) startSntp();
}

bool timeSynced() { return time(nullptr) >= PLAUSIBLE; }
uint32_t timeEpoch() { return (uint32_t)time(nullptr); }

void timeLocalString(char *out, size_t n) {
  out[0] = 0;
  time_t t = time(nullptr);
  if (t < PLAUSIBLE) return;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

static bool computeQuiet() {
  if (!cfg.quiet_on || cfg.quiet_from == cfg.quiet_to) return false;
  time_t t = time(nullptr);
  if (t < PLAUSIBLE) return false;   // not synced: the buzzer sounds
  struct tm tmv;
  localtime_r(&t, &tmv);
  int m = tmv.tm_hour * 60 + tmv.tm_min;
  if (cfg.quiet_from < cfg.quiet_to) return m >= cfg.quiet_from && m < cfg.quiet_to;
  return m >= cfg.quiet_from || m < cfg.quiet_to;   // crosses midnight
}

bool timeQuietNow() { return quiet; }
void timeRefresh() { force = true; }

void timeUpdate() {
  uint32_t now = nowMs();
  if (!force && now - lastAt < 1000) return;
  force = false;
  lastAt = now;
  // Zone change: live, SNTP untouched.
  if (cfg.tz != tzApplied) applyTz();
  // Server change: restart a running sync with the new name; otherwise it
  // just applies to the next sync.
  if (cfg.ntp_server != ntpApplied) {
    if (running) startSntp(); else ntpApplied = cfg.ntp_server;
  }
  sntpSchedule(now);
  quiet = computeQuiet();
  buzzerSetQuiet(quiet);
}

bool timeSetManual(uint32_t epoch) {
  if (sntpSynced) return false;   // SNTP wins; later syncs still count either way
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  manualSet = true;
  force = true;
  return true;
}
