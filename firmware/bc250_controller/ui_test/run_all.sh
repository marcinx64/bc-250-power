#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs every UI suite, each against a fresh mock started with the options that suite needs (README.md).
# Prints one line per suite (its FAIL lines too, if any). Needs python3 and bun. Exit status: suites that failed.
cd "$(dirname "$0")" || exit 1
fails=0
# run SCRIPT "MOCK_VAR=1 ..." "TEST_VAR=1 ..." [extra mock options for a second mock on port 8767]
run() {
  local name="$1" mockenv="$2" testenv="$3" mock2="$4" pids=()
  env $mockenv python3 mock.py 8766 >/dev/null 2>&1 & pids+=($!)
  if [ -n "$mock2" ]; then env $mock2 python3 mock.py 8767 >/dev/null 2>&1 & pids+=($!); fi
  sleep 1
  local out="" one
  for one in $name; do out+=$(env P=8766 $testenv timeout 120 bun "$one" 2>&1)$'\n'; done
  kill "${pids[@]}" 2>/dev/null; wait "${pids[@]}" 2>/dev/null
  local p f; p=$(grep -c '^PASS' <<<"$out"); f=$(grep -c '^FAIL' <<<"$out")
  if [ "$f" -gt 0 ] || [ "$p" -eq 0 ]; then
    fails=$((fails + 1)); echo "FAIL $name [$mockenv $testenv]: $p pass, $f fail"; grep -v '^PASS' <<<"$out" | head -8
  else echo "ok   $name [$mockenv $testenv]: $p pass"; fi
}

PLAIN="MOCK_CONN=1 MOCK_TZNAME=Europe/Warsaw"
for t in test_dash test_index test_toast test_banners test_ble test_theme test_hold test_hold_s test_late test_info test_factory test_since test_osupdate; do
  run $t.js "$PLAIN" ""
done
run test_i18n.js "$PLAIN" "AP_P=8767" "MOCK_APONLY=1"
run test_cfgreset.js "MOCK_CONN=1" ""
run test_fw.js "MOCK_HOLDOFF=1 MOCK_CONN=1" ""
run test_flow.js "MOCK_HOLDOFF=1 MOCK_CONN=1" ""
run "test_setup.js test_setup2.js" "MOCK_APONLY=1" ""
run test_2g4.js "MOCK_RADIO=2g4" "BLE_P=8767" "MOCK_CONN=1"
run test_bthost.js "MOCK_RADIO=2g4" ""
run test_autofill.js "MOCK_RADIO=2g4" ""
run test_radioname.js "MOCK_RADIO=2g4" ""
run test_wakemethod.js "MOCK_CONN=1" ""
run test_clear.js "MOCK_OTHER=1 MOCK_CONN=1" ""
run test_clear.js "MOCK_OTHER=1 MOCK_RADIO=2g4" ""
run test_health_old.js "MOCK_TEMP=absent" ""
run test_ble_empty.js "MOCK_NOCTL=1 MOCK_CONN=1" ""
run test_time.js "MOCK_CONN=1" "MODE=main"
run test_time.js "MOCK_UNSYNCED=1" "MODE=unsynced"
run test_time.js "MOCK_UNSYNCED=1 MOCK_IGNORETIME=1" "MODE=unsynced IGNORE=1"
run test_time.js "MOCK_CONN=1" "MODE=kept"
run test_time.js "MOCK_NONET=1" "MODE=nonet"
run test_time.js "MOCK_NOTIME=1" "MODE=notime"
echo "suites with failures: $fails"
exit $fails
