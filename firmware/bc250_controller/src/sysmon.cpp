// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysmon.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "util.h"
#include <atomic>
#include <climits>

static const uint32_t PERIOD_MS = 5000;
static temperature_sensor_handle_t tsens = nullptr;
static std::atomic<int32_t> tenths{INT32_MIN};   // 0.1 C
static std::atomic<int> cpu{-1};
static uint32_t lastAt = 0;
static uint64_t idle0 = 0, wall0 = 0;

void sysmonInit() {
  temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&c, &tsens) == ESP_OK) {
    if (temperature_sensor_enable(tsens) != ESP_OK) { temperature_sensor_uninstall(tsens); tsens = nullptr; }
  } else {
    tsens = nullptr;
  }
  idle0 = ulTaskGetIdleRunTimeCounter();
  wall0 = (uint64_t)esp_timer_get_time();
  lastAt = nowMs();
}

void sysmonUpdate() {
  uint32_t now = nowMs();
  if ((int32_t)(now - lastAt) < (int32_t)PERIOD_MS) return;
  lastAt = now;
  if (tsens) {
    float c;
    if (temperature_sensor_get_celsius(tsens, &c) == ESP_OK)
      tenths.store((int32_t)(c * 10.0f + (c < 0 ? -0.5f : 0.5f)));
  }
  // Run-time counter: esp_timer microseconds (sdkconfig), 64 bit.
  uint64_t idle = ulTaskGetIdleRunTimeCounter(), wall = (uint64_t)esp_timer_get_time();
  uint64_t di = idle - idle0, dw = wall - wall0;
  idle0 = idle; wall0 = wall;
  if (dw > 0) {
    int busy = 100 - (int)((di * 100 + dw / 2) / dw);
    cpu.store(busy < 0 ? 0 : busy > 100 ? 100 : busy);
  }
}

bool chipTempValid() { return tenths.load() != INT32_MIN; }
float chipTempC() { return tenths.load() / 10.0f; }
int cpuLoadPct() { return cpu.load(); }
uint32_t heapTotal() { return (uint32_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT); }
