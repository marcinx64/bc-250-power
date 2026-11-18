#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# End-to-end test of rfreplay: mksigmf writes synthetic captures, rfreplay
# must find the packets in them. Usage: sh test_rfreplay.sh BUILD_DIR
B=${1:-build}
T=$B/rfreplay_test
mkdir -p "$T"
fail=0
check() {   # check DESCRIPTION OUTPUT PATTERN
  if printf '%s\n' "$2" | grep -q -- "$3"; then echo "  ok   $1"; else echo "  FAIL $1 (no match for: $3)"; fail=1; fi
}

for dt in ci16_le ci8 cf32_le; do
  "$B/mksigmf" "$T/syn_$dt" --datatype $dt --esb2m >/dev/null || fail=1
  out=$("$B/rfreplay" "$T/syn_$dt" 2>&1)
  check "$dt: ESB 1 Mbit/s frame learnt" "$out" "snapshot 0 (sample 0, 2437.000 MHz): esb1m key A1B2C3D4E5 exact wrong 0 offset +3[0-9][0-9][0-9] kHz"
  # ci8 quantises the 68 us ID packet coarsely: one wrong bit, corrected by
  # the BCH check, is as good a catch as an exact one
  check "$dt: Bluetooth paging LAP learnt" "$out" "snapshot 1 .*btpage key 9A8B7C \(exact\|approx wrong 1\)"
  check "$dt: ESB 2 Mbit/s frame learnt" "$out" "snapshot 4 .*esb2m key A1B2C3D4E5 exact"
  check "$dt: noise snapshot silent" "$(printf '%s\n' "$out" | grep -c 'snapshot 2 ')" "^0$"
done

out=$("$B/rfreplay" "$T/syn_ci16_le.sigmf-meta" --key esb1m:5A6B7C8D9E --key btpage:9A8B7C 2>&1)
check "listed key with one wrong bit: approximate hit" "$out" "snapshot 3 .*esb1m key 5A6B7C8D9E approx wrong 1 .*\[listed\]"
check "listed host: exact hit" "$out" "btpage key 9A8B7C exact wrong 0.*\[listed\]"
out=$("$B/rfreplay" "$T/syn_ci16_le" --key esb1m:5A6B7C8D9E --budget 0 2>&1)
check "budget 0: the approximate hit is gone" "$(printf '%s\n' "$out" | grep -c 'snapshot 3 ')" "^0$"
out=$("$B/rfreplay" "$T/syn_ci16_le" --only btpage --quiet 2>&1)
check "--only btpage --quiet: summary only" "$out" "summary: 5 snapshots, .* 1 exact hits"
out=$("$B/rfreplay" "$T/syn_ci16_le" --conj --quiet 2>&1)
check "--conj (mirrored spectrum): finds nothing" "$out" "0 exact hits"

# a wrong sample rate is refused with a clear message
"$B/mksigmf" "$T/syn_bad_rate" --rate 8000000 >/dev/null
out=$("$B/rfreplay" "$T/syn_bad_rate" 2>&1); rc=$?
check "sample rate 8e6 refused" "$out" "core:sample_rate is 8000000.*16000000"
[ $rc -ne 0 ] && echo "  ok   exit status $rc" || { echo "  FAIL exit status 0 for a bad sample rate"; fail=1; }
out=$("$B/rfreplay" "$T/does_not_exist" 2>&1); rc=$?
check "missing file reported" "$out" "cannot read"

[ $fail -eq 0 ] && echo "rfreplay: all checks passed" || echo "rfreplay: FAILED"
exit $fail
