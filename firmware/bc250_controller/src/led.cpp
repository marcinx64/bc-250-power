// SPDX-License-Identifier: GPL-3.0-or-later
#include "led.h"
#include <math.h>
#include <algorithm>
#include "driver/ledc.h"
#include "pins.h"
#include "power.h"
#include "settings.h"
#include "util.h"

// LEDC set up like Arduino's analogWrite(pin, v): low-speed timer 0, 8 bits,
// 1 kHz, channel 0; a value of 255 or more is "full on" (duty 256).
static const ledc_mode_t LED_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t LED_CH = LEDC_CHANNEL_0;
static bool ledcReady = false;
static int lastOut = -1;

static void out(int v) {
  v = std::min(std::max(v, 0), 255);
  if (v == lastOut) return;
  lastOut = v;
  if (!ledcReady) return;
  ledc_set_duty(LED_MODE, LED_CH, v >= 255 ? 256 : v);
  ledc_update_duty(LED_MODE, LED_CH);
}

// Breathing with a squared (gamma) curve, as in the bench test.
static int breathe(uint32_t now, uint32_t period, int peak) {
  float phase = (now % period) / (float)period;
  float v = (1.0f - cosf(phase * 2.0f * (float)M_PI)) / 2.0f;
  v = v * v;
  return (int)(v * peak + 0.5f);
}

void ledInit() {
  ledc_timer_config_t t = {};
  t.speed_mode = LED_MODE;
  t.duty_resolution = LEDC_TIMER_8_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 1000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ledc_channel_config_t c = {};
  c.gpio_num = PIN_LED;
  c.speed_mode = LED_MODE;
  c.channel = LED_CH;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 0;
  c.hpoint = 0;
  ledcReady = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
  lastOut = -1;
  out(0);
}

void ledUpdate() {
  static uint32_t lastAt = 0;
  uint32_t now = nowMs();
  if (now - lastAt < 20) return;  // 50 updates per second is smooth enough
  lastAt = now;

  int confirm = powerConfirmFlash();   // factory-reset gesture: highest priority
  if (confirm) {
    out(confirm == 1 ? 255 : 0);      // full brightness whatever led_on is
    return;
  }
  if (powerSilentLocked()) {  // automatic OS update: breathe as in `off`, whatever the state
    out(breathe(now, cfg.led_off_period_ms, cfg.led_off_max));
    return;
  }
  if (powerErrorActive()) {  // 5 Hz blink; full brightness so it is visible
    out(((now / 100) & 1) ? 255 : 0);
    return;
  }
  uint32_t held = 0;
  int phase = powerHoldPhase(&held);
  if (phase) {   // hold feedback: 2 Hz or 6 Hz, 50 %, phase locked to the press
    int level = cfg.led_on ? cfg.led_on : 255;
    bool on = phase == 1 ? ((held / 250) & 1) == 0 : (((held * 12) / 1000) & 1) == 0;
    out(on ? level : 0);
    return;
  }
  switch (powerState()) {
    case PWR_OFF:           out(breathe(now, cfg.led_off_period_ms, cfg.led_off_max)); break;
    case PWR_STARTING:
    case PWR_SHUTTING_DOWN: out(breathe(now, 1000, cfg.led_on)); break;
    case PWR_ON:            out(cfg.led_on); break;
  }
}
