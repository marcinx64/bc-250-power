// SPDX-License-Identifier: GPL-3.0-or-later
// Automatic OS updates (osupd.h, spec section 26).
#include "osupd.h"
#include "ssh.h"
#include "power.h"
#include "settings.h"
#include "time_mgr.h"
#include "wifi_mgr.h"
#include "web.h"
#include "eventlog.h"
#include "util.h"
#include <algorithm>
#include <string.h>
#include <time.h>

// File-local types are in unnamed namespaces (settings.h has a struct
// Settings; two definitions of one name corrupt the heap).
namespace {
struct Cfg {
  bool on = false;
  uint32_t from = 180, len = 120, days = 127;
  bool reboot = false;   // after == "reboot"
};

enum Phase : uint8_t { P_IDLE, P_POWER_ON, P_SESSION, P_AFTER, P_WAIT_OFF, P_REBOOT_WAIT };

struct Job {
  Phase ph = P_IDLE;
  char kind = 0;                 // 'n' night, 'm' manual, 't' test
  uint32_t startMs = 0, phaseMs = 0, endMs = 0;   // endMs: end of the window (night), else 0
  uint32_t actionsBase = 0;
  uint8_t tries = 0;             // shutdown attempts so far
  bool poweredOn = false;        // this job started the PC
  bool silent = false;
  bool loggedIn = false;         // the key worked in this job
  bool takenOver = false;
  bool failed = false, stopped = false, shutReq = false, afterReboot = false;
  std::string error, at;
  int exitStatus = -1;
  uint32_t outBytes = 0;
  bool rebootNeeded = false;
  std::string tail;
};

struct Last {
  bool valid = false;
  char kind = 0;
  std::string at, result, error, end, tail;
  int exitStatus = -1;
  uint32_t durationS = 0, outBytes = 0;
  bool rebootNeeded = false;
};
}  // namespace

static const char *NS = "osupd";
static const uint32_t GRACE_MIN = 5;                 // a night job may start this long after the window start
static const uint32_t CONNECT_MAX_MS = 600000;       // wait for the PC to boot
static const uint32_t END_RESERVE_MS = 300000;       // the session ends this long before the window end
static const uint32_t MANUAL_LIMIT_MS = 7200000;
static const uint32_t TEST_LIMIT_MS = 60000;
static const uint32_t POWEROFF_CONNECT_MS = 30000;
static const uint32_t POWEROFF_LIMIT_MS = 60000;
static const uint32_t REBOOT_WAIT_MS = 30000;
static const uint32_t SILENT_TAIL_MS = 2000;         // keeps the silence over the last beep

static Cfg c;
static Job job;
static Last last;
static uint32_t lastKey = 0;       // window day of the last night job (persisted)
static uint32_t lastTickAt = 0;
static bool silentHold = false;    // job over, silence still held for the last beep
static uint32_t silentOffAt = 0;

// ---- calendar ---------------------------------------------------------------

// Days since 1970-01-01 of a civil date, and back (proleptic Gregorian).
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = (int)(z - era * 146097);
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = (int)(yy + (m <= 2));
}

