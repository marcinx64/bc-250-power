#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# One-time setup of the BC-250 PC for the controller's automatic OS updates:
#   curl -fsS http://@CTL@/bc250-setup.sh | sudo sh
set -eu
u=${SUDO_USER:-root}
[ "$u" != root ] || { echo "run it with sudo, as your normal user" >&2; exit 1; }
bin=/usr/local/sbin/bc250-update
sf=/etc/sudoers.d/bc250-update

# the update script, owned by root so the user cannot change what sudo runs
mkdir -p /usr/local/sbin
curl -fsS http://@CTL@/bc250-update.sh -o "$bin.new"
chmod 755 "$bin.new"
mv "$bin.new" "$bin"

# the user may run only that script as root, without a password
echo "$u ALL=(root) NOPASSWD: $bin" > "$sf.new"
chmod 440 "$sf.new"
visudo -cf "$sf.new" >/dev/null || { rm -f "$sf.new"; echo "bad sudoers file" >&2; exit 1; }
mv "$sf.new" "$sf"

# the controller's key may only run that script (the shell may be fish: no quotes tricks)
g=$(id -gn "$u")
d=$(getent passwd "$u" | cut -d: -f6)/.ssh
k=$d/authorized_keys
install -d -m 700 -o "$u" -g "$g" "$d"
{ grep -sv bc250-controller "$k" || :
  # shellcheck disable=SC2016 # $SSH_ORIGINAL_COMMAND is expanded by the login shell, not here
  printf '%s\n' 'restrict,command="sudo -n /usr/local/sbin/bc250-update \"$SSH_ORIGINAL_COMMAND\"" @KEY@'
} > "$k.new"
chown "$u:$g" "$k.new"
chmod 600 "$k.new"
mv "$k.new" "$k"
restorecon -R "$bin" "$d" 2>/dev/null || :

systemctl enable --now sshd.service 2>/dev/null || systemctl enable --now ssh.service
# check: the user may run the script through sudo without a password
sudo -u "$u" sudo -n -l "$bin" >/dev/null || { echo "the sudo rule does not work" >&2; exit 1; }
echo "bc250-setup: done for $u: $bin, $sf, $k, sshd running"
