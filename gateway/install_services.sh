#!/usr/bin/env bash
# Installs the BuddyBand gateway services as systemd units (WP3). Idempotent.
#   scp -r gateway buddyband-gw:~/buddyband-gateway
#   ssh -t buddyband-gw 'sudo bash ~/buddyband-gateway/install_services.sh'
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
OWNER="${SUDO_USER:-iotlabpi47}"
HOME_DIR="$(getent passwd "$OWNER" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")" && pwd)"

for unit in "$HERE"/systemd/*.service; do
  name="$(basename "$unit")"
  sed -e "s|@OWNER@|$OWNER|g" -e "s|@HOME@|$HOME_DIR|g" "$unit" > "/etc/systemd/system/$name"
  echo "installed $name"
done
systemctl daemon-reload
for unit in "$HERE"/systemd/*.service; do
  name="$(basename "$unit" .service)"
  systemctl enable --quiet "$name"
  systemctl restart "$name"
  sleep 2
  echo "$name: $(systemctl is-active "$name")"
done
journalctl -u buddyband-logger -n 5 --no-pager
