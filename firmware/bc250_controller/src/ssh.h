// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SSH client of the controller: logs in to the BC-250 with the controller's
// own Ed25519 key and runs one of the update modes (spec section 26). The
// protocol is in ssh_client.cpp; this module keeps the key, the target and
// the pinned host key in NVS, runs a session in its own task and records the
// result and the memory it took. osupd.cpp decides when to run a session.
//
// Threading: everything here except the session task runs with netLock held
// (HTTP handlers, main task). The session task takes netLock only to publish
// its progress.
#include <stdint.h>
#include <string>
#include <ArduinoJson.h>

void sshInit();               // main task start-up: PSA crypto, settings
void sshUpdate();             // main loop (netLock held): memory figures, self-test
bool sshRunning();            // a session task exists (any task)

// What the last session did. Valid from the end of the session until the
// next sshStart() (netLock).
struct SshOutcome {
  bool valid = false;
  bool ok = false;            // the command ran and its channel closed; for "poweroff" and
                              // "reboot" also: logged in, then the connection dropped
  bool loggedIn = false;      // the key was accepted in this session
  bool keyRefused = false;    // the server refused the key
  bool hostKeyBad = false;    // the host key does not match the pinned one
  int exitStatus = -1;
  std::string error, tail;    // tail: last 1 KB of the output, line breaks kept
  bool rebootNeeded = false;  // the tail has "bc250-update: reboot needed"
  uint32_t outBytes = 0, totalMs = 0;
};
const SshOutcome &sshOutcome();

// Starts a session task that runs `mode` ("check", "update", "poweroff" or
// "reboot") as the SSH command. It retries the TCP connection about every
// 10 s for connectMs (0: one attempt) and ends the session limitMs after the
// start at the latest. Never blocks.
bool sshStart(const char *mode, uint32_t connectMs, uint32_t limitMs, std::string &err);
void sshStop();               // asks a running session to end
bool sshLoggedIn();           // the running session has logged in (any task)

// Target. Changing host or port forgets the pinned host key and the link.
const std::string &sshHost();
uint32_t sshPort();
const std::string &sshUser();
bool sshSetTarget(const std::string &host, uint32_t port, const std::string &user, std::string &err);
void sshForgetHost();         // also clears `linked`

// Key: created on first use, so only call this while automatic updates are on.
bool sshKeyInfo(std::string &line, std::string &fingerprint);
std::string sshHostKeyFingerprint();   // "" = none pinned
bool sshLinked();             // persisted: a session logged in with our key
void sshMemToJson(JsonObject o);
