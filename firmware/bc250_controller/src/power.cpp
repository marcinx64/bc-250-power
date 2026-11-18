// SPDX-License-Identifier: GPL-3.0-or-later
// Power state machine and front button. Spec section 2.
// Runs every loop iteration and depends on nothing but nowMs() and GPIO,
// so WiFi, web and BLE can never stall it. All time comparisons use
// unsigned subtraction or (int32_t) differences, so nowMs() wrap is safe.
#include "power.h"
#include "pins.h"
#include "settings.h"
#include "buzzer.h"
#include "eventlog.h"
#include "util.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Control lock: a FreeRTOS mutex (priority inheritance) guarding everything
// in this file plus the buzzer and LED state. The control task holds it for
// its whole 5 ms tick; every other task takes it only for short, non-blocking
// work (no NVS, no network, no waiting). Order: control lock -> log lock.
static SemaphoreHandle_t ctl = nullptr;
void ctlLock() { xSemaphoreTake(ctl, portMAX_DELAY); }
void ctlUnlock() { xSemaphoreGive(ctl); }

static const uint32_t PULSE_MS = 500;
static const uint32_t BTN_DEBOUNCE_MS = 30;
static const uint32_t SENSE_LIT_MS = 200;
// Running-state hold thresholds are settings (section 21), read fresh each use.
static inline uint32_t holdResetMs() { return (uint32_t)cfg.hold_reset_s * 1000UL; }
static inline uint32_t holdForceMs() { return (uint32_t)cfg.hold_force_s * 1000UL; }
// Forgotten-password gesture at power-up: fixed, independent of the settings.
static const uint32_t BOOT_FACTORY_RESET_HOLD_MS = 10000;
static const uint32_t HOLD_FLASH_AFTER_MS = 1000;
static const uint32_t ERROR_BLINK_MS = 3000;

static PowerState state = PWR_OFF;
static uint32_t stateSince = 0;
static bool locked = false;
static bool pson = false;

// PWR / RST pulses: always end after PULSE_MS whatever the state does.
static bool pwrPulse = false, rstPulse = false;
static uint32_t pwrEndAt = 0, rstEndAt = 0;
// The UI polls about once a second and a pulse lasts 500 ms, so the API
// keeps reporting a pulse for a while after it ended. Display only: the
// pins and the state machine use pwrPulse / rstPulse.
static const uint32_t PULSE_SHOW_MS = 2500;
static bool pwrShow = false, rstShow = false;
static uint32_t pwrShowUntil = 0, rstShowUntil = 0;

// Error signal (LED blink window)
static bool errorOn = false;
static uint32_t errorUntil = 0;

// SENSE debounce
static bool senseRawPrev = false;
static uint32_t senseRawAt = 0;
static bool litDeb = false;

// Button
static bool btnRawPrev = false;
static uint32_t btnRawAt = 0;
static bool btnDeb = false;
static uint32_t btnSince = 0;
// What the current press may still do. Cleared when the state changes under
// a held button, so a press never acts in a state it did not start in.
static bool pressActs = false;   // 5 s cue, release actions (short press / reset)
static bool pressForce = false;  // 10 s forced power off
static bool cueDone = false;

// Debug sense source (section 13). RAM only, "real" after every boot.
static SenseMode senseMode = SENSE_REAL;
static bool simLit = false;
static bool simLitPending = false;
static uint32_t simLitAt = 0;
static bool simDarkPending = false;
static uint32_t simDarkAt = 0;
static bool simRelitPending = false;
static uint32_t simRelitAt = 0;
static bool simPsonPrev = false, simPwrPrev = false, simRstPrev = false;

// Forgotten-password gesture: button already down at boot, held 10 s.
static bool bootHeld = false;
static uint32_t bootAt = 0;
static bool factoryResetFlag = false;
static bool confirmOn = false;      // gesture confirmed: LED flashes 5 times
static uint32_t confirmAt = 0;

static bool pwrPulseDone = false;  // start sequence: PWR pulse fired

// Section 17: keep PS_ON through a deliberate restart. The marker lives in
// RTC memory (survives a software reset) and is trusted once, only after
// ESP_RST_SW. The pad latch is enabled only inside powerPrepareRestart().
static const uint32_t PC_ON_MAGIC = 0x50434F4E;   // "PCON"
static RTC_NOINIT_ATTR uint32_t rtcPcOn;
static bool resumedBoot = false;
static ResumeResult resumeRes = RESUME_NONE;
static bool frozen = false;   // restart is imminent: state machine stands still

