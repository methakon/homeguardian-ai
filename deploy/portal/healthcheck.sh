#!/usr/bin/env bash
# Portal healthcheck. Reads only: it issues HTTP requests to loopback and inspects the systemd state.
# Usage: bash deploy/portal/healthcheck.sh [--port N]
set -uo pipefail
PORT="${HG_WEB_PORT:-8800}"
[ "${1:-}" = "--port" ] && PORT="${2:-8800}"

fail=0
printf '  systemd: '
if command -v systemctl >/dev/null; then
  state="$(systemctl is-active hg-portal 2>/dev/null || true)"
  echo "${state:-unknown}"; [ "$state" = "active" ] || fail=1
else echo "no systemd"; fi

printf '  listener: '
if ss -ltn 2>/dev/null | grep -q "127.0.0.1:${PORT} "; then echo "127.0.0.1:$PORT up"; else echo "NOT listening"; fail=1; fi

printf '  dashboard (/): '
# curl's -w prints 000 when it cannot connect, so it must not be combined with a fallback echo: that yields
# "000000" and makes the output unreadable exactly when a human is debugging.
if ! code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:${PORT}/" 2>/dev/null)"; then
  code="000"
fi
echo "HTTP $code"; [ "$code" = "200" ] || fail=1

printf '  media-control surface requires a session: '
# Probe /api/webrtc/streams: a GET that the portal gates on a session (web_mcp/server.js:560-563 -> 401).
# Deliberately NOT /api/auth/session, which is a STATUS endpoint and correctly answers 200 with
# "not logged in" -- asserting 401 there was a false alarm in an earlier revision of this script.
if ! code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:${PORT}/api/webrtc/streams" 2>/dev/null)"; then
  code="000"
fi
echo "HTTP $code"
case "$code" in
  401|403) ;;                                        # gated: correct
  000) fail=1; echo "    ^ could not reach the portal at all";;
  *) fail=1; echo "    ^ expected 401/403 without a session, got $code — investigate";;
esac

printf '  session status endpoint answers: '
if ! code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:${PORT}/api/auth/session" 2>/dev/null)"; then code="000"; fi
echo "HTTP $code (200 is correct here: it reports "not logged in" rather than erroring)"

printf '  memory: '
if command -v systemctl >/dev/null; then
  mem="$(systemctl show hg-portal -p MemoryCurrent --value 2>/dev/null || true)"
  echo "${mem:-unknown} bytes (cap 180M)"
else echo "n/a"; fi

exit $fail
