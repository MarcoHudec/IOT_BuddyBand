#!/usr/bin/env bash
# Shows every BuddyBand MQTT message arriving at the Pi broker (read-only, user "nodered").
#   tools/watch_mqtt.sh                 all topics of group 1
#   tools/watch_mqtt.sh 'buddyband/+/band/+/alarm'
# Needs mosquitto-clients and ~/.config/buddyband/credentials.env (copied from the Pi).
set -euo pipefail
# shellcheck disable=SC1090
source "${BB_CRED:-$HOME/.config/buddyband/credentials.env}"
HOST="${BB_HOST:-raspberrypi47.local}"
TOPIC="${1:-buddyband/#}"
echo "watching $TOPIC on $HOST (Ctrl+C to stop)"
exec mosquitto_sub -h "$HOST" -u nodered -P "$BB_NODERED_PW" -t "$TOPIC" -v -F '%I %t %p'