// Section 25: when the BC-250 started running (state `on` or `shutting_down`)
// or last stopped. 64-bit milliseconds from esp_timer, so a long uptime never
// wraps. Only the starting -> on and running -> off edges change them: a
// failed start, a reset pulse or a shutdown that times out leave them alone.
static int64_t runSince = 0;     // meaningful while running
static int64_t stopSince = 0;    // meaningful while not running
static bool stopAtStart = true;  // not run since the controller was powered up: stopSince is that power-up
static inline int64_t ms64() { return esp_timer_get_time() / 1000; }
static inline bool isRunning(PowerState s) { return s == PWR_ON || s == PWR_SHUTTING_DOWN; }

// The same in RTC memory, which keeps its contents through every reset but a
// power-up: running or not, and how long so far. Rewritten on every edge and
// once a minute by the control task (plain memory writes, never blocks), and
// right before a deliberate restart, so any restart of the controller (web,
// update, crash, watchdog, brown-out) takes it over. A deliberate restart
// loses only the restart itself; another reset during an off period also up
// to the last minute (minute precision is all this needs). Two slots, written
// in turn, each with a sequence number and a check value: a reset in the
// middle of a write leaves the other slot whole.
struct SinceSlot { uint32_t magic, seq, running, atStart; uint64_t elapsedMs; uint32_t check; };
static const uint32_t SINCE_MAGIC = 0x534E4345;   // "SNCE"
static const uint32_t SINCE_SAVE_MS = 60000;
static RTC_NOINIT_ATTR SinceSlot rtcSince[2];
static uint32_t sinceSeq = 0;
static uint32_t sinceSavedAt = 0;   // nowMs() of the last write
static uint32_t sinceCheck(const SinceSlot &s) {
  const uint8_t *p = (const uint8_t *)&s;
  uint32_t h = 2166136261u;   // FNV-1a over everything before check (no padding there)
  for (size_t i = 0; i < offsetof(SinceSlot, check); i++) h = (h ^ p[i]) * 16777619u;
  return h;
}
static bool sinceValid(const SinceSlot &s) { return s.magic == SINCE_MAGIC && s.check == sinceCheck(s); }
static void sinceSave() {   // [lock held]
  bool running = isRunning(state);
  SinceSlot &s = rtcSince[++sinceSeq & 1];
  s.magic = SINCE_MAGIC;
  s.seq = sinceSeq;
  s.running = running;
  s.atStart = !running && stopAtStart;
  s.elapsedMs = (uint64_t)(ms64() - (running ? runSince : stopSince));
  s.check = sinceCheck(s);
  sinceSavedAt = nowMs();
}

// Automatic OS updates (section 26). Both are guarded by the control lock;
// silent is read by the buzzer and LED inside the tick.
static uint32_t userActions = 0;
static bool silent = false;
// A person did something: counted, and the silent mode ends at once.
static void userAction() { userActions++; silent = false; }

static bool realLit();
static const char *NAMES[] = {"off", "starting", "on", "shutting_down"};

static void setPson(bool on) {
  pson = on;
  pinWrite(PIN_PSON, on ? 1 : 0);
}

static void setState(PowerState s) {
  bool wasRunning = isRunning(state), willRun = isRunning(s);
  if (!wasRunning && willRun) runSince = ms64();
  if (wasRunning && !willRun) { stopSince = ms64(); stopAtStart = false; }
  if (wasRunning && !willRun && resumeRes == RESUME_PENDING) resumeRes = RESUME_LOST;
  state = s;
  stateSince = nowMs();
  pressActs = false;
  if (s == PWR_OFF) pressForce = false;
  if (wasRunning != willRun) sinceSave();
}

static void pulsePwr() {
  if (pwrPulse) return;
  pwrPulse = true;
  pwrEndAt = nowMs() + PULSE_MS;
  pwrShow = true; pwrShowUntil = pwrEndAt + PULSE_SHOW_MS;
  pinWrite(PIN_PWR, 1);
}

static void pulseRst() {
  if (rstPulse) return;
  rstPulse = true;
  rstEndAt = nowMs() + PULSE_MS;
  rstShow = true; rstShowUntil = rstEndAt + PULSE_SHOW_MS;
  pinWrite(PIN_RST, 1);
}

static void startPc(const char *reason) {
  beeps(1);
  setPson(true);
  // Old debounced "lit" must not count: demand a fresh 200 ms of low.
  litDeb = false;
  senseRawAt = nowMs();
  pwrPulseDone = false;
  setState(PWR_STARTING);
  logEvent("power on: %s", reason);
}

