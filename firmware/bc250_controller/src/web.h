// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
void webInit();
// True while someone is using the web UI or uploading firmware (a request
// in the last 20 s). The Bluetooth task then keeps its scan light so the
// shared radio serves WiFi well. Safe to call from any task.
bool webRecentlyActive();
// Called by the main task with netLock() held, about every 5 ms, last in the
// loop. Housekeeping only (deferred restart, upload timeouts): requests are
// served by the esp_http_server task, whose handlers take netLock().
void webUpdate();
// True while a firmware upload is being written (the rf task then leaves the
// radio alone, the power-on lock is held). Safe to call from any task.
bool webOtaRunning();
