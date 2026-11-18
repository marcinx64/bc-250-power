#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
for spec in "8766 main" "8767 unsynced" "8768 unsynced IGN" "8769 kept" "8770 nonet" "8771 notime"; do
  set -- $spec; echo "== $spec"
  if [ "$3" = IGN ]; then P=$1 MODE=$2 IGNORE=1 timeout 40 bun test_time.js 2>&1 | grep -v "^PASS"; else P=$1 MODE=$2 timeout 60 bun test_time.js 2>&1 | grep -v "^PASS"; fi
done