static void forceOff(const char *reason) {
  setPson(false);
  beeps(3);
  setState(PWR_OFF);
  logEvent("forced off: %s", reason);
}

static void shutdownRequest(const char *reason) {
  beeps(1);
  pulsePwr();
  setState(PWR_SHUTTING_DOWN);
  logEvent("shutdown: %s", reason);
}

static void resetRequest(const char *reason) {
  pulseRst();
  logEvent("reset: %s", reason);
}

// Very first thing in app_main, before any pin is configured. Returns true on
// a resume: PSON is then already driven HIGH by us and must not be touched
// by the normal all-LOW block. Sequence on a resume: output HIGH is set up
// while the pad is still latched, only then is the latch released, so the
// pin never leaves HIGH.
bool powerBootEarly() {
  bool resume = (esp_reset_reason() == ESP_RST_SW && rtcPcOn == PC_ON_MAGIC);
  rtcPcOn = 0;   // trusted once
  if (resume) {
    pinOutput(PIN_PSON, 1);
  }
  // Harmless when nothing is latched. After this the pad is always
  // controllable by pinWrite.
  gpio_hold_dis((gpio_num_t)PIN_PSON);
  resumedBoot = resume;
  return resume;
}

bool powerResumed() { return resumedBoot; }
ResumeResult powerResumeResult() { ctlLock(); ResumeResult r = resumeRes; ctlUnlock(); return r; }
const char *resumeResultName(ResumeResult r) {
  static const char *n[] = {"none", "pending", "kept", "lost"};
  return n[r];
}

void powerInit() {
  ctl = xSemaphoreCreateMutex();  // before any task can call in
  uint32_t now = nowMs();
  state = PWR_OFF;
  stateSince = now;
  senseRawPrev = realLit();
  senseRawAt = now;
  btnRawPrev = btnDeb = pinRead(PIN_SWITCH) == 0;
  btnRawAt = btnSince = now;
  // A button held at boot does nothing (pressActs/pressForce are false);
  // it only counts toward the factory-reset gesture below.
  bootHeld = btnDeb;
  bootAt = now;
  if (resumedBoot) {
    // The PC is running and PSON is already HIGH. Start in `on` with the
    // sense treated as lit; the debouncer then turns a really dark sense into
    // `off` after off_detect_ms as usual. No beep. A button held now is
    // ignored until released and does not arm the factory-reset gesture.
    pson = true;
    litDeb = true;
    senseRawAt = now;
    pwrPulseDone = true;
    bootHeld = false;
    state = PWR_ON;
    stateSince = now;
    resumeRes = RESUME_PENDING;
  }
  // Section 25: take the times over from RTC memory. Without a record, or
  // after a power-up or an external reset (how long the power was gone is
  // unknown), the BC-250 counts as off since this start, or on since now on
  // a resume.
  int64_t now64 = ms64();
  runSince = stopSince = now64;
  stopAtStart = !resumedBoot;
  const SinceSlot *r = nullptr;
  for (const SinceSlot &s : rtcSince)
    if (sinceValid(s) && (!r || (int32_t)(s.seq - r->seq) > 0)) r = &s;
  esp_reset_reason_t why = esp_reset_reason();
  bool trust = why != ESP_RST_POWERON && why != ESP_RST_EXT && why != ESP_RST_UNKNOWN && why != ESP_RST_DEEPSLEEP;
  if (r && trust) {
    sinceSeq = r->seq;
    if (resumedBoot) {
      if (r->running) runSince = now64 - (int64_t)r->elapsedMs;
    } else if (r->running) {
      stopAtStart = false;   // it was running: the reset dropped PSON, so it stopped just now
    } else {
      stopSince = now64 - (int64_t)r->elapsedMs;
      stopAtStart = r->atStart != 0;
    }
  }
  sinceSave();
}

static bool realLit() { return pinRead(PIN_SENSE) == 0; }

