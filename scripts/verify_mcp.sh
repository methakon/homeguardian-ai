#!/usr/bin/env bash
# End-to-end evidence for hg_mcp_server (HG-MCP-01): drives the REAL binary over loopback
# with curl and asserts the protocol, session, auth, origin and bound behaviour.
#
# Everything happens in a temporary directory: the credential file is generated here, at run
# time, with mode 0600, and is never committed. No network service is exposed beyond
# 127.0.0.1, and no physical sensor is touched.
#
# Usage: bash scripts/verify_mcp.sh            (from the repository root)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${HG_MCP_BIN:-}" ]; then
  if [ -x "$ROOT/build/hg_mcp_server" ]; then
    BIN="$ROOT/build/hg_mcp_server"
  elif [ -x "$ROOT/hg_mcp_build/hg_mcp_server" ]; then
    BIN="$ROOT/hg_mcp_build/hg_mcp_server"
  else
    BIN="$ROOT/build/hg_mcp_server"
  fi
else
  BIN="$HG_MCP_BIN"
fi
CONFIG="${HG_MCP_CONFIG:-$ROOT/config/homeguardian.demo.json}"
PORT="${HG_MCP_TEST_PORT:-$(( 18800 + RANDOM % 400 ))}"
TMP="$(mktemp -d)"
TOKEN="test-token-$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
TOKEN_BOB="bob-token-$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
PASS=0; FAIL=0; SERVER_PID=""

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null
    for _ in $(seq 1 30); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.2; done
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

check() { # check <label> <expected> <actual>
  if [ "$2" = "$3" ]; then printf '  PASS  %-58s %s\n' "$1" "$3"; PASS=$((PASS+1));
  else printf '  FAIL  %-58s expected %s, got %s\n' "$1" "$2" "$3"; FAIL=$((FAIL+1)); fi
}
check_contains() { # check_contains <label> <needle> <haystack>
  if printf '%s' "$3" | grep -qF -- "$2"; then printf '  PASS  %-58s contains %s\n' "$1" "$2"; PASS=$((PASS+1));
  else printf '  FAIL  %-58s missing %s in: %s\n' "$1" "$2" "$3"; FAIL=$((FAIL+1)); fi
}
check_not_contains() {
  if printf '%s' "$3" | grep -qF -- "$2"; then printf '  FAIL  %-58s leaked %s\n' "$1" "$2"; FAIL=$((FAIL+1));
  else printf '  PASS  %-58s no %s\n' "$1" "$2"; PASS=$((PASS+1)); fi
}

[ -x "$BIN" ] || { echo "server binary not built: $BIN"; exit 2; }

echo "== 1. startup refusal: a missing credential store must exit non-zero (fail closed)"
set +e
"$BIN" "$CONFIG" "$TMP/does-not-exist.json" >"$TMP/out1" 2>&1
rc=$?
set -e
check "missing credential file exits 3" "3" "$rc"

echo "== 3. provision a 0600 credential file and a lax one (permission check)"
umask 077
cat > "$TMP/creds.json" <<JSON
{
  "$TOKEN": {"principal": "verify-script-alice", "allow_status": true, "allow_alert_summary": true},
  "$TOKEN_BOB": {"principal": "verify-script-bob", "allow_status": true, "allow_alert_summary": true}
}
JSON
chmod 600 "$TMP/creds.json"
cp "$TMP/creds.json" "$TMP/creds-lax.json"; chmod 644 "$TMP/creds-lax.json"
set +e
"$BIN" "$CONFIG" "$TMP/creds-lax.json" >"$TMP/out3" 2>&1
rc=$?
set -e
check "world-readable credential file is rejected" "3" "$rc"

echo "== 3b. startup refusal: non-loopback bind without explicit opt-in (reaches the bind check)"
set +e
HG_MCP_BIND=0.0.0.0 "$BIN" "$CONFIG" "$TMP/creds.json" >"$TMP/out2" 2>&1
rc=$?
set -e
check "refuses a non-loopback bind" "4" "$rc"

echo "== 3c. startup refusal: non-loopback bind with HG_MCP_ALLOW_REMOTE=0 must still be refused"
set +e
HG_MCP_ALLOW_REMOTE=0 HG_MCP_BIND=0.0.0.0 "$BIN" "$CONFIG" "$TMP/creds.json" >"$TMP/out2c" 2>&1
rc=$?
set -e
check "refuses non-loopback bind when HG_MCP_ALLOW_REMOTE=0" "4" "$rc"

