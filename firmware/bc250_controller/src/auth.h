// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>

void authInit();
bool authPasswordSet();
// Session id from the Cookie header value, "" if none/invalid.
bool authValidCookie(const std::string &cookieHeader);
// Login. Returns HTTP status (200, 401, 429); on 200 `sid` is the new session.
int authLogin(const std::string &password, std::string &sid);
void authLogout(const std::string &cookieHeader);
// Seconds of login lockout left (0 = none).
uint32_t authLockedSeconds();
// Set/change password. `cookieHeader` identifies the caller's session (kept,
// others dropped). Returns HTTP status; 200 gives the session id in `sid`.
int authSetPassword(const std::string &oldPw, const std::string &newPw, const std::string &cookieHeader,
                    std::string &sid, std::string &err);
void authErase();   // factory reset: forget the password and all sessions