// Simulated PC for bench work: lit 3 s after PSON, PWR pulse while lit ->
// dark 5 s later, RST pulse -> dark for 1 s, PSON low -> dark at once.
static void simUpdate(uint32_t now) {
  if (pson && !simPsonPrev) { simLitPending = true; simLitAt = now + 3000; }
  if (!pson) {
    simLit = false; simLitPending = simDarkPending = simRelitPending = false;
  }
  if (pwrPulse && !simPwrPrev && simLit && !simDarkPending) { simDarkPending = true; simDarkAt = now + 5000; }
  if (rstPulse && !simRstPrev && simLit) {
    simLit = false; simDarkPending = false;
    simRelitPending = true; simRelitAt = now + 1000;
  }
  simPsonPrev = pson; simPwrPrev = pwrPulse; simRstPrev = rstPulse;
  if (simLitPending && (int32_t)(now - simLitAt) >= 0) { simLitPending = false; simLit = pson; }
  if (simDarkPending && (int32_t)(now - simDarkAt) >= 0) { simDarkPending = false; simLit = false; }
  if (simRelitPending && (int32_t)(now - simRelitAt) >= 0) { simRelitPending = false; simLit = pson; }
}

static bool senseInput(uint32_t now) {
  switch (senseMode) {
    case SENSE_LIT: return true;
    case SENSE_DARK: return false;
    case SENSE_SIM: simUpdate(now); return simLit;
    default: return realLit();
  }
}

static void onPress(uint32_t now) {
  userAction();
  btnSince = now;
  cueDone = false;
  pressActs = pressForce = false;
  if (state == PWR_OFF) {
    // Starts the PC; the press is then ignored until released.
    if (!locked) startPc("button");
    pressActs = pressForce = false;
    return;
  }
  pressActs = (state == PWR_ON);
  pressForce = true;
}

static void onRelease(uint32_t now) {
  uint32_t held = now - btnSince;
  if (pressForce && held >= holdForceMs()) {  // loop stalled past the force time
    forceOff("button held (force time)");
  } else if (pressActs && state == PWR_ON) {
    // cueDone: the user was told to let go for a reset, even if the
    // threshold was raised meanwhile.
    if (held < holdResetMs() && !cueDone) shutdownRequest("button");
    else resetRequest("button");
  }
  pressActs = pressForce = false;
}

void powerUpdate() {
  uint32_t now = nowMs();

  // 1. End pulses first, whatever else happens.
  if (pwrPulse && (int32_t)(now - pwrEndAt) >= 0) { pinWrite(PIN_PWR, 0); pwrPulse = false; }
  if (rstPulse && (int32_t)(now - rstEndAt) >= 0) { pinWrite(PIN_RST, 0); rstPulse = false; }
  if (pwrShow && (int32_t)(now - pwrShowUntil) >= 0) pwrShow = false;
  if (rstShow && (int32_t)(now - rstShowUntil) >= 0) rstShow = false;
  if (errorOn && (int32_t)(now - errorUntil) >= 0) errorOn = false;

  // Restart imminent (section 17): leave PSON and the state alone from here.
  if (frozen) return;

  // 2. SENSE debounce: lit after 200 ms low, dark after off_detect_ms high.
  bool rawLit = senseInput(now);
  if (rawLit != senseRawPrev) { senseRawPrev = rawLit; senseRawAt = now; }
  if (rawLit && !litDeb && now - senseRawAt >= SENSE_LIT_MS) litDeb = true;
  if (!rawLit && litDeb && now - senseRawAt >= (uint32_t)cfg.off_detect_ms) litDeb = false;
  // 2a. Resume (section 17): kept on once the power LED has been lit 200 ms.
  if (resumeRes == RESUME_PENDING && rawLit && now - senseRawAt >= SENSE_LIT_MS) {
    resumeRes = RESUME_KEPT;
    logEvent("resumed: BC-250 kept on");
  }

  // 2b. Factory-reset gesture (only if the button was down at power-up).
  if (bootHeld) {
    if (!btnDeb) bootHeld = false;
    else if (now - bootAt >= BOOT_FACTORY_RESET_HOLD_MS) {
      bootHeld = false;
      factoryResetFlag = true;
      beeps(5);            // 5 beeps and 5 LED flashes: "erased, let go"
      confirmOn = true;
      confirmAt = now;
      logEvent("factory reset: password and WiFi erased");
    }
  }

  // 3. Button debounce (30 ms, both directions).
  bool rawDown = pinRead(PIN_SWITCH) == 0;
  if (rawDown != btnRawPrev) { btnRawPrev = rawDown; btnRawAt = now; }
  if (rawDown != btnDeb && now - btnRawAt >= BTN_DEBOUNCE_MS) {
    btnDeb = rawDown;
    if (btnDeb) onPress(now); else onRelease(now);
  }

  // 4. Button hold timing.
  if (btnDeb) {
    uint32_t held = now - btnSince;
    if (pressForce && held >= holdForceMs()) {
      forceOff("button held (force time)");
    } else if (pressActs && !cueDone && held >= holdResetMs()) {
      beeps(2);  // tells the user to let go now for a reset
      cueDone = true;
    }
  }

  // 5. State machine.
  uint32_t inState = now - stateSince;
  switch (state) {
    case PWR_OFF:
      break;
    case PWR_STARTING:
      if (litDeb) {
        setState(PWR_ON);
        logEvent("running");
      } else {
        if (cfg.start_pwr_pulse && !pwrPulseDone && inState >= (uint32_t)cfg.start_pwr_delay_ms) {
          pwrPulseDone = true;
          pulsePwr();
          logEvent("start: PWR pulse");
        }
        if (inState >= (uint32_t)cfg.start_timeout_ms) {
          setPson(false);
          beepLong(1000);
          errorOn = true;
          errorUntil = now + ERROR_BLINK_MS;
          setState(PWR_OFF);
          logEvent("start failed: no power LED");
        }
      }
      break;
    case PWR_ON:
      if (!litDeb) {
        bool lost = resumeRes == RESUME_PENDING;
        setPson(false);
        beeps(1);
        setState(PWR_OFF);
        if (lost) logEvent("resume failed: power LED dark, BC-250 not kept on");
        else logEvent("off: BC-250 shut down");
      }
      break;
    case PWR_SHUTTING_DOWN:
      if (!litDeb) {
        setPson(false);
        setState(PWR_OFF);
        logEvent("off: shutdown complete");
      } else if (inState >= (uint32_t)cfg.shutdown_timeout_ms) {
        setState(PWR_ON);
        logEvent("shutdown timed out, still on");
      }
      break;
  }

  // 6. Section 25: keep the RTC record current.
  if (now - sinceSavedAt >= SINCE_SAVE_MS) sinceSave();
}

