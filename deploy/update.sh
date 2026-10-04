#!/usr/bin/env bash
# Update the Pi to the latest image published by CI.
# Run by hand, or from cron for automatic deploys:
#   */15 * * * * bash ~/bme280-pi-telemetry/deploy/update.sh >> ~/bme-update.log 2>&1
set -euo pipefail
cd "$(dirname "$0")/.."

git pull --ff-only --quiet          # picks up compose/config changes
docker compose pull --quiet api
docker compose up -d --no-build     # recreates the container only if the image changed
docker image prune -f >/dev/null
echo "$(date -Is) running image $(docker compose images -q api)"
