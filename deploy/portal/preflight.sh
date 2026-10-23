#!/usr/bin/env bash
# Preflight for the HomeGuardian portal deployment. CHECKS ONLY — this script changes nothing: no firewall,
# no DNS, no packages, no Docker, no mail configuration. It prints a report and exits non-zero if a
# requirement fails, so it is safe to run on the production host before deciding anything.
#
# Usage: bash deploy/portal/preflight.sh [--app-dir DIR] [--user USER]
set -uo pipefail

APP_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PORTAL_USER="$(id -un)"
while [ $# -gt 0 ]; do
  case "$1" in
    --app-dir) APP_DIR="$2"; shift 2 ;;
    --user)    PORTAL_USER="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

PASS=0; FAIL=0; WARN=0
ok()   { printf '  PASS  %-52s %s\n' "$1" "${2:-}"; PASS=$((PASS+1)); }
bad()  { printf '  FAIL  %-52s %s\n' "$1" "${2:-}"; FAIL=$((FAIL+1)); }
warn() { printf '  WARN  %-52s %s\n' "$1" "${2:-}"; WARN=$((WARN+1)); }

echo "== HomeGuardian portal preflight (check-only) =="
echo "  app dir: $APP_DIR"
echo "  target user: $PORTAL_USER"

# --- what the portal needs to exist --------------------------------------------------------------
[ -f "$APP_DIR/web_mcp/server.js" ] && ok "portal source present" "web_mcp/server.js" \
  || bad "portal source present" "web_mcp/server.js missing under $APP_DIR"
[ -d "$APP_DIR/node_modules" ] && ok "node dependencies installed" "$(ls "$APP_DIR/node_modules" | wc -l) packages" \
  || bad "node dependencies installed" "run: npm install --omit=dev"
NODE_BIN="$(command -v node || true)"
[ -n "$NODE_BIN" ] && ok "node found" "$("$NODE_BIN" --version)" || bad "node found" "not installed"
if [ -n "$NODE_BIN" ]; then
  major="$("$NODE_BIN" --version | sed 's/^v//; s/\..*//')"
  [ "$major" -ge 18 ] && ok "node major version" "v$major (>= 18 required)" \
    || bad "node major version" "v$major is too old"
fi
command -v systemctl >/dev/null && ok "systemd present" "unit-based install possible" \
  || warn "systemd present" "no systemd: run the portal under your own supervisor"

# --- the port must be free, and must be the loopback one -----------------------------------------
PORT="${HG_WEB_PORT:-8800}"
if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
  warn "port $PORT" "already listening — the portal will fail to bind unless that listener goes away"
else
  ok "port $PORT free" "portal binds 127.0.0.1:$PORT (loopback by construction)"
fi
ss -ltn 2>/dev/null | grep -qE ':(8790|18990) ' && warn "private ports" \
  "something is on 8790/18990 — the MCP endpoint must stay private and unpublished"

# --- the host's other services must remain untouched (asserted, never changed) --------------------
for p in 22 25 465 587 993 995; do
  if ss -ltn 2>/dev/null | grep -q ":${p} "; then ok "service on port $p present" "left alone by this deployment"
  else warn "service on port $p" "not listening (fine if this host does not run it)"; fi
done

# --- memory headroom: the unit caps the portal at 180M --------------------------------------------
if [ -r /proc/meminfo ]; then
  total=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)
  avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  echo "  ----  memory: ${total} MiB total, ${avail} MiB available; the unit is capped at 180 MiB"
  [ "$avail" -ge 300 ] && ok "memory headroom" "${avail} MiB free for the portal + mail" \
    || warn "memory headroom" "${avail} MiB free — the cap leaves little room"
fi

# --- the tunnel front (owner-supplied) ------------------------------------------------------------
if command -v cloudflared >/dev/null; then
  ok "cloudflared present" "$(cloudflared --version 2>/dev/null | head -1)"
else
  warn "cloudflared present" "not installed — required for HTTPS ingress without a public port"
fi
echo "  ----  owner-supplied values this deployment needs (not checked, cannot be):"
echo "          <PORTAL_HOSTNAME>  DNS record in Cloudflare for the dashboard"
echo "          <TUNNEL_NAME>      cloudflared tunnel name"
echo "          <PATH_TO_TUNNEL_CREDENTIALS_JSON>  created by 'cloudflared tunnel create', mode 0600"
echo "          TLS/DNS for that hostname — verify in the Cloudflare dashboard; this script does not touch it"

echo "  --------------------------------------------------------------------"
echo "  preflight: $PASS passed, $WARN warnings, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
echo "  NOTE: a passing preflight is not a deployment. Nothing was installed or started."
