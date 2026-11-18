// SPDX-License-Identifier: GPL-3.0-or-later
#include "tasks.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "power.h"
#include "buzzer.h"
#include "led.h"
#include "ble.h"
#include "quietmem.h"

// Stack sizes are bytes. Control: logEvent's vsnprintf (~1.5 KB worst case)
// on top of the small state machine. BLE: NimBLE scan start/stop run in the
// caller's context plus logEvent via powerWake. Both chosen generously;
// main.cpp logs the real high-water marks once after 2 minutes.
static const uint32_t CONTROL_STACK = 6144;
static const uint32_t BLE_STACK = 8192;
static TaskHandle_t hControl = nullptr, hBle = nullptr;

// Nothing in here may block: no NVS, no network, no BLE API. The only wait
// is on locks that other tasks hold for microseconds (priority inheritance).
static void controlTask(void *) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    ctlLock();
    powerUpdate();
    buzzerUpdate();
    ledUpdate();
    ctlUnlock();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(5));
  }
}

static void bleTask(void *) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    bleUpdate();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(25));
  }
}

void controlTaskStart() {
  // 2.4G build: the stack lives in quiet memory (it is never touched while
  // a capture runs: the capture holds a critical section). The task control
  // block stays in normal RAM.
  static StaticTask_t tcb;
  StackType_t *stack = (StackType_t *)quietAlloc(CONTROL_STACK);
  if (stack) hControl = xTaskCreateStatic(controlTask, "control", CONTROL_STACK, nullptr, 5, stack, &tcb);
  else xTaskCreate(controlTask, "control", CONTROL_STACK, nullptr, 5, &hControl);
}

void bleTaskStart() {
  xTaskCreate(bleTask, "ble", BLE_STACK, nullptr, 2, &hBle);
}

uint32_t controlStackFree() { return hControl ? uxTaskGetStackHighWaterMark(hControl) : 0; }
uint32_t bleStackFree() { return hBle ? uxTaskGetStackHighWaterMark(hBle) : 0; }
