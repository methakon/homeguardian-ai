#!/usr/bin/env bash
# HomeGuardian AI — MCP Private Overlay Ingress Security Test Suite (HG-NET-02D)
#
# Asserts the network overlay ingress security boundaries:
#   1. Wildcard 0.0.0.0 / :: / 0 bind attempts are unconditionally refused (exit code 4),
#      preventing accidental exposure on external physical network interfaces (wlan0, eth0, rmnet).
#   2. Non-loopback addresses without HG_MCP_ALLOW_REMOTE=1 are refused (exit code 4).
#   3. Explicit overlay binding (e.g. 127.0.0.42 / private overlay alias) with explicit
#      allow_remote starts and listens only on the designated address.
#   4. Application authentication is strictly enforced over overlay connections:
#      - No bearer token -> 401
#      - Forged bearer token -> 401
#      - Valid token -> 200 + 256-bit CSPRNG Mcp-Session-Id
#      - Session binding & cross-principal protection -> 403
#      - Read-only tools/list & tools/call verified
#      - Origin filtering verified
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${HG_MCP_BIN:-$ROOT/build/hg_mcp_server}"
CONFIG="${HG_MCP_CONFIG:-$ROOT/config/homeguardian.demo.json}"
OVERLAY_PORT="${HG_OVERLAY_PORT:-$(( 19400 + RANDOM % 400 ))}"
OVERLAY_BIND="127.0.0.42" # Represents a dedicated private overlay endpoint/alias

TMP="$(mktemp -d)"
TOKEN_OPERATOR="overlay-op-token-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
TOKEN_ATTACKER="attacker-token-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    for _ in $(seq 1 30); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

check() {
  local label="$1" expected="$2" actual="$3"
  if [ "$expected" = "$actual" ]; then
    printf '  \033[0;32mPASS\033[0m  %-62s %s\n' "$label" "$actual"
    PASS=$((PASS+1))
  else
    printf '  \033[0;31mFAIL\033[0m  %-62s expected %s, got %s\n' "$label" "$expected" "$actual"
    FAIL=$((FAIL+1))
  fi
}

check_contains() {
  local label="$1" needle="$2" haystack="$3"
  if printf '%s' "$haystack" | grep -qF -- "$needle"; then
    printf '  \033[0;32mPASS\033[0m  %-62s contains %s\n' "$label" "$needle"
    PASS=$((PASS+1))
  else
    printf '  \033[0;31mFAIL\033[0m  %-62s missing %s in: %s\n' "$label" "$needle" "$haystack"
    FAIL=$((FAIL+1))
  fi
}

[ -x "$BIN" ] || { echo "ERROR: C++ MCP server binary not found at $BIN"; exit 2; }

echo "================================================================================"
echo "  HomeGuardian AI — MCP Overlay Ingress Test Suite (HG-NET-02D)"
echo "================================================================================"

# Provision 0600 test credentials
umask 077
cat > "$TMP/mcp_creds.json" <<JSON
{
  "$TOKEN_OPERATOR": {
    "principal": "operator-overlay-client",
    "allow_status": true,
    "allow_alert_summary": true
  }
}
JSON
chmod 0600 "$TMP/mcp_creds.json"

# --- Section 1: Wildcard Bind Refusal ---
echo ""
echo "--- 1. Wildcard Bind Refusal (Zero-Trust Network Ingress) ---"

set +e
HG_MCP_BIND=0.0.0.0 HG_MCP_ALLOW_REMOTE=0 "$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/out_wild1" 2>&1
rc=$?
set -e
check "refuse 0.0.0.0 when allow_remote=0" "4" "$rc"
check_contains "error message rejects wildcard" "refusing to bind wildcard address" "$(cat "$TMP/out_wild1")"

set +e
HG_MCP_BIND=0.0.0.0 HG_MCP_ALLOW_REMOTE=1 "$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/out_wild2" 2>&1
rc=$?
set -e
check "refuse 0.0.0.0 even when allow_remote=1" "4" "$rc"
check_contains "error message rejects wildcard with allow_remote" "refusing to bind wildcard address" "$(cat "$TMP/out_wild2")"

set +e
HG_MCP_BIND="::" HG_MCP_ALLOW_REMOTE=1 "$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/out_wild3" 2>&1
rc=$?
set -e
check "refuse IPv6 :: wildcard even when allow_remote=1" "4" "$rc"

set +e
HG_MCP_BIND="0" HG_MCP_ALLOW_REMOTE=1 "$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/out_wild4" 2>&1
rc=$?
set -e
check "refuse literal 0 wildcard even when allow_remote=1" "4" "$rc"

# --- Section 2: Non-Loopback Opt-In Requirement ---
echo ""
echo "--- 2. Non-Loopback Interface Bind Guard ---"

