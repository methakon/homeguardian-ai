#!/usr/bin/env bash
# Install the HomeGuardian portal unit. DRY-RUN BY DEFAULT: with no --apply it prints exactly what it would
# do and exits. It never edits firewall rules, DNS, mail configuration, Docker, or any file outside the unit
# directory and the app tree.
#
# Usage:  bash deploy/portal/install.sh --user <user> [--hostname portal.example.com] [--data-dir DIR] [--apply]
set -uo pipefail

APP_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
UNIT_DIR="/etc/systemd/system"
PORTAL_USER=""; HOSTNAME_=""; DATA_DIR=""; APPLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --user) PORTAL_USER="$2"; shift 2 ;;
    --hostname) HOSTNAME_="$2"; shift 2 ;;
    --data-dir) DATA_DIR="$2"; shift 2 ;;
    --apply) APPLY=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$PORTAL_USER" ] || { echo "usage: $0 --user <user> [--hostname H] [--data-dir D] [--apply]" >&2; exit 2; }
DATA_DIR="${DATA_DIR:-/var/lib/hg-portal}"

echo "== portal install plan =="
echo "  app dir      : $APP_DIR"
echo "  portal user  : $PORTAL_USER"
echo "  data dir     : $DATA_DIR  (created if absent; the unit may write ONLY here)"
echo "  unit         : $UNIT_DIR/hg-portal.service"
echo "  steps:"
echo "    1. install -m 0644 deploy/portal/hg-portal.service $UNIT_DIR/hg-portal.service (placeholders substituted)"
echo "    2. systemctl daemon-reload"
echo "    3. systemctl enable --now hg-portal"
echo "  not done by this script: firewall, DNS, tunnel creation, TLS, mail, Docker, credentials"

if [ "$APPLY" -ne 1 ]; then
  echo "  (dry run — nothing written; re-run with --apply to perform exactly the steps above)"
  exit 0
fi
[ "$(id -u)" -eq 0 ] || { echo "  --apply requires root (systemctl + $UNIT_DIR)" >&2; exit 3; }
set -e
install -d -m 0750 -o "$PORTAL_USER" -g "$PORTAL_USER" "$DATA_DIR"
sed -e "s|<APP_DIR>|$APP_DIR|g" -e "s|<PORTAL_USER>|$PORTAL_USER|g" -e "s|<DATA_DIR>|$DATA_DIR|g" \
    "$APP_DIR/deploy/portal/hg-portal.service" > "$UNIT_DIR/hg-portal.service"
chmod 0644 "$UNIT_DIR/hg-portal.service"
systemctl daemon-reload
systemctl enable --now hg-portal
sleep 2
systemctl is-active hg-portal >/dev/null && echo "  portal active" || { echo "  portal failed to start; see: journalctl -u hg-portal -n 50" >&2; exit 4; }
bash "$APP_DIR/deploy/portal/healthcheck.sh" || exit 5
