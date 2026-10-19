#!/usr/bin/env bash
# mcp_protocol_checks.sh — the SINGLE definition of the HomeGuardian MCP protocol assertions.
#
# Both the container harness and the on-phone self-test call this, so the checks exist once. If an
# assertion changes, it changes here and both environments follow — there is no second copy to drift.
#
# Takes a base URL and a bearer token. Emits one machine-readable line per check:
#
#     CHECK<TAB>name<TAB>PASS|FAIL<TAB>detail
#
# and a final SUMMARY line. Exit 0 only if every check passed.
#
# Usage: mcp_protocol_checks.sh <base-url> <bearer-token> [timeout-seconds]
set -u

URL="${1:?usage: mcp_protocol_checks.sh <base-url> <token> [timeout]}"
TOK="${2:?bearer token required}"
TMO="${3:-8}"

pass=0; fail=0
emit() { printf 'CHECK\t%s\t%s\t%s\n' "$1" "$2" "$3"; }
ok()  { pass=$((pass+1)); emit "$1" PASS "$2"; }
ng()  { fail=$((fail+1)); emit "$1" FAIL "$2"; }
chk() { if [ "$2" = "$3" ]; then ok "$1" "$2"; else ng "$1" "got=$2 want=$3"; fi; }

H=(-H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream')
J=(-H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream')

# --- initialize -------------------------------------------------------------------------------
curl -s -m "$TMO" -D "$TMPDIR/h.txt" -o "$TMPDIR/init.json" -X POST "$URL" "${H[@]}" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"checks","version":"1"}}}' \
  >/dev/null 2>&1

if grep -q 'homeguardian-mcp' "$TMPDIR/init.json" 2>/dev/null; then ok "initialize.serverInfo" "homeguardian-mcp"; else ng "initialize.serverInfo" "$(head -c 90 "$TMPDIR/init.json" 2>/dev/null)"; fi
if grep -q '2025-11-25' "$TMPDIR/init.json" 2>/dev/null; then ok "initialize.protocolVersion" "2025-11-25"; else ng "initialize.protocolVersion" "absent"; fi

SID=$(grep -i '^Mcp-Session-Id:' "$TMPDIR/h.txt" 2>/dev/null | tr -d '\r' | awk '{print $2}')
[ -n "$SID" ] && ok "initialize.sessionId" "${SID:0:12}..." || ng "initialize.sessionId" "none issued"

# --- tools/list: exactly two read-only tools --------------------------------------------------
curl -s -m "$TMO" -o "$TMPDIR/tools.json" -X POST "$URL" "${H[@]}" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' >/dev/null 2>&1
N=$(grep -o '"name":"[^"]*"' "$TMPDIR/tools.json" 2>/dev/null | wc -l)
chk "tools.count" "$N" "2"
grep -q 'get_homeguardian_status' "$TMPDIR/tools.json" 2>/dev/null && ok "tools.get_homeguardian_status" present || ng "tools.get_homeguardian_status" missing
grep -q 'get_alert_summary'      "$TMPDIR/tools.json" 2>/dev/null && ok "tools.get_alert_summary" present      || ng "tools.get_alert_summary" missing

# --- both read-only calls ---------------------------------------------------------------------
S=$(curl -s -m "$TMO" -X POST "$URL" "${H[@]}" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}' 2>/dev/null)
printf '%s' "$S" | grep -q 'capture_enabled' && ok "call.get_homeguardian_status" "real engine state" || ng "call.get_homeguardian_status" "$(printf '%s' "$S" | head -c 80)"
printf '%s' "$S" | grep -q 'read_only'       && ok "call.status.read_only" true               || ng "call.status.read_only" absent
A=$(curl -s -m "$TMO" -X POST "$URL" "${H[@]}" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' 2>/dev/null)
printf '%s' "$A" | grep -q 'alert_count' && ok "call.get_alert_summary" "real engine state" || ng "call.get_alert_summary" "$(printf '%s' "$A" | head -c 80)"

# --- authentication refusal -------------------------------------------------------------------
c=$(curl -s -m "$TMO" -o /dev/null -w '%{http_code}' -X POST "$URL" "${J[@]}" \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' 2>/dev/null || echo 000)
chk "auth.no_header" "$c" "401"
c=$(curl -s -m "$TMO" -o /dev/null -w '%{http_code}' -X POST "$URL" \
  -H 'Authorization: Bearer wrong-token' -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' 2>/dev/null || echo 000)
chk "auth.bogus_bearer" "$c" "401"

# --- session isolation ------------------------------------------------------------------------
c=$(curl -s -m "$TMO" -o /dev/null -w '%{http_code}' -X POST "$URL" "${H[@]}" \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' 2>/dev/null || echo 000)
chk "session.no_session_id" "$c" "400"
c=$(curl -s -m "$TMO" -o /dev/null -w '%{http_code}' -X POST "$URL" "${H[@]}" -H "Mcp-Session-Id: bogus" \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' 2>/dev/null || echo 000)
chk "session.bogus_session_id" "$c" "404"

printf 'SUMMARY\t%s\t%s\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
