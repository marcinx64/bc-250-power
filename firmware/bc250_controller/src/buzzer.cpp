// SPDX-License-Identifier: GPL-3.0-or-later
#include "buzzer.h"
#include "pins.h"
#include "power.h"
#include "settings.h"
#include "util.h"
#include "driver/gpio.h"
#include <algorithm>

static const uint32_t BEEP_ON_MS = 250;
static const uint32_t BEEP_GAP_MS = 200;

static int left = 0;
static uint32_t onMs = BEEP_ON_MS;
static bool sounding = false;
// Quiet hours (section 19): published by the main task about once a second;
// the control task only reads it. Silence changes no timing: the sequence
// runs as usual, the pin just stays LOW. The silent mode of an automatic OS
// update (powerSilentLocked) acts the same way.
static volatile bool quiet = false;
static bool pinHigh = false;
void buzzerSetQuiet(bool q) { quiet = q; }
// Single place that touches the pin. Called every tick while a beep is
// sounding, so entering quiet time mid-beep drives the pin LOW at once.
static void drive(bool want) {
  bool hi = want && !quiet && !powerSilentLocked();
  if (hi != pinHigh) { pinHigh = hi; pinWrite(PIN_BUZZER, hi ? 1 : 0); }
}
static uint32_t nextAt = 0;

void buzzerInit() {
  // Pin is already an output, LOW (set first thing in app_main).
  // Without the strongest drive the buzzer does not sound on this board.
  gpio_set_drive_capability((gpio_num_t)PIN_BUZZER, GPIO_DRIVE_CAP_3);
}

static void start(int n, uint32_t len) {
  if (!cfg.buzzer) return;
  left = n;
  onMs = len;
  sounding = false;
  nextAt = nowMs();
}

void beeps(int n) { start(std::min(std::max(n, 1), 5), BEEP_ON_MS); }
void beepLong(uint32_t ms) { start(1, ms); }

void buzzerUpdate() {
  if (!cfg.buzzer) {  // switched off live: silence at once
    if (sounding || left) { drive(false); sounding = false; left = 0; }
    return;
  }
  if (sounding) drive(true);
  if (left == 0 && !sounding) return;
  uint32_t now = nowMs();
  if ((int32_t)(now - nextAt) < 0) return;
  if (sounding) {
    drive(false);
    sounding = false;
    nextAt = now + BEEP_GAP_MS;
  } else if (left > 0) {
    drive(true);
    sounding = true;
    left--;
    nextAt = now + onMs;
  }
}
