#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The firmware's SSH client (host build) against a real OpenSSH server run as
# the current user on 127.0.0.1 with throw-away keys. Skipped when there is
# no sshd. Usage: sh test_ssh.sh <build dir>
set -u
B=${1:-build}
SSHD=$(command -v sshd || ls /usr/sbin/sshd 2>/dev/null)
if [ -z "$SSHD" ]; then echo "SKIP: no sshd"; exit 0; fi
D=$(mktemp -d)
trap 'kill $(cat "$D/sshd.pid" 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
PORT=$((20000 + $$ % 20000))
ME=$(id -un)
ssh-keygen -q -t ed25519 -N '' -f "$D/host_ed25519" || exit 1
CLIENT="$B/ssh_host"
$CLIENT "$D/key" --pubkey > "$D/pub" || exit 1
cp "$D/pub" "$D/authorized_keys"
cat > "$D/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $D/host_ed25519
PidFile $D/sshd.pid
AuthorizedKeysFile $D/authorized_keys
StrictModes no
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
LogLevel VERBOSE
EOF
"$SSHD" -D -e -f "$D/sshd_config" 2> "$D/sshd.log" &
for i in 1 2 3 4 5 6 7 8 9 10; do
  [ -s "$D/sshd.pid" ] && break
  sleep 0.2
done
# Some systems do not let sshd run as a normal user (privilege separation).
if [ ! -s "$D/sshd.pid" ]; then echo "SKIP: sshd did not start: $(tail -1 "$D/sshd.log")"; exit 0; fi
FAIL=0
check() {   # name expected-exit got-exit
  if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1 (exit $3, expected $2)"; FAIL=1; fi
}

# 1. output on both streams and the exit status
OUT=$($CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'echo hello; echo warn >&2; exit 3' 2> "$D/err1")
check "exit status" 3 $?
[ "$OUT" = "hello" ] && grep -q '^warn$' "$D/err1" && echo "  ok   stdout and stderr" || { echo "  FAIL streams: '$OUT'"; cat "$D/err1"; FAIL=1; }
HOSTKEY=$(sed -n 's/^ssh_host: host key \([0-9a-f]*\) .*/\1/p' "$D/err1")
FP=$(ssh-keygen -l -f "$D/host_ed25519.pub" | awk '{print $2}')
grep -q "$FP" "$D/err1" && echo "  ok   host key fingerprint as ssh-keygen" || { echo "  FAIL fingerprint"; FAIL=1; }

# 2. a lot of output: window adjustments, many packets
N=$($CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'head -c 1000000 /dev/zero | tr "\0" x' 2> "$D/err2" | wc -c)
check "1 MB of output" 1000000 "$N"

# 2b. stop while output keeps coming (the poll callback must still be asked)
# (8 s of output; the client checks at least once a second)
SSH_HOST_CANCEL_AFTER=100000 $CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'timeout 8 yes' 2> "$D/err2b" > /dev/null
MS=$(sed -n 's/.* total=\([0-9]*\)ms.*/\1/p' "$D/err2b")
grep -q 'ssh_host: cancelled' "$D/err2b" && [ "${MS:-99999}" -lt 3000 ] && echo "  ok   stop during output (${MS} ms)" || { echo "  FAIL stop during output"; tail -2 "$D/err2b"; FAIL=1; }

# 3. pinned key accepted, wrong pinned key refused
$CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'true' "$HOSTKEY" 2> "$D/err3"
check "pinned host key" 0 $?
$CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'true' 0000000000000000000000000000000000000000000000000000000000000001 2> "$D/err4"
check "wrong host key refused" 255 $?
grep -q 'does not match' "$D/err4" || { echo "  FAIL no mismatch message"; FAIL=1; }

# 4. a key that is not in authorized_keys
$CLIENT "$D/other" 127.0.0.1 $PORT "$ME" 'true' 2> "$D/err5"
check "unknown key refused" 255 $?
grep -q 'refused the key' "$D/err5" || { echo "  FAIL no refusal message"; cat "$D/err5"; FAIL=1; }

# 5. restricted key with a forced command (the recommended setup on the BC-250)
printf 'restrict,command="echo forced; exit 7" %s\n' "$(cat "$D/pub")" > "$D/authorized_keys"
OUT=$($CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'echo not this' 2> "$D/err6")
check "forced command" 7 $?
[ "$OUT" = "forced" ] && echo "  ok   forced command output" || { echo "  FAIL forced output '$OUT'"; FAIL=1; }

# 6. a slow command with silence longer than the keepalive time (5 s in ssh_host)
cp "$D/pub" "$D/authorized_keys"
OUT=$($CLIENT "$D/key" 127.0.0.1 $PORT "$ME" 'sleep 12; echo late' 2> "$D/err7")
check "silent command with keepalives" 0 $?
[ "$OUT" = "late" ] || { echo "  FAIL late output '$OUT'"; FAIL=1; }

if [ $FAIL != 0 ]; then echo "--- sshd log"; tail -30 "$D/sshd.log"; exit 1; fi
echo "SSH CLIENT TESTS PASSED ($(ssh -V 2>&1))"
