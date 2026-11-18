// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>

enum PowerState { PWR_OFF, PWR_STARTING, PWR_ON, PWR_SHUTTING_DOWN };

// Threading: powerUpdate() runs only in the control task (tasks.cpp) with
// the control lock held. Functions marked [lock held] below are for use
// inside that tick only. Functions marked [takes lock] are for other tasks.
void ctlLock();
void ctlUnlock();

bool powerBootEarly();   // FIRST thing in app_main after utilInit(): resume decision, PSON latch release
bool powerResumed();     // this boot was a resume (PSON held through the restart)
// What became of the resume (section 17): pending until the power LED is seen
// lit for 200 ms (kept) or the state leaves `on` first (lost).
enum ResumeResult { RESUME_NONE, RESUME_PENDING, RESUME_KEPT, RESUME_LOST };
ResumeResult powerResumeResult();         // [takes lock]
const char *resumeResultName(ResumeResult r);   // "none" | "pending" | "kept" | "lost"
void powerInit();     // creates the control lock; samples the button
void powerUpdate();   // [lock held] one tick

PowerState powerState();                 // [lock held]
const char *powerStateName(PowerState s);   // pure lookup, no lock
bool powerErrorActive();
bool senseLit();        // debounced
bool buttonDown();      // debounced
uint32_t buttonHeldMs();
bool pwrPulseActive();
bool rstPulseActive();
bool psonOn();

// While locked (firmware update in progress) no power-on is accepted from
// any source. Chosen over aborting the update: simpler and cannot half-brick.
void powerLock(bool locked);              // [takes lock]
// Restart/update gate (section 17): ok in `off`, or in `on` with pson_hold.
// Sets the power-on lock when ok. [takes lock]
bool powerLockForRestart(std::string &err);
bool powerRestartPossible(std::string &err);   // check only [takes lock]
enum RestartMode { RESTART_PLAIN, RESTART_HELD, RESTART_WAIT, RESTART_REFUSED };
// Right before esp_restart(); HELD means PSON is latched and frozen. [takes lock]
RestartMode powerPrepareRestart();
PowerState powerStateSync();             // [takes lock]


// Web actions: "on", "shutdown", "reset", "force_off". False + err if refused.
// Counted as a user action.
bool powerAction(const std::string &action, std::string &err);
// Bluetooth / 2.4G wake. True if the PC was started. Counted as a user
// action, also when the PC is already on.
bool powerWake(const char *reason);
// Automatic OS updates (section 26): "on" or "shutdown" like the web actions,
// but not a user action. [takes lock]
bool powerJobAction(const std::string &action, std::string &err);
// Counter of user actions: debounced front-button presses, web power
// actions, wakes. A user action also ends the silent mode. [takes lock]
uint32_t powerUserActions();
// Silent mode of an automatic job: no sound at all, and the LED breathes as
// in `off` in every state, without the error blink. [takes lock]
void powerSetSilent(bool silent);
bool powerSilent();             // [takes lock]
bool powerSilentLocked();       // [lock held] for buzzer.cpp and led.cpp

enum SenseMode { SENSE_REAL, SENSE_LIT, SENSE_DARK, SENSE_SIM };

struct PowerSnapshot {
  PowerState state; bool senseLit, senseRaw, pson, pwr, rst, button, error;
  bool pressLive;   // the current press can still do something (cue, reset, force off)
  uint32_t heldMs; SenseMode mode;
  // Section 25. running: state `on` or `shutting_down`. sinceS: seconds
  // since it started running, or since it last stopped. stopAtStart: it has
  // not run since the controller was powered up (sinceS counts from then).
  bool running, stopAtStart; uint32_t sinceS;
};
PowerSnapshot powerSnapshot();               // [takes lock] one consistent copy
bool powerSetSenseMode(const std::string &m, std::string &err);
// True once after the boot-time 10 s button hold; caller erases credentials.
bool powerTakeFactoryReset();
// [lock held] Confirmation of the factory-reset gesture: 5 LED flashes in
// step with the 5 beeps. Returns 0 when not active, 1 = LED on, 2 = LED off.
int powerConfirmFlash();
int powerHoldPhase(uint32_t *heldMs);   // [lock held] 0 none, 1 slow, 2 fast flash