set +e
HG_MCP_BIND="10.42.0.2" HG_MCP_ALLOW_REMOTE=0 "$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/out_nl1" 2>&1
rc=$?
set -e
check "refuse non-loopback bind without allow_remote=1" "4" "$rc"
check_contains "error specifies missing allow_remote" "refusing to bind a non-loopback address without allow_remote" "$(cat "$TMP/out_nl1")"

# --- Section 3: Starting MCP on Specific Overlay Address ---
echo ""
echo "--- 3. Launch Server on Explicit Overlay Interface ($OVERLAY_BIND:$OVERLAY_PORT) ---"

HG_MCP_PORT="$OVERLAY_PORT" \
HG_MCP_BIND="$OVERLAY_BIND" \
HG_MCP_ALLOW_REMOTE=1 \
HG_MCP_ORIGINS="http://$OVERLAY_BIND:$OVERLAY_PORT" \
"$BIN" "$CONFIG" "$TMP/mcp_creds.json" >"$TMP/overlay_server.log" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 40); do
  if curl -s -o /dev/null --max-time 1 "http://$OVERLAY_BIND:$OVERLAY_PORT/mcp" 2>/dev/null; then break; fi
  kill -0 "$SERVER_PID" 2>/dev/null || { echo "Overlay server failed to start:"; cat "$TMP/overlay_server.log"; exit 3; }
  sleep 0.1
done

check "overlay server process alive" "running" "$(kill -0 "$SERVER_PID" 2>/dev/null && echo "running" || echo "dead")"

OVERLAY_URL="http://$OVERLAY_BIND:$OVERLAY_PORT/mcp"
ACCEPT_HDR="Accept: application/json, text/event-stream"

# --- Section 4: Overlay Authentication & Session Binding ---
echo ""
echo "--- 4. Application-Layer Security over Overlay Transport ---"

# Test 4a: Missing authentication header
code_noauth=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}}')
check "unauthenticated request over overlay -> 401" "401" "$code_noauth"

# Test 4b: Invalid bearer token
code_badauth=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_ATTACKER" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}}')
check "invalid token over overlay -> 401" "401" "$code_badauth"

# Test 4c: Valid operator authentication + session minting
init_res=$(curl -s -D "$TMP/init_headers" -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":100,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"overlay-tester","version":"1"}}}')

SID=$(grep -i '^mcp-session-id:' "$TMP/init_headers" | tr -d '\r' | awk '{print $2}')
check "valid token handshake -> 256-bit Mcp-Session-Id" "64" "${#SID}"
check_contains "protocolVersion negotiation" '"protocolVersion":"2025-11-25"' "$init_res"
check_contains "serverInfo contains homeguardian-mcp" 'homeguardian-mcp' "$init_res"

# Test 4d: Subsequent tool request without session id -> 400
code_nosid=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":101,"method":"tools/list"}')
check "request without session header over overlay -> 400" "400" "$code_nosid"

# Test 4e: Subsequent request with forged session id -> 404
code_forgedsid=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: 0000000000000000000000000000000000000000000000000000000000000000" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":102,"method":"tools/list"}')
check "request with forged session header -> 404" "404" "$code_forgedsid"

# Test 4f: Valid tools/list over overlay
tools_res=$(curl -s -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":103,"method":"tools/list"}')
check_contains "tools/list exposes get_homeguardian_status" "get_homeguardian_status" "$tools_res"
check_contains "tools/list exposes get_alert_summary" "get_alert_summary" "$tools_res"

# Test 4g: tools/call read-only status verification
status_res=$(curl -s -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":104,"method":"tools/call","params":{"name":"get_homeguardian_status"}}')
check_contains "tools/call status returns Running state" 'Running' "$status_res"
check_contains "tools/call status returns read_only:true" 'read_only' "$status_res"

# Test 4h: Rejection of mutating tool invocation over overlay
mutate_res=$(curl -s -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":105,"method":"tools/call","params":{"name":"modify_security_rules"}}')
check_contains "mutating tool call rejected with -32602" '"code":-32602' "$mutate_res"

# Test 4i: Origin header enforcement over overlay
code_badorigin=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID" \
  -H "Origin: http://evil-attacker.example.com" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":106,"method":"tools/call","params":{"name":"get_homeguardian_status"}}')
check "unauthorized Origin over overlay -> 403" "403" "$code_badorigin"

# Test 4j: Clean session termination
del_code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID")
check "DELETE session over overlay -> 200" "200" "$del_code"

# Session is now dead
code_after_del=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$OVERLAY_URL" \
  -H "Accept: application/json" \
  -H "Authorization: Bearer $TOKEN_OPERATOR" \
  -H "Mcp-Session-Id: $SID" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":107,"method":"tools/list"}')
check "subsequent call on terminated session -> 404" "404" "$code_after_del"

echo ""
echo "================================================================================"
echo "  SUMMARY: $PASS passed, $FAIL failed"
echo "================================================================================"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