PowerState powerState() { return state; }
const char *powerStateName(PowerState s) { return NAMES[s]; }
bool powerErrorActive() { return errorOn; }
bool senseLit() { return litDeb; }
bool buttonDown() { return btnDeb; }
uint32_t buttonHeldMs() { return btnDeb ? nowMs() - btnSince : 0; }
bool pwrPulseActive() { return pwrPulse; }
bool rstPulseActive() { return rstPulse; }
bool psonOn() { return pson; }

void powerLock(bool l) { ctlLock(); locked = l; ctlUnlock(); }

static bool restartPossible(std::string &err) {
  if (state == PWR_OFF || (state == PWR_ON && cfg.pson_hold)) return true;
  if (state == PWR_ON) err = "the BC-250 is on; enable \"keep the BC-250 on during restart\" first";
  else err = "not possible while the BC-250 is starting or shutting down";
  return false;
}

// Gate for /api/restart and /api/update. Allowed in `off`, or in `on` when
// pson_hold is set. Blocks power-on from now on until powerLock(false) (in
// `on` that changes nothing: shutdown, reset and force-off still work).
bool powerLockForRestart(std::string &err) {
  ctlLock();
  bool ok = restartPossible(err);
  if (ok) locked = true;
  ctlUnlock();
  return ok;
}

// Check only, no lock taken.
bool powerRestartPossible(std::string &err) {
  ctlLock();
  bool ok = restartPossible(err);
  ctlUnlock();
  return ok;
}

// Called by the web layer immediately before esp_restart(). Decides and
// latches atomically under the control lock; after HELD the state machine is
// frozen, so nothing can touch PSON until the chip resets. The unprotected
// window (a forced off or sense change is not acted on) is the time from here
// to the reset: esp_restart()'s shutdown handlers, roughly 10-50 ms.
RestartMode powerPrepareRestart() {
  ctlLock();
  RestartMode m;
  if (state == PWR_OFF) {
    m = RESTART_PLAIN;
  } else if (state == PWR_ON && cfg.pson_hold && pson) {
    logEvent("restart: BC-250 kept on (PS_ON latched)");
    rtcPcOn = PC_ON_MAGIC;
    frozen = true;
    gpio_hold_en((gpio_num_t)PIN_PSON);
    m = RESTART_HELD;
  } else if (state == PWR_STARTING || state == PWR_SHUTTING_DOWN) {
    m = RESTART_WAIT;   // let it settle; never restart unlatched while on
  } else {
    m = RESTART_REFUSED;  // `on` without pson_hold
  }
  if (m == RESTART_PLAIN || m == RESTART_HELD) sinceSave();   // section 25: as fresh as possible
  ctlUnlock();
  return m;
}

PowerState powerStateSync() { ctlLock(); PowerState s = state; ctlUnlock(); return s; }

