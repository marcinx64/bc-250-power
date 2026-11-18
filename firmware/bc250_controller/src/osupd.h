// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Automatic OS updates of the BC-250 (spec section 26): the schedule, the
// night / manual / test job and its settings. The SSH session itself is in
// ssh.cpp.
//
// Threading: everything here runs with netLock held (main task about once a
// second, HTTP handlers). Nothing here is called from the control task; it
// talks to the power state machine through powerJobAction(), powerSetSilent()
// and powerUserActions().
#include <string>
#include <ArduinoJson.h>

void osupdInit();                 // after sshInit()
void osupdUpdate();               // main loop: schedule and job steps (about 1 Hz)
bool osupdEnabled();
bool osupdBusy();                 // a job or an SSH session is running
void osupdToJson(JsonObject o);   // GET /api/osupdate
// POST /api/osupdate. Returns 0 when applied, else the HTTP status (400, 409) with err set.
int osupdConfigure(JsonObjectConst o, std::string &err);
bool osupdRun(std::string &err);          // POST /api/osupdate/run, false = 409
bool osupdTest(std::string &err);         // POST /api/osupdate/test, false = 409
void osupdStop();                         // POST /api/osupdate/stop
bool osupdForgetHost(std::string &err);   // POST /api/osupdate/forget_host, false = 409
// Address the PC uses to reach this controller (station IP), "" without one.
std::string osupdControllerAddress();
