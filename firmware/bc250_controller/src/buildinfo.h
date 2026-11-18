// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Date and time of this build, e.g. "2026-11-15 02:53:59" (build machine local time).
// buildinfo.cpp is rewritten before every build (touch_buildinfo.py), so
// this is the time of the build, not of the last edit of some file.
const char *buildTime();
// "@@BC250FW|<version>|<build time>|@@", findable in the firmware file.
const char *fwMarker();