PowerSnapshot powerSnapshot() {
  PowerSnapshot p;
  ctlLock();
  p.state = state;
  p.senseLit = litDeb;
  p.senseRaw = realLit();
  p.pson = pson;
  p.pwr = pwrPulse || pwrShow;
  p.rst = rstPulse || rstShow;
  p.button = btnDeb;
  p.heldMs = btnDeb ? nowMs() - btnSince : 0;
  p.pressLive = btnDeb && (pressActs || pressForce);
  p.error = errorOn;
  p.mode = senseMode;
  p.running = isRunning(state);
  p.sinceS = (uint32_t)((ms64() - (p.running ? runSince : stopSince)) / 1000);
  p.stopAtStart = !p.running && stopAtStart;
  ctlUnlock();
  return p;
}

static bool powerActionLocked(const std::string &a, std::string &err, const char *who) {
  if (a == "on") {
    if (state != PWR_OFF) { err = "already on"; return false; }
    if (locked) { err = "firmware update in progress"; return false; }
    startPc(who);
    return true;
  }
  if (a == "shutdown") {
    if (state != PWR_ON) { err = "only possible while on"; return false; }
    shutdownRequest(who);
    return true;
  }
  if (a == "reset") {
    if (state != PWR_ON) { err = "only possible while on"; return false; }
    beeps(2);
    resetRequest("web");
    return true;
  }
  if (a == "force_off") {
    if (state == PWR_OFF) { err = "already off"; return false; }
    forceOff("web");
    return true;
  }
  err = "unknown action";
  return false;
}

bool powerAction(const std::string &a, std::string &err) {
  ctlLock();
  userAction();
  bool ok = powerActionLocked(a, err, "web");
  ctlUnlock();
  return ok;
}

bool powerJobAction(const std::string &a, std::string &err) {
  if (a != "on" && a != "shutdown") { err = "unknown action"; return false; }
  ctlLock();
  bool ok = powerActionLocked(a, err, "os update");
  ctlUnlock();
  return ok;
}

uint32_t powerUserActions() { ctlLock(); uint32_t n = userActions; ctlUnlock(); return n; }
void powerSetSilent(bool s) { ctlLock(); silent = s; ctlUnlock(); }
bool powerSilent() { ctlLock(); bool s = silent; ctlUnlock(); return s; }
bool powerSilentLocked() { return silent; }

bool powerWake(const char *reason) {
  ctlLock();
  userAction();
  bool ok = (state == PWR_OFF && !locked);
  if (ok) startPc(reason);
  ctlUnlock();
  return ok;
}

bool powerTakeFactoryReset() { ctlLock(); bool f = factoryResetFlag; factoryResetFlag = false; ctlUnlock(); return f; }
bool powerSetSenseMode(const std::string &m, std::string &err) {
  SenseMode n;
  if (m == "real") n = SENSE_REAL;
  else if (m == "lit") n = SENSE_LIT;
  else if (m == "dark") n = SENSE_DARK;
  else if (m == "sim") n = SENSE_SIM;
  else { err = "sense must be real, lit, dark or sim"; return false; }
  ctlLock();
  if (n == senseMode) { ctlUnlock(); return true; }
  senseMode = n;
  // Entering sim: assume a PC that is already running if PSON is high.
  simLit = pson; simLitPending = simDarkPending = simRelitPending = false;
  simPsonPrev = pson; simPwrPrev = pwrPulse; simRstPrev = rstPulse;
  logEvent("debug sense: %s", m.c_str());
  ctlUnlock();
  return true;
}

// LED feedback while a live press is held (section 21). [lock held]
// 0 none, 1 slow flash, 2 fast flash; *heldMs = time since the debounced press.
// Fast only where a release would reset (pressActs, state on).
// 5 flashes with the same rhythm as the beeps: 250 ms on, 200 ms off.
int powerConfirmFlash() {
  if (!confirmOn) return 0;
  uint32_t t = nowMs() - confirmAt;
  if (t >= 5 * 450) { confirmOn = false; return 0; }
  return (t % 450) < 250 ? 1 : 2;
}

int powerHoldPhase(uint32_t *heldMs) {
  if (!btnDeb || !(pressActs || pressForce)) return 0;
  uint32_t held = nowMs() - btnSince;
  *heldMs = held;
  if (held < HOLD_FLASH_AFTER_MS) return 0;
  if (pressActs && state == PWR_ON && held >= holdResetMs()) return 2;
  return 1;
}