echo "== 4. start the server on 127.0.0.1:$PORT"
HG_MCP_PORT="$PORT" HG_MCP_AUDIT_DB="$TMP/audit.db" \
  "$BIN" "$CONFIG" "$TMP/creds.json" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/mcp" 2>/dev/null; then break; fi
  kill -0 "$SERVER_PID" 2>/dev/null || { echo "server died:"; cat "$TMP/server.log"; exit 3; }
  sleep 0.2
done
URL="http://127.0.0.1:$PORT/mcp"
AUTH="Authorization: Bearer $TOKEN"
AUTH_BOB="Authorization: Bearer $TOKEN_BOB"
JSON="Content-Type: application/json"
ACCEPT="Accept: application/json, text/event-stream"

echo "== 5. initialize: version negotiation + session minting"
code=$(curl -s -D "$TMP/h1" -o "$TMP/b1" -w '%{http_code}' -X POST "$URL" \
  -H "$AUTH" -H "$JSON" -H "$ACCEPT" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"verify","version":"1"}}}')
check "initialize returns 200" "200" "$code"
check_contains "negotiated protocol version" '"protocolVersion":"2025-11-25"' "$(cat "$TMP/b1")"
check_contains "serverInfo name" 'homeguardian-mcp' "$(cat "$TMP/b1")"
SID=$(grep -i '^mcp-session-id:' "$TMP/h1" | tr -d '\r' | awk '{print $2}')
if [ -n "$SID" ]; then printf '  PASS  %-58s %s\n' "Mcp-Session-Id minted" "${SID:0:8}..."; PASS=$((PASS+1));
else printf '  FAIL  no Mcp-Session-Id header\n'; FAIL=$((FAIL+1)); fi
check "Mcp-Session-Id has length 64 (256-bit CSPRNG entropy)" "64" "${#SID}"
if printf '%s' "$SID" | grep -qE '^[0-9a-f]{64}$'; then
  printf '  PASS  %-58s hex lowercase\n' "Mcp-Session-Id is valid 64-char lowercase hex"; PASS=$((PASS+1));
else
  printf '  FAIL  %-58s got %s\n' "Mcp-Session-Id is valid 64-char lowercase hex" "$SID"; FAIL=$((FAIL+1));
fi
SESS="Mcp-Session-Id: $SID"

echo "== 6. notifications/initialized returns 202 with no body"
code=$(curl -s -o "$TMP/b2" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')
check "notification -> 202" "202" "$code"
check "notification has no body" "0" "$(wc -c < "$TMP/b2" | tr -d ' ')"

echo "== 7. tools/list: exactly the two read tools"
code=$(curl -s -o "$TMP/b3" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
check "tools/list returns 200" "200" "$code"
check_contains "advertises get_homeguardian_status" 'get_homeguardian_status' "$(cat "$TMP/b3")"
check_contains "advertises get_alert_summary" 'get_alert_summary' "$(cat "$TMP/b3")"
check_not_contains "no mutating tool advertised" 'set_capture' "$(cat "$TMP/b3")"

echo "== 8. tools/call for both tools"
code=$(curl -s -o "$TMP/b4" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}')
check "tools/call status returns 200" "200" "$code"
check_contains "status payload" 'capture_enabled' "$(cat "$TMP/b4")"
code=$(curl -s -o "$TMP/b5" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_alert_summary"}}')
check "tools/call alert summary returns 200" "200" "$code"
check_contains "alert summary payload" 'alert_count' "$(cat "$TMP/b5")"

echo "== 9. authentication boundary"
code=$(curl -s -D "$TMP/h9" -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/list"}')
check "no token -> 401" "401" "$code"
check_contains "401 advertises Bearer" 'Bearer' "$(cat "$TMP/h9")"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "Authorization: Bearer wrong" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/list"}')
check "bad token -> 401" "401" "$code"
code=$(curl -s -o "$TMP/b9" -w '%{http_code}' -X POST "$URL" -H "Authorization: Bearer wrong" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_homeguardian_status"}}')
check "unauthenticated tools/call -> 401" "401" "$code"
check_not_contains "401 body leaks no diagnostic" 'expired' "$(cat "$TMP/b9")"
check_not_contains "401 body names no principal" 'principal' "$(cat "$TMP/b9")"

echo "== 9b. cross-principal session binding and ownership verification"
# Principal Bob attempts to hijack, read, notify, ping, re-initialize or delete Alice's session
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":61,"method":"tools/list"}')
check "cross-principal tools/list (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":62,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}')
check "cross-principal tools/call status (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":63,"method":"tools/call","params":{"name":"get_alert_summary"}}')
check "cross-principal tools/call alert summary (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')
check "cross-principal notification (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":64,"method":"ping"}')
check "cross-principal ping (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":65,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"verify","version":"1"}}}')
check "cross-principal re-initialize (Bob -> Alice session) -> 403" "403" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH_BOB" -H "$SESS")
check "cross-principal DELETE (Bob -> Alice session) -> 403" "403" "$code"

