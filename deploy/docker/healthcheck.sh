#!/usr/bin/env bash
# HomeGuardian hub container health probe (milestone D-01).
#
# Talks HTTP to the hub's loopback listener with bash's /dev/tcp, so the runtime
# image needs no curl/wget. Healthy == /api/health answers {"ok":true,...} and
# the persisted store is open (the route only answers when the app is serving).
#
# Exit codes: 0 healthy, 1 unhealthy (Docker records the latter in the
# container's health status without restarting it - see restart policy docs).
set -uo pipefail

PORT="${HG_PORT:-8080}"
TIMEOUT_S="${HG_HEALTH_TIMEOUT:-4}"

# Timeout guard: a wedged listener must not hang the probe forever.
exec 3<>"/dev/tcp/127.0.0.1/${PORT}" || exit 1
printf 'GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: hg-healthcheck\r\nConnection: close\r\n\r\n' >&3

response=""
if command -v timeout >/dev/null 2>&1; then
    response="$(timeout "$TIMEOUT_S" cat <&3 || true)"
else
    response="$(cat <&3 || true)"
fi
exec 3<&-
exec 3>&-

case "$response" in
    *'"ok":true'*) exit 0 ;;
    *) exit 1 ;;
esac
