#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the PC scripts (../pc_scripts): syntax, shellcheck, bc250-update.sh
# against stub commands, bc250-setup.sh inside rootless podman containers.
# Parts are skipped (SKIP line) when their tool is missing. Usage: sh test_pc_scripts.sh
# shellcheck disable=SC2086 # $A, $O are lists of stub names
set -u
H=$(cd "$(dirname "$0")" && pwd)
S=$H/../pc_scripts
UPD=$S/bc250-update.sh
SET=$S/bc250-setup.sh
T=$(mktemp -d)
trap 'kill $(cat "$T/http.pid" 2>/dev/null) 2>/dev/null; rm -rf "${T:?}"' EXIT
FAIL=0
ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; FAIL=1; }

echo "== syntax and shellcheck"
for f in "$UPD" "$SET"; do
  if sh -n "$f"; then ok "sh -n ${f##*/}"; else bad "sh -n ${f##*/}"; fi
  if head -2 "$f" | tr '\n' ' ' | grep -q '^#!/bin/sh # SPDX-License-Identifier: GPL-3.0-or-later'
  then ok "header ${f##*/}"; else bad "header ${f##*/}"; fi
done
if command -v shellcheck >/dev/null; then SC=shellcheck
elif command -v uvx >/dev/null; then SC="uvx --from shellcheck-py shellcheck"
else SC=; fi
if [ -n "$SC" ]; then
  # shellcheck disable=SC2086 # SC is a command with arguments
  if $SC "$UPD" "$SET"; then ok shellcheck; else bad shellcheck; fi
else echo "SKIP: no shellcheck (and no uvx)"; fi

echo "== bc250-update.sh against stub commands"
# The stubs are the only commands on PATH. Each logs "name args" to $LOG and
# answers from the rules file $CFG/<name>: lines "glob-of-args|exit code|output".
mkdir -p "$T/bin" "$T/cfg"
LOG=$T/log; CFG=$T/cfg; export LOG CFG
cat > "$T/stub" <<'EOF'
#!/bin/sh
n=${0##*/}; echo "$n $*" >> "$LOG"
[ -f "$CFG/$n" ] || exit 0
while IFS='|' read -r pat rc out; do
  # shellcheck disable=SC2254 # the pattern is meant to be a glob
  case "$*" in $pat) [ -z "$out" ] || printf '%b\n' "$out"; exit "$rc" ;; esac
