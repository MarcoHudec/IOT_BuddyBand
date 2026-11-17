#!/usr/bin/env bash
# Copies the event logger CSVs from the Pi into evaluation/events/ (WP3/WP4 evaluation).
#   tools/fetch_events.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p evaluation/events
scp -q "${BB_SSH:-buddyband-gw}:buddyband-data/events-*.csv" evaluation/events/
wc -l evaluation/events/events-*.csv
