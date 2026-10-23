#!/usr/bin/env bash
# Roll the portal back: stop it, disable it, restore the previously installed unit if one was saved, and prove
# the port is free again. Dry-run by default. Touches nothing else on the host.
#
# Usage: bash deploy/portal/rollback.sh [--apply]
set -uo pipefail

UNIT="/etc/systemd/system/hg-portal.service"
BACKUP="${UNIT}.previous"
APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1
PORT="${HG_WEB_PORT:-8800}"

echo "== portal rollback plan =="
echo "  1. systemctl stop hg-portal"
echo "  2. systemctl disable hg-portal"
echo "  3. if $BACKUP exists: restore it over $UNIT and daemon-reload; else leave no unit file"
echo "  4. verify nothing is listening on 127.0.0.1:$PORT"
echo "  not done: firewall, DNS, tunnel, mail, Docker, data (the data dir is left in place deliberately)"

if [ "$APPLY" -ne 1 ]; then echo "  (dry run — nothing changed; re-run with --apply)"; exit 0; fi
[ "$(id -u)" -eq 0 ] || { echo "  --apply requires root" >&2; exit 3; }
systemctl stop hg-portal 2>/dev/null || true
systemctl disable hg-portal 2>/dev/null || true
if [ -f "$BACKUP" ]; then install -m 0644 "$BACKUP" "$UNIT"; else rm -f "$UNIT"; fi
systemctl daemon-reload
sleep 1
if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
  echo "  WARNING: something still listens on $PORT — investigate before declaring the rollback complete" >&2
  exit 4
fi
echo "  rollback complete: no listener on $PORT, unit ${BACKUP}"