done < "$CFG/$n"
exit 0
EOF
chmod +x "$T/stub"
fresh() {   # fresh <stub names...>
  rm -f "${T:?}"/bin/* "${CFG:?}"/*; : > "$LOG"
  ln -s "$(command -v wc)" "$T/bin/wc"; ln -s "$(command -v grep)" "$T/bin/grep"
  for n in "$@"; do ln -s "$T/stub" "$T/bin/$n"; done
  rule uname '-r|0|.'    # "/usr/lib/modules/." exists: no reboot needed
}
rule() { echo "$2" >> "$CFG/$1"; }
go() {      # go <args>: run the script, set rc and out
  out=$(PATH=$T/bin $SHB "$UPD" "$@" 2>&1); rc=$?
}
t() {       # t <name> <expected rc> [pattern wanted in output] [pattern not wanted]
  n=$1; want=$2; pat=${3-}; npat=${4-}
  r=ok
  [ "$rc" = "$want" ] || r="exit $rc, wanted $want"
  [ -z "$pat" ] || printf '%s\n' "$out" | grep -q -- "$pat" || r="no '$pat' in: $out"
  [ -z "$npat" ] || ! printf '%s\n' "$out" | grep -q -- "$npat" || r="unexpected '$npat'"
  if [ "$r" = ok ]; then ok "$n"; else bad "$n ($r)"; fi
}
logged() { if grep -q -- "$2" "$LOG"; then ok "$1"; else bad "$1 (log lacks '$2')"; fi; }
SHB=$(command -v sh)
M='bc250-update: reboot needed'; D='bc250-update: done'

fresh; go;                  t "no mode" 2 usage
go foo;                     t "bad mode" 2 usage
go 'update; reboot';        t "bad mode with a command" 2 usage
fresh systemctl; go poweroff; t "poweroff" 0; logged "poweroff -> systemctl" '^systemctl poweroff$'
fresh systemctl; go reboot;   t "reboot" 0;   logged "reboot -> systemctl" '^systemctl reboot$'
fresh; go update;           t "unknown OS" 2 unsupported
go check;                   t "unknown OS check" 2 unsupported

# Arch / CachyOS
A="pacman uname"
fresh $A; rule pacman '-Qu|0|a 1-1 -> 1-2\nb 1-1 -> 2-1'; go check
t "arch check" 0 '2 updates'
fresh $A; rule pacman '-Qq*|0|archlinux-keyring\ncachyos-keyring'; rule pacman '-Qu|1|'; go update
t "arch update" 0 "^$D\$" "$M"
logged "arch keyrings first" '^pacman -Sy --noconfirm --noprogressbar --needed archlinux-keyring cachyos-keyring$'
logged "arch full upgrade" '^pacman -Su --noconfirm --noprogressbar$'
fresh $A; rule pacman '-Qq*|1|'; rule pacman '-Qu|1|'; : > "$CFG/uname"; rule uname '-r|0|0.0-none'; go update
t "arch reboot marker" 0 "^$M\$"
fresh $A; rule pacman '-Su*|3|'; go update;  t "arch -Su fails" 3 "" "$D"
fresh $A; rule pacman '-Sy*|4|'; go update;  t "arch -Sy fails" 4 "" "$D"
fresh $A; rule pacman '-Qu|0|x 1 -> 2'; go update; t "arch still pending" 1 "still pending" "$D"

# Bazzite / Fedora Atomic
O="rpm-ostree flatpak"
fresh $O; rule rpm-ostree 'upgrade --check|0|AvailableUpdate'; rule flatpak 'remote-ls*|0|a\nb\nc'; go check
t "ostree check" 0 'flatpak updates: 3'
fresh $O; go update
t "ostree update, nothing staged" 0 "^$D\$" "$M"
logged "ostree upgrade" '^rpm-ostree upgrade$'
logged "ostree flatpak" '^flatpak update --system --noninteractive -y$'
fresh $O; rule rpm-ostree 'status*|77|'; go update; t "ostree staged" 0 "^$M\$"
fresh $O; rule rpm-ostree 'status*|1|'; go update;  t "ostree status error" 1 "" "$D"
fresh $O; rule rpm-ostree 'upgrade*|1|boom'; go update; t "ostree upgrade fails" 1 boom "$D"
fresh $O; rule flatpak 'update*|2|'; go update;     t "flatpak update fails" 2 "" "$D"
fresh $O; rule flatpak 'remote-ls*|0|org.x'; go update; t "flatpak still pending" 1 "still pending" "$D"
fresh $O pacman; go update; logged "ostree wins over pacman" '^rpm-ostree upgrade$'

# plain Fedora
fresh dnf; rule dnf '-q check-update|100|pkg.x86_64 1 repo\npkg2.x86_64 1 repo'; go check
t "dnf check" 0 '2 updates'
fresh dnf; go update
t "dnf update" 0 "^$D\$" "$M"
logged "dnf upgrade" '^dnf -y upgrade --refresh$'
fresh dnf; rule dnf '-q needs-restarting*|1|'; go update; t "dnf reboot marker" 0 "^$M\$"
fresh dnf; rule dnf '-y*|1|'; go update;       t "dnf upgrade fails" 1 "" "$D"
fresh dnf; rule dnf '-q check-update|100|p'; go update; t "dnf still pending" 1 "still pending" "$D"

echo "== bc250-setup.sh in rootless podman"
if ! command -v podman >/dev/null; then echo "SKIP: no podman"
elif ! command -v python3 >/dev/null; then echo "SKIP: no python3 (the controller stand-in)"
else
  KEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFAKEKEYFORTESTSONLY000000000000000000000 bc250-controller'
  mkdir -p "$T/www"
  cp "$UPD" "$T/www/bc250-update.sh"
  # the stand-in for the controller: a web server on this host
  PORT=$((20000 + $$ % 20000))
  python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$T/www" >/dev/null 2>&1 &
  echo $! > "$T/http.pid"
  sed -e "s|@CTL@|127.0.0.1:$PORT|g" -e "s|@KEY@|$KEY|" "$SET" > "$T/www/bc250-setup.sh"
  cat > "$T/inner.sh" <<'EOF'
set -eu
# --- prepare: sudo, a user with an old controller line and another key
if command -v dnf >/dev/null; then dnf -y -q install sudo >/dev/null 2>&1
else pacman -Syu --noconfirm --needed sudo >/dev/null 2>&1; fi
useradd -m -s /bin/sh tester
echo 'tester ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/00-test
printf '#!/bin/sh\necho "$*" >> /tmp/systemctl.log\n' > /usr/local/bin/systemctl   # no systemd here
chmod +x /usr/local/bin/systemctl
mkdir -p /home/tester/.ssh
printf '%s\n' 'ssh-ed25519 AAAAOTHERKEY other@laptop' 'restrict,command="x" ssh-ed25519 AAAAOLD bc250-controller' > /home/tester/.ssh/authorized_keys
chown -R tester: /home/tester/.ssh
# --- run the documented command twice
for i in 1 2; do sudo -u tester -H sh -c "curl -fsS http://127.0.0.1:$PORT/bc250-setup.sh | sudo sh"; done
# --- check
fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: '$3', wanted '$2'"; fail=1; fi; }
chk "update script owner and mode" "root 755" "$(stat -c '%U %a' /usr/local/sbin/bc250-update)"
chk "update script is the served one" "" "$( [ "$(cksum < /usr/local/sbin/bc250-update)" = "$(cksum < /t/www/bc250-update.sh)" ] || echo differs)"
chk "sudoers owner and mode" "root 440" "$(stat -c '%U %a' /etc/sudoers.d/bc250-update)"
chk "sudoers line" "tester ALL=(root) NOPASSWD: /usr/local/sbin/bc250-update" "$(cat /etc/sudoers.d/bc250-update)"
chk "visudo -cf" 0 "$(visudo -cf /etc/sudoers.d/bc250-update >/dev/null; echo $?)"
chk "no leftovers" "" "$(ls /etc/sudoers.d /usr/local/sbin /home/tester/.ssh | grep '\.new' || :)"
chk ".ssh owner and mode" "tester 700" "$(stat -c '%U %a' /home/tester/.ssh)"
ak=/home/tester/.ssh/authorized_keys
chk "authorized_keys owner and mode" "tester 600" "$(stat -c '%U %a' $ak)"
chk "one controller line after two runs" 1 "$(grep -c bc250-controller $ak)"
chk "other key kept" 1 "$(grep -c AAAAOTHERKEY $ak)"
chk "old controller line gone" 0 "$(grep -c AAAAOLD $ak || :)"
chk "controller line" "restrict,command=\"sudo -n /usr/local/sbin/bc250-update \\\"\$SSH_ORIGINAL_COMMAND\\\"\" $KEY" "$(grep bc250-controller $ak)"
chk "sshd enabled and started (2 runs)" 2 "$(grep -c 'enable --now sshd.service' /tmp/systemctl.log)"
# the forced command as sshd runs it: the login shell executes sh -c "<command>"
cmd=$(grep bc250-controller $ak | sed 's/^restrict,command="//; s/" ssh-ed25519.*//; s/\\"/"/g')
printf 'SSH_ORIGINAL_COMMAND=bogus; export SSH_ORIGINAL_COMMAND\nsh -c '"'%s'"'\n' "$cmd" > /tmp/fc.sh
rc=0; out=$(sudo -u tester sh /tmp/fc.sh 2>&1) || rc=$?
chk "forced command runs the script as root via sudo" "2 usage" "$rc $(echo "$out" | grep -o usage | head -1)"
exit $fail
EOF
  for img in registry.fedoraproject.org/fedora:latest docker.io/library/archlinux:latest; do
    echo "-- $img"
    if ! podman image exists "$img" && ! timeout 300 podman pull -q "$img" >/dev/null 2>&1; then
      echo "SKIP: cannot pull $img"; continue
    fi
    if podman run --rm --network=host --security-opt label=disable -e PORT="$PORT" -e KEY="$KEY" \
         -v "$T:/t:ro" "$img" sh /t/inner.sh > "$T/pod.out" 2>&1; then
      grep '^  ok' "$T/pod.out"; ok "setup in $img"
    else
      cat "$T/pod.out"; bad "setup in $img"
    fi
  done
fi

if [ "$FAIL" = 0 ]; then echo "PC scripts: all passed"; else echo "PC scripts: FAILED"; fi
exit "$FAIL"