# Positive control: Alice retains uninterrupted access to her session after Bob's forbidden attempts
code=$(curl -s -o "$TMP/b_pos" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":66,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}')
check "positive control: Alice session undisturbed -> 200" "200" "$code"
check_contains "positive control status payload" 'capture_enabled' "$(cat "$TMP/b_pos")"

# Forged / unknown session ID
FORGED_SID="deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "Mcp-Session-Id: $FORGED_SID" \
  -d '{"jsonrpc":"2.0","id":67,"method":"tools/list"}')
check "forged 256-bit session ID -> 404" "404" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH" -H "Mcp-Session-Id: $FORGED_SID")
check "DELETE forged 256-bit session ID -> 404" "404" "$code"

# Independent Bob session creation
code=$(curl -s -D "$TMP/h_bob" -o "$TMP/b_bob" -w '%{http_code}' -X POST "$URL" \
  -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" \
  -d '{"jsonrpc":"2.0","id":68,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"bob","version":"1"}}}')
check "Bob initialize returns 200" "200" "$code"
SID_BOB=$(grep -i '^mcp-session-id:' "$TMP/h_bob" | tr -d '\r' | awk '{print $2}')
check "Bob session ID length 64" "64" "${#SID_BOB}"
if [ "$SID_BOB" != "$SID" ]; then
  printf '  PASS  %-58s distinct\n' "Bob and Alice have distinct session IDs"; PASS=$((PASS+1));
else
  printf '  FAIL  %-58s identical (%s)\n' "Bob and Alice have distinct session IDs" "$SID_BOB"; FAIL=$((FAIL+1));
fi

# Bob can use his own session
code=$(curl -s -o "$TMP/b_bob_call" -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "Mcp-Session-Id: $SID_BOB" \
  -d '{"jsonrpc":"2.0","id":69,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}')
check "Bob uses Bob session -> 200" "200" "$code"

# Symmetric isolation: Alice cannot use Bob's session
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "Mcp-Session-Id: $SID_BOB" \
  -d '{"jsonrpc":"2.0","id":70,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}')
check "Alice cannot access Bob session -> 403" "403" "$code"

# Bob can delete his own session
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH_BOB" -H "Mcp-Session-Id: $SID_BOB")
check "Bob DELETE Bob session -> 200" "200" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "Mcp-Session-Id: $SID_BOB" \
  -d '{"jsonrpc":"2.0","id":71,"method":"tools/list"}')
check "Bob session terminated -> 404" "404" "$code"

echo "== 10. Origin validation"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -H 'Origin: http://evil.example' -d '{"jsonrpc":"2.0","id":8,"method":"ping"}')
check "disallowed Origin -> 403" "403" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -H "Origin: http://127.0.0.1:$PORT" -d '{"jsonrpc":"2.0","id":9,"method":"ping"}')
check "allowlisted Origin -> 200" "200" "$code"

echo "== 11. error taxonomy over the wire"
code=$(curl -s -o "$TMP/b11" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" -d '{bad json')
check_contains "malformed JSON -> -32700" '-32700' "$(cat "$TMP/b11")"
code=$(curl -s -o "$TMP/b12" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":11,"method":"resources/list"}')
check_contains "unsupported method -> -32601" '-32601' "$(cat "$TMP/b12")"
code=$(curl -s -o "$TMP/b13" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"nope"}}')
check_contains "unknown tool -> -32602" '-32602' "$(cat "$TMP/b13")"

echo "== 12. session and transport rules"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H 'Mcp-Session-Id: deadbeef' \
  -d '{"jsonrpc":"2.0","id":13,"method":"tools/list"}')
