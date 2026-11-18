// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Pin map, see docs/HARDWARE.md. Never use GPIO2, 8, 9, 21.
// UART0 must never be started: its RX is GPIO20 (SWITCH).
#include <stdint.h>
#include "driver/gpio.h"

#define PIN_PSON   3   // HIGH = ATX supply on
#define PIN_BUZZER 4   // HIGH = sound (active buzzer)
#define PIN_SENSE  5   // LOW = BC-250 power LED lit
#define PIN_PWR    6   // HIGH = power button pressed
#define PIN_RST    7   // HIGH = reset button pressed
#define PIN_SWITCH 20  // LOW = front button pressed
#define PIN_LED    10  // front button LED, PWM

// Plain GPIO helpers (like Arduino's pinMode/digitalWrite/digitalRead).
// pinOutput sets the level before it switches the pad to an output, so the
// pin goes straight to `level`.
static inline void pinWrite(int pin, int level) { gpio_set_level((gpio_num_t)pin, level); }
static inline int pinRead(int pin) { return gpio_get_level((gpio_num_t)pin); }
static inline void pinOutput(int pin, int level) {
  gpio_set_level((gpio_num_t)pin, level);
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_OUTPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
  gpio_set_level((gpio_num_t)pin, level);
}
static inline void pinInputPullup(int pin) {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
}