static int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Local time as seconds since 1970-01-01 00:00 local (the zone's offset applied).
static int64_t localSecs() {
  time_t t = time(nullptr);
  struct tm m;
  localtime_r(&t, &m);
  return daysFromCivil(m.tm_year + 1900, m.tm_mon + 1, m.tm_mday) * 86400 + m.tm_hour * 3600 + m.tm_min * 60 + m.tm_sec;
}

static std::string fmtLocal(int64_t ls) {
  int y, mo, d;
  civilFromDays(floorDiv(ls, 86400), y, mo, d);
  int sod = (int)(ls - floorDiv(ls, 86400) * 86400);
  return strPrintf("%04d-%02d-%02d %02d:%02d", y, mo, d, sod / 3600, (sod / 60) % 60);
}

// Window day: the local day on which the window that contains `ls` started.
static int64_t windowDay(int64_t ls) { return floorDiv(ls - (int64_t)c.from * 60, 86400); }
static bool dayOn(int64_t wday) { return (c.days >> (int)((wday + 4) % 7)) & 1; }   // 1970-01-01 was a Thursday

static bool configured() { return !sshHost().empty() && !sshUser().empty(); }

// ---- settings -----------------------------------------------------------------

static void save() {
  Nvs n;
  if (!n.open(NS, true)) return;
  n.setBool("on", c.on);
  n.setU32("from", c.from);
  n.setU32("len", c.len);
  n.setU32("days", c.days);
  n.setStr("after", c.reboot ? "reboot" : "off");
}

static void saveLastKey() {
  Nvs n;
  if (n.open(NS, true)) n.setU32("lk", lastKey);
}

void osupdInit() {
  Nvs n;
  if (!n.open(NS, false)) return;
  c.on = n.getBool("on", false);
  c.from = std::min<uint32_t>(n.getU32("from", 180), 1439);
  c.len = std::min<uint32_t>(std::max<uint32_t>(n.getU32("len", 120), 15), 120);
  c.days = n.getU32("days", 127) & 127;
  if (!c.days) c.days = 127;
  c.reboot = n.getStr("after", "off") == "reboot";
  lastKey = n.getU32("lk", 0);
}

bool osupdEnabled() { return c.on; }
bool osupdBusy() { return job.ph != P_IDLE || sshRunning(); }

std::string osupdControllerAddress() { return wifiStaIp(); }   // the web server is on port 80

// ---- jobs -------------------------------------------------------------------

static const char *kindName(char k) { return k == 'n' ? "night" : k == 'm' ? "manual" : k == 't' ? "test" : ""; }

static const char *stateName() {
  switch (job.ph) {
    case P_IDLE: return "idle";
    case P_POWER_ON: return "starting";
    case P_SESSION: return (job.kind == 't' || sshLoggedIn()) ? "updating" : "waiting";
    case P_AFTER: return job.afterReboot ? "rebooting" : "shutting_down";
    case P_WAIT_OFF: return "shutting_down";
    case P_REBOOT_WAIT: return "rebooting";
  }
  return "idle";
}

static void releaseSilent(uint32_t now) {
  if (job.silent && !job.takenOver) { silentHold = true; silentOffAt = now + SILENT_TAIL_MS; }
  job.silent = false;
}

static void recordSkipped(char kind, const char *why) {
  last = Last();
  last.valid = true;
  last.kind = kind;
  char t[24];
  timeLocalString(t, sizeof t);
  last.at = std::string(t, strnlen(t, 16));
  last.result = "skipped";
  last.error = why;
  last.end = powerStateSync() == PWR_OFF ? "off" : "on";
  logEvent("os update: skipped, %.30s", why);
}

static void finish(uint32_t now) {
  PowerState st = powerStateSync();
  last = Last();
  last.valid = true;
  last.kind = job.kind;
  last.at = job.at;
  last.result = job.stopped ? "stopped" : job.failed ? "failed" : "ok";
  last.error = job.error;
  if (job.takenOver) last.error += last.error.empty() ? "taken over" : "; taken over";
  last.exitStatus = job.exitStatus;
  last.durationS = (now - job.startMs) / 1000;
  last.rebootNeeded = job.rebootNeeded;
  last.outBytes = job.outBytes;
  last.end = st == PWR_OFF ? "off" : "on";
  last.tail = job.tail;
  logEvent("os update: %s %s, PC %s", kindName(job.kind), last.result.c_str(), last.end.c_str());
  releaseSilent(now);
  job = Job();
}

static bool startSession(const char *mode, uint32_t connectMs, uint32_t limitMs) {
  std::string err;
  if (sshStart(mode, connectMs, limitMs, err)) return true;
  if (job.error.empty()) job.error = err;
  return false;
}

// The PC has to go off (and was started by this job, nobody else is using it).
static void beginShutdown(uint32_t now) {
  PowerState st = powerStateSync();
  if (!job.poweredOn || job.takenOver || st == PWR_OFF) { finish(now); return; }
  job.phaseMs = now;
  job.tries = 1;
  job.shutReq = false;
  if (job.loggedIn && st == PWR_ON && startSession("poweroff", POWEROFF_CONNECT_MS, POWEROFF_LIMIT_MS)) {
    job.afterReboot = false;
    job.ph = P_AFTER;
    return;
  }
  job.ph = P_WAIT_OFF;   // the power button, once the PC is on
}

static void failJob(uint32_t now, const std::string &why) {
  job.failed = true;
  if (job.error.empty() && !job.stopped) job.error = why;
  logEvent("os update: failed: %.40s", why.c_str());
  beginShutdown(now);
}

static void startMain(uint32_t now) {
  const char *mode = job.kind == 't' ? "check" : "update";
  uint32_t connect = job.kind == 't' ? 0 : CONNECT_MAX_MS, limit = job.kind == 't' ? TEST_LIMIT_MS : MANUAL_LIMIT_MS;
  if (job.kind == 'n') {
    uint32_t left = job.endMs - now;
    limit = left > END_RESERVE_MS + 30000 ? left - END_RESERVE_MS : 30000;
    connect = std::min(connect, limit);
  }
  job.ph = P_SESSION;
  job.phaseMs = now;
  if (!startSession(mode, connect, limit)) failJob(now, job.error);
}

static void beginJob(char kind, bool powerOn, uint32_t now) {
  job = Job();
  job.kind = kind;
  job.startMs = job.phaseMs = now;
  char t[24];
  timeLocalString(t, sizeof t);
  job.at = std::string(t, strnlen(t, 16));
  job.silent = powerOn;
  logEvent("os update: %s start", kindName(kind));
  if (powerOn) {
    powerSetSilent(true);
    silentHold = false;
  }
  job.actionsBase = powerUserActions();
  if (!powerOn) { startMain(now); return; }
  std::string err;
  job.poweredOn = true;
  job.ph = P_POWER_ON;
  if (!powerJobAction("on", err)) { job.poweredOn = false; failJob(now, err); }
}

static bool canStart(std::string &err, bool needOn) {
  if (!c.on) { err = "automatic updates are off"; return false; }
  if (!configured()) { err = "set the BC-250 address and user first"; return false; }
  if (osupdBusy()) { err = "a job is running"; return false; }
  if (!wifiConnected()) { err = "not connected to WiFi"; return false; }
  if (webOtaRunning()) { err = "a firmware update is running"; return false; }
  PowerState st = powerStateSync();
  if (st == PWR_STARTING || st == PWR_SHUTTING_DOWN) { err = "the BC-250 is starting or shutting down"; return false; }
  if (needOn && st != PWR_ON) { err = "the BC-250 is not on"; return false; }
  return true;
}

bool osupdRun(std::string &err) {
  if (!canStart(err, false)) return false;
  beginJob('m', powerStateSync() == PWR_OFF, nowMs());
  return true;
}

bool osupdTest(std::string &err) {
  if (!canStart(err, true)) return false;
  beginJob('t', false, nowMs());
  return true;
}

void osupdStop() {
  if (job.ph == P_IDLE) return;
  uint32_t now = nowMs();
  job.stopped = true;
  logEvent("os update: stopped");
  switch (job.ph) {
    case P_SESSION: sshStop(); break;               // the tick sees the session end
    case P_POWER_ON: job.failed = true; beginShutdown(now); break;
    case P_REBOOT_WAIT: finish(now); break;
    default: break;                                 // shutting down: carry on
  }
}

bool osupdForgetHost(std::string &err) {
  if (osupdBusy()) { err = "a job is running"; return false; }
  sshForgetHost();
  return true;
}

static void onMainDone(uint32_t now) {
  const SshOutcome &o = sshOutcome();
  job.loggedIn = job.loggedIn || o.loggedIn;
  job.exitStatus = o.exitStatus;
  job.outBytes = o.outBytes;
  job.rebootNeeded = o.rebootNeeded;
  job.tail = o.tail;
  if (job.stopped) { job.failed = true; beginShutdown(now); return; }
  std::string why;
  if (!o.ok) why = o.error;
  else if (o.exitStatus != 0) why = o.exitStatus < 0 ? "no exit status" : strPrintf("exit status %d", o.exitStatus);
  if (!why.empty()) {
    failJob(now, why);
    return;
  }
  if (job.kind == 't' || !job.poweredOn || job.takenOver) { finish(now); return; }
  job.afterReboot = c.reboot;
  job.phaseMs = now;
  if (startSession(c.reboot ? "reboot" : "poweroff", POWEROFF_CONNECT_MS, POWEROFF_LIMIT_MS)) job.ph = P_AFTER;
  else failJob(now, job.error);
}

static void onAfterDone(uint32_t now) {
  const SshOutcome &o = sshOutcome();
  if (job.takenOver) { finish(now); return; }
  if (job.afterReboot) {
    if (o.ok) { job.ph = P_REBOOT_WAIT; job.phaseMs = now; return; }
    job.failed = true;
    if (job.error.empty()) job.error = "reboot: " + o.error;
    finish(now);
    return;
  }
  if (!o.ok) logEvent("os update: poweroff over SSH failed");
  job.ph = P_WAIT_OFF;   // the button follows if it did not work
  job.phaseMs = now;
  job.tries = 1;
  job.shutReq = o.ok;    // SSH poweroff is the first attempt
}

static void stepJob(uint32_t now) {
  PowerState st = powerStateSync();
  // A person took over: silence and LED override end, the PC stays as it is.
  if (job.kind != 't' && (job.poweredOn || job.ph == P_POWER_ON) && !job.takenOver &&
      powerUserActions() != job.actionsBase) {
    job.takenOver = true;
    powerSetSilent(false);
    logEvent("os update: taken over");
  }
  bool windowEnd = job.kind == 'n' && (int32_t)(now - job.endMs) >= 0;
  switch (job.ph) {
    case P_POWER_ON:
      if (st == PWR_ON) startMain(now);
      else if (windowEnd) failJob(now, "window end reached");
      else if (st == PWR_OFF && now - job.phaseMs > 3000) {
        job.poweredOn = false;   // it never started: nothing to shut down
        failJob(now, "the BC-250 did not start");
      } else if (now - job.phaseMs > (uint32_t)cfg.start_timeout_ms + 15000) failJob(now, "the BC-250 did not start");
      break;
    case P_SESSION:
      if (!sshRunning() && sshOutcome().valid) onMainDone(now);
      else if (windowEnd && !job.stopped && !job.failed) {
        job.failed = true;
        job.error = "window end reached";
        sshStop();
      }
      break;
    case P_AFTER:
      if (!sshRunning() && sshOutcome().valid) onAfterDone(now);
      break;
    case P_WAIT_OFF:
      if (st == PWR_OFF || job.takenOver) {   // taken over: the PC is left to the user
        finish(now);
        break;
      }
      if (st == PWR_ON && !job.shutReq) {
        std::string err;
        powerJobAction("shutdown", err);
        job.shutReq = true;
        job.phaseMs = now;
      }
      if (now - job.phaseMs >= (uint32_t)cfg.shutdown_timeout_ms + 5000) {
        if (job.tries < 2) {   // once more with the power button
          job.tries = 2;
          job.phaseMs = now;
          std::string err;
          if (st == PWR_ON) powerJobAction("shutdown", err);
          logEvent("os update: shutdown retry");
        } else {
          job.failed = true;
          if (job.error.empty()) job.error = "the BC-250 did not shut down";
          else job.error += "; the BC-250 did not shut down";
          logEvent("os update: BC-250 still on, giving up");
          finish(now);
        }
      }
      break;
    case P_REBOOT_WAIT:
      if (now - job.phaseMs >= REBOOT_WAIT_MS) finish(now);
      break;
    case P_IDLE: break;
  }
}

// ---- schedule -----------------------------------------------------------------

static void checkNight(uint32_t now) {
  if (!timeSynced() || !configured() || !c.days) return;
  int64_t ls = localSecs();
  int64_t wd = windowDay(ls);
  int64_t into = ls - (wd * 86400 + (int64_t)c.from * 60);   // seconds since the window start
  if (!dayOn(wd) || into >= (int64_t)GRACE_MIN * 60 || (uint32_t)wd == lastKey) return;
  PowerState st = powerStateSync();
  const char *why = nullptr;
  bool retry = false;   // may clear itself: try again until a minute before the grace ends
  // Never linked, or the key was refused / the host key changed: powering the
  // PC on would only end in a shutdown. Night runs wait for a working test.
  if (!sshLinked()) why = "not set up: test the connection first";
  else if (st != PWR_OFF) why = "the BC-250 was on";
  else if (!wifiConnected()) { why = "not connected to WiFi"; retry = true; }
  else if (webOtaRunning()) { why = "a firmware update is running"; retry = true; }
  if (retry && into < (int64_t)(GRACE_MIN - 1) * 60) return;
  lastKey = (uint32_t)wd;
  saveLastKey();
  if (why) { recordSkipped('n', why); return; }
  beginJob('n', true, now);
  job.endMs = now + (uint32_t)((int64_t)c.len * 60 - into) * 1000u;
}

void osupdUpdate() {
  uint32_t now = nowMs();
  if (now - lastTickAt < 1000) return;
  lastTickAt = now;
  if (silentHold && (int32_t)(now - silentOffAt) >= 0) {
    silentHold = false;
    if (job.ph == P_IDLE) powerSetSilent(false);
  }
  if (job.ph != P_IDLE) stepJob(now);
  else if (c.on) checkNight(now);
}

// ---- API ------------------------------------------------------------------------

static std::string nextRun() {
  if (!c.on || !timeSynced() || !configured() || !c.days || !sshLinked()) return std::string();
  int64_t ls = localSecs();
  int64_t wd = windowDay(ls);
  for (int64_t k = wd; k <= wd + 8; k++) {
    int64_t start = k * 86400 + (int64_t)c.from * 60;
    if (!dayOn(k) || (uint32_t)k == lastKey) continue;
    if (start + (int64_t)GRACE_MIN * 60 <= ls) continue;
    return fmtLocal(start);
  }
  return std::string();
}

void osupdToJson(JsonObject o) {
  o["enabled"] = c.on;
  o["host"] = sshHost();
  o["port"] = sshPort();
  o["user"] = sshUser();
  o["from"] = c.from;
  o["len"] = c.len;
  o["days"] = c.days;
  o["after"] = c.reboot ? "reboot" : "off";
  std::string key, fp;
  if (c.on && sshKeyInfo(key, fp)) {
    o["pubkey"] = key;
    o["fingerprint"] = fp;
  } else {
    o["pubkey"] = "";
    o["fingerprint"] = "";
  }
  o["host_key"] = sshHostKeyFingerprint();
  o["linked"] = sshLinked();
  std::string ctl = osupdControllerAddress();
  o["setup_cmd"] = ctl.empty() ? std::string() : "curl -fsS http://" + ctl + "/bc250-setup.sh | sudo sh";
  o["clock"] = timeSynced();
  o["next"] = nextRun();
  JsonObject j = o["job"].to<JsonObject>();
  j["state"] = stateName();
  j["kind"] = kindName(job.kind);
  j["since_s"] = job.ph == P_IDLE ? 0 : (nowMs() - job.startMs) / 1000;
  if (last.valid) {
    JsonObject l = o["last"].to<JsonObject>();
    l["kind"] = kindName(last.kind);
    l["at"] = last.at;
    l["result"] = last.result;
    l["error"] = last.error;
    l["exit"] = last.exitStatus;
    l["duration_s"] = last.durationS;
    l["reboot_needed"] = last.rebootNeeded;
    l["end"] = last.end;
    l["out_bytes"] = last.outBytes;
    l["tail"] = last.tail;
  }
  JsonObject m = o["mem"].to<JsonObject>();
  sshMemToJson(m);
}

static bool validHost(const std::string &s) {
  if (s.empty() || s.size() > 63) return false;
  for (unsigned char ch : s) if (ch <= ' ' || ch == 0x7f) return false;
  return true;
}

int osupdConfigure(JsonObjectConst o, std::string &err) {
  bool onlyDisable = o.size() == 1 && o["enabled"].is<bool>() && !o["enabled"].as<bool>();
  if (!onlyDisable && osupdBusy()) { err = "a job is running"; return 409; }
  Cfg n = c;
  std::string host = sshHost(), user = sshUser();
  uint32_t port = sshPort();
  bool setTarget = false;
  auto num = [&](const char *k, uint32_t lo, uint32_t hi, uint32_t &out) {
    JsonVariantConst v = o[k];
    if (v.isNull()) return true;
    if (!v.is<uint32_t>() || v.as<uint32_t>() < lo || v.as<uint32_t>() > hi) {
      err = strPrintf("%s must be %u..%u", k, (unsigned)lo, (unsigned)hi);
      return false;
    }
    out = v.as<uint32_t>();
    return true;
  };
  if (!o["enabled"].isNull()) {
    if (!o["enabled"].is<bool>()) { err = "enabled must be true or false"; return 400; }
    n.on = o["enabled"].as<bool>();
  }
  if (!o["host"].isNull()) {
    if (!o["host"].is<const char *>() || !validHost(o["host"].as<const char *>())) { err = "host must be 1..63 characters"; return 400; }
    host = o["host"].as<const char *>();
    setTarget = true;
  }
  if (!o["user"].isNull()) {
    if (!o["user"].is<const char *>() || !validHost(o["user"].as<const char *>()) || strlen(o["user"].as<const char *>()) > 32) {
      err = "user must be 1..32 characters";
      return 400;
    }
    user = o["user"].as<const char *>();
    setTarget = true;
  }
  if (!num("port", 1, 65535, port) || !num("from", 0, 1439, n.from) || !num("len", 15, 120, n.len) ||
      !num("days", 1, 127, n.days)) return 400;
  if (!o["port"].isNull()) setTarget = true;
  if (!o["after"].isNull()) {
    const char *a = o["after"].is<const char *>() ? o["after"].as<const char *>() : "";
    if (strcmp(a, "off") && strcmp(a, "reboot")) { err = "after must be off or reboot"; return 400; }
    n.reboot = !strcmp(a, "reboot");
  }
  if (onlyDisable) osupdStop();
  if (setTarget && !sshSetTarget(host, port, user, err)) return 409;
  c = n;
  save();
  return 0;
}