check "unknown session -> 404" "404" "$code"
code=$(curl -s -o "$TMP/b14" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" \
  -d '{"jsonrpc":"2.0","id":14,"method":"tools/list"}')
check "no session on a non-initialize request -> 400" "400" "$code"
code=$(curl -s -D "$TMP/h15" -o /dev/null -w '%{http_code}' -X GET "$URL" -H "$AUTH" -H "$ACCEPT")
check "GET /mcp -> 405" "405" "$code"
check_contains "405 advertises Allow" 'POST' "$(cat "$TMP/h15")"
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/../etc/passwd" -X POST -H "$AUTH" -H "$JSON" -d '{}')
if [ "$code" = "404" ] || [ "$code" = "400" ]; then printf '  PASS  %-58s %s\n' "unknown path refused" "$code"; PASS=$((PASS+1));
else printf '  FAIL  unknown path returned %s\n' "$code"; FAIL=$((FAIL+1)); fi

echo "== 13. request bounds"
big=$(python3 -c "print('{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":\"' + 'a'*70000 + '\"}}')")
code=$(printf '%s' "$big" | curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" --data-binary @-)
check "oversize body -> 413" "413" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H 'Content-Type: text/plain' -H "$ACCEPT" -H "$SESS" -d '{}')
check "wrong content type -> 415" "415" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -H 'MCP-Protocol-Version: 1999-01-01' -d '{"jsonrpc":"2.0","id":15,"method":"ping"}')
check "unsupported protocol version header -> 400" "400" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":151,"method":"ping"}')
check "duplicate Authorization header -> 400" "400" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":152,"method":"ping"}')
check "duplicate Mcp-Session-Id header -> 400" "400" "$code"

t_start=$(date +%s)
code=$(python3 -c "import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $PORT))
s.sendall(b'POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n')
s.settimeout(6.0)
try:
    resp = s.recv(1024).decode('utf-8', errors='ignore')
    if '408' in resp:
        print('408')
    else:
        print('other')
except Exception:
    print('timeout')
finally:
    s.close()
")
t_elapsed=$(( $(date +%s) - t_start ))
check "incomplete header times out with 408" "408" "$code"
if [ "$t_elapsed" -le 5 ]; then
  printf '  PASS  %-58s %ds <= 5s\n' "slowloris socket freed within deadline" "$t_elapsed"; PASS=$((PASS+1));
else
  printf '  FAIL  %-58s took %ds > 5s\n' "slowloris socket freed within deadline" "$t_elapsed"; FAIL=$((FAIL+1));
fi

echo "== 14. SSE-only client still receives its response"
code=$(curl -s -o "$TMP/b16" -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H 'Accept: text/event-stream' -H "$SESS" \
  -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","id":16,"method":"ping"}')
check "SSE-only client -> 200" "200" "$code"
check_contains "SSE framing" 'event: message' "$(cat "$TMP/b16")"

echo "== 15. DELETE terminates the session, and it is then unknown"
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH" -H "$SESS")
check "DELETE session -> 200" "200" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":17,"method":"tools/list"}')
check "terminated session -> 404" "404" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL" -H "$AUTH_BOB" -H "$JSON" -H "$ACCEPT" -H "$SESS" \
  -d '{"jsonrpc":"2.0","id":18,"method":"tools/list"}')
check "terminated session unknown to other principals -> 404" "404" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH")
check "DELETE without session header -> 400" "400" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$URL" -H "$AUTH" -H "$SESS")
check "DELETE already-terminated session -> 404" "404" "$code"

echo "== 16. clean shutdown on SIGTERM"
kill -TERM "$SERVER_PID" 2>/dev/null
gone=0
for _ in $(seq 1 40); do kill -0 "$SERVER_PID" 2>/dev/null || { gone=1; break; }; sleep 0.25; done
check "server exits on SIGTERM" "1" "$gone"
check_contains "shutdown logged a request count" 'requests=' "$(cat "$TMP/server.log")"
if grep -qiE "leak|segmentation|SIGSEGV|terminate called|assert" "$TMP/server.log"; then
  printf '  FAIL  server log mentions a crash signal\n'; FAIL=$((FAIL+1))
else printf '  PASS  %-58s clean\n' "no crash signal in the server log"; PASS=$((PASS+1)); fi
SERVER_PID=""

echo
echo "== SUMMARY: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
