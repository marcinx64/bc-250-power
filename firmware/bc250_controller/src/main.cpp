// SPDX-License-Identifier: GPL-3.0-or-later
// BC-250 power controller. See docs/FINAL_FIRMWARE_SPEC.md.
// The power state machine runs in its own task (tasks.cpp); nothing in the
// main task below it may block it. Plain ESP-IDF: app_main does the start-up
// and creates the main task.
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "pins.h"
#include "settings.h"
#include "power.h"
#include "buzzer.h"
#include "led.h"
#include "wifi_mgr.h"
#include "ble.h"
#include "rf.h"
#include "web.h"
#include "auth.h"
#include "eventlog.h"
#include "version.h"
#include "ssh.h"
#include "osupd.h"
#include "tasks.h"
#include "time_mgr.h"
#include "quietmem.h"
#include "radio.h"
#include "sysmon.h"
#include "util.h"

static void startMainTask();

extern "C" void app_main(void) {
  utilInit();   // netLock, NVS, esp_netif, event loop, USB console (non-blocking)
  // The 5.5 bootloader of devices in the field has rollback enabled: an
  // updated app must say it runs, or the next reset goes back. Done at
  // once, before anything else can fail.
  esp_ota_mark_app_valid_cancel_rollback();

  radioBoot();   // hands the unused radio's memory to the heap
  // Resume decision first: after a deliberate restart under a running PC
  // (section 17) PSON must stay HIGH, so it is not touched below.
  bool resumed = powerBootEarly();

  // All other outputs LOW before anything else, so no opto is on while we start.
  if (!resumed) pinOutput(PIN_PSON, 0);
  pinOutput(PIN_PWR, 0);
  pinOutput(PIN_RST, 0);
  pinOutput(PIN_BUZZER, 0);
  pinOutput(PIN_LED, 0);
  pinInputPullup(PIN_SENSE);
  pinInputPullup(PIN_SWITCH);

  logInit();   // log lock must exist before anything logs

  buzzerInit();
  ledInit();
  settingsLoad();
  authInit();
  timeInit();
  powerInit();      // samples the button before the control task exists
  logEvent("boot, firmware %s", FW_VERSION);
  if (resumed) logEvent("resumed: PS_ON held through the restart");
  controlTaskStart();   // button/power work from here on, while WiFi starts

  wifiInit();
  if (radio2g4()) rfInit();
  else { bleInit(); bleTaskStart(); }
  sysmonInit();
  sshInit();
  osupdInit();
  webInit();
  startMainTask();
  // app_main returns: the IDF main task is deleted and its stack freed.
}

// Main loop (priority 1): network side only. Power, buzzer, LED and
// Bluetooth live in their own tasks (tasks.cpp) and never wait for this one.
// In 2.4G mode the task's stack is in quiet memory (CPU-only; this task
// never runs during a capture, which is a critical section in the rf task);
// app_main returns afterwards and hands its own 8 KB heap stack back for
// firmware uploads, which need ~40 KB of WiFi buffers at their peak. In
// Bluetooth mode there is no quiet memory and the stack comes from the heap.
static TaskHandle_t hMain = nullptr;
static const uint32_t MAIN_STACK = 8192;
static void mainLoop();
static void mainTask(void *) {
  for (;;) mainLoop();
}
static void startMainTask() {
  static StaticTask_t tcb;
  StackType_t *stack = (StackType_t *)quietAlloc(MAIN_STACK);
  if (stack) hMain = xTaskCreateStatic(mainTask, "main", MAIN_STACK, nullptr, 1, stack, &tcb);
  else xTaskCreate(mainTask, "main", MAIN_STACK, nullptr, 1, &hMain);
}

static void mainLoop() {
  netLock();
  if (powerTakeFactoryReset()) {   // flag is set in the control task
    authErase();                   // NVS writes happen here, not there
    wifiForget();
  }
  wifiUpdate();
#ifdef MEMCHECK
  static uint32_t memAt = 0;
  if (nowMs() - memAt > 5000) {
    memAt = nowMs();
    conPrintf("MEM free=%u min=%u largest=%u\n", (unsigned)esp_get_free_heap_size(),
              (unsigned)esp_get_minimum_free_heap_size(),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  }
#endif
  timeUpdate();
  sysmonUpdate();
  webUpdate();
  sshUpdate();
  osupdUpdate();

  static bool hwmLogged = false;
  if (!hwmLogged && nowMs() > 120000) {
    hwmLogged = true;
    logEvent("stack free: control %u, ble %u, main %u", (unsigned)controlStackFree(),
             (unsigned)bleStackFree(), (unsigned)uxTaskGetStackHighWaterMark(hMain));
  }
  netUnlock();
  // Sleep between passes: the lock is free for the web handlers in
  // between, and the idle task gets its time.
  vTaskDelay(pdMS_TO_TICKS(5));
}
