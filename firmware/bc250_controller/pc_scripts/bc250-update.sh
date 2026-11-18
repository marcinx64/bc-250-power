#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# /usr/local/sbin/bc250-update: the BC-250 controller runs it as root (ssh + sudo).
# Usage: bc250-update check|update|poweroff|reboot
set -eu
export LC_ALL=C
mode=${1-}
case $mode in
check|update) ;;
poweroff|reboot) exec systemctl "$mode" ;;
*) echo "usage: bc250-update check|update|poweroff|reboot" >&2; exit 2 ;;
esac
PRETTY_NAME=unknown
# shellcheck disable=SC1091 # not a file of ours
[ -r /etc/os-release ] && . /etc/os-release
have() { command -v "$1" >/dev/null 2>&1; }
left() { echo "bc250-update: updates still pending" >&2; exit 1; }
need() { echo "bc250-update: reboot needed"; }

if have rpm-ostree; then            # Bazzite, Fedora Atomic
  if [ "$mode" = check ]; then
    echo "$PRETTY_NAME"
    rpm-ostree upgrade --check
    echo "flatpak updates: $(flatpak remote-ls --system --updates | wc -l)"
    exit 0
  fi
  rpm-ostree upgrade
  flatpak update --system --noninteractive -y
  [ -z "$(flatpak remote-ls --system --updates)" ] || left
  # 77 = a deployment is staged; it goes live at the next boot
  rc=0; rpm-ostree status --pending-exit-77 >/dev/null || rc=$?
  [ $rc = 0 ] || [ $rc = 77 ] || exit 1
  [ $rc = 0 ] || need
elif have pacman; then              # Arch, CachyOS
  if [ "$mode" = check ]; then
    echo "$PRETTY_NAME, $( (checkupdates || pacman -Qu) 2>/dev/null | wc -l) updates"
    exit 0
  fi
  kr=$(pacman -Qq archlinux-keyring cachyos-keyring 2>/dev/null || true)
  # shellcheck disable=SC2086 # word splitting wanted: zero, one or two names
  pacman -Sy --noconfirm --noprogressbar --needed $kr
  pacman -Su --noconfirm --noprogressbar
  [ -z "$(pacman -Qu | grep -v '\[ignored\]' || true)" ] || left   # IgnorePkg ones stay
  [ -d "/usr/lib/modules/$(uname -r)" ] || need
elif have dnf; then                 # plain Fedora
  if [ "$mode" = check ]; then
    echo "$PRETTY_NAME, $( (dnf -q check-update || true) | grep -c . || true) updates"
    exit 0
  fi
  dnf -y upgrade --refresh
  dnf -q check-update >/dev/null || left
  dnf -q needs-restarting -r >/dev/null 2>&1 || need
else
  echo "bc250-update: unsupported OS: $PRETTY_NAME" >&2
  exit 2
fi
echo "bc250-update: done"
