#!/usr/bin/env bash
# mi_a3_edge_verify.sh — functional MCP validation of the REAL HomeGuardian C++
# MCP server running ON the Mi A3, exercised from the host over an adb-forwarded
# LOOPBACK port only.
#
# Isolation: nothing here binds a LAN address. `adb forward` exposes the device's
# 127.0.0.1:8790 as the host's 127.0.0.1:<local>. The server itself refuses a
# non-loopback bind unless HG_MCP_ALLOW_REMOTE is set, which we never set.
#
# Usage: scripts/mi_a3_edge_verify.sh [serial] [local_port] [device_port]
set -uo pipefail

SERIAL="${1:-edd53dbd04de}"
LPORT="${2:-18790}"
DPORT="${3:-8790}"
DEV_DIR="${DEV_DIR:-/data/local/tmp/hg_edge}"
HGTMP="${HGTMP:-$HOME/.hermes/cache/scratch/hg-edge}"
mkdir -p "$HGTMP"
BASE="http://127.0.0.1:${LPORT}/mcp"

PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
check(){ # check <desc> <actual> <expected>
  if [ "$2" = "$3" ]; then ok "$1 (=$2)"; else bad "$1 (got '$2', want '$3')"; fi
}

adbs() { timeout 60 adb -s "$SERIAL" "$@"; }

echo "== Mi A3 edge MCP verification =="
echo "serial=$SERIAL  host 127.0.0.1:$LPORT -> device 127.0.0.1:$DPORT"

# ---- 0. bring the transport up -------------------------------------------------
adbs forward --remove tcp:"$LPORT" >/dev/null 2>&1
adbs forward tcp:"$LPORT" tcp:"$DPORT" >/dev/null 2>&1 || { echo "adb forward failed"; exit 1; }
TOK=$(adbs shell "cat $DEV_DIR/mcp_credentials.json" | python3 -c 'import sys,json; print(list(json.load(sys.stdin))[0])')
[ -n "$TOK" ] || { echo "could not read credential from device"; exit 1; }
AUTH="Authorization: Bearer $TOK"
CT="Content-Type: application/json"
AC="Accept: application/json, text/event-stream"

# POST returns "<http_code>\n<body>"
post() { # post <json> [extra curl args...]
  local body="$1"; shift
  curl -sS -o "$HGTMP/body.$$" -w '%{http_code}' -X POST "$BASE" \
       -H "$CT" -H "$AC" "$@" --data "$body"
  echo; cat "$HGTMP/body.$$" 2>/dev/null; rm -f "$HGTMP/body.$$"
}

echo
echo "== auth boundary =="
r=$(post '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' -D "$HGTMP/hdr.$$")
check "tools/list WITHOUT Authorization -> 401" "$(echo "$r" | head -1)" "401"
grep -qi 'www-authenticate: *Bearer' "$HGTMP/hdr.$$" && ok "401 carries WWW-Authenticate: Bearer" || bad "missing WWW-Authenticate"
r=$(post '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' -H "Authorization: Bearer wrong-token-000")
check "tools/list with WRONG bearer -> 401" "$(echo "$r" | head -1)" "401"
r=$(post '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' -H "$AUTH" -H "Origin: http://evil.example")
check "tools/list with foreign Origin -> 403" "$(echo "$r" | head -1)" "403"

echo
echo "== MCP handshake =="
r=$(post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"mi-a3-edge-verify","version":"1.0"}}}' \
        -H "$AUTH" -D "$HGTMP/h2.$$")
code=$(echo "$r" | head -1); body=$(echo "$r" | tail -n +2)
check "initialize -> 200" "$code" "200"
SID=$(grep -i '^mcp-session-id:' "$HGTMP/h2.$$" | tr -d '\r' | awk '{print $2}')
[ -n "$SID" ] && ok "Mcp-Session-Id minted" || bad "no Mcp-Session-Id"
echo "  server: $(echo "$body" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["result"]["serverInfo"],"proto",d["result"]["protocolVersion"])' 2>/dev/null)"
check "initialize advertises protocolVersion 2025-11-25" \
      "$(echo "$body" | python3 -c 'import sys,json;print(json.load(sys.stdin)["result"]["protocolVersion"])' 2>/dev/null)" "2025-11-25"

post '{"jsonrpc":"2.0","method":"notifications/initialized"}' -H "$AUTH" -H "Mcp-Session-Id: $SID" >/dev/null

# a non-initialize request with no session must be 400
r=$(post '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' -H "$AUTH")
check "tools/list with NO session -> 400" "$(echo "$r" | head -1)" "400"

echo
echo "== tools/list =="
r=$(post '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' -H "$AUTH" -H "Mcp-Session-Id: $SID")
check "tools/list -> 200" "$(echo "$r" | head -1)" "200"
echo "$r" | tail -n +2 | python3 -c '
import sys,json
t=json.load(sys.stdin)["result"]["tools"]
print("  tools:", [x["name"] for x in t])
' 2>/dev/null

echo
echo "== tools/call (real engine, read-only) =="
r=$(post "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get_homeguardian_status\",\"arguments\":{}}}" -H "$AUTH" -H "Mcp-Session-Id: $SID")
check "tools/call get_homeguardian_status -> 200" "$(echo "$r" | head -1)" "200"
echo "  payload: $(echo "$r" | tail -n +2 | head -c 400)"
r=$(post "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_alert_summary\",\"arguments\":{}}}" -H "$AUTH" -H "Mcp-Session-Id: $SID")
check "tools/call get_alert_summary -> 200" "$(echo "$r" | head -1)" "200"
echo "  payload: $(echo "$r" | tail -n +2 | head -c 400)"

echo
echo "== read-only contract is enforced =="
r=$(post '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"enable_capture","arguments":{}}}' -H "$AUTH" -H "Mcp-Session-Id: $SID")
echo "$r" | tail -n +2 | grep -q '\-32602' && ok "mutating tool name -> -32602 (no such tool)" || bad "mutating tool name not rejected"
r=$(post '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{"enable_capture":true}}}' -H "$AUTH" -H "Mcp-Session-Id: $SID")
echo "  arguments-injection attempt -> $(echo "$r" | tail -n +2 | head -c 200)"

echo
echo "== transport rules =="
check "GET /mcp -> 405" "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE" -H "$AUTH")" "405"
check "DELETE /mcp -> 200 (session terminated)" "$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$BASE" -H "$AUTH" -H "Mcp-Session-Id: $SID")" "200"
check "DELETE /mcp without session -> 400" "$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$BASE" -H "$AUTH")" "400"

# > 64 KiB body must be refused with 413. Generate the payload with python (a
# shell-quoted 70k literal is a quoting error waiting to happen).
PAYLOAD="$HGTMP/payload70k.json"
python3 -c 'import json,sys; json.dump({"jsonrpc":"2.0","id":7,"method":"tools/list","pad":"a"*70000}, open(sys.argv[1],"w"))' "$PAYLOAD"
check "POST with 70KiB body -> 413" \
      "$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$BASE" -H "$CT" -H "$AC" -H "$AUTH" --data-binary @"$PAYLOAD")" "413"

# Chunked request bodies are refused outright (501 + explicit reason), so there is
# no request-smuggling path via an ambiguous body length.
check "POST with chunked Transfer-Encoding -> 501 (refused)" \
      "$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$BASE" -H "$CT" -H "$AC" -H "$AUTH" -H "Transfer-Encoding: chunked" --data-binary '{"jsonrpc":"2.0"}' --http1.1)" "501"

# Raw socket: a body with NO Content-Length must be 411, not "wait for more".
check "raw POST with body and no Content-Length -> 411" \
      "$(python3 - "$LPORT" "$TOK" <<'PY'
import socket, sys
# Auth is checked BEFORE body framing, so the request must carry a valid bearer
# token or it would (correctly) stop at 401 and never reach the 411 path.
port, tok = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=5)
req = ("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       "Content-Type: application/json\r\nAccept: application/json\r\n"
       f"Authorization: Bearer {tok}\r\n\r\n" '{"jsonrpc":"2.0"}')
s.sendall(req.encode())
d = s.recv(200).decode(errors="replace")
s.close()
print(d.split()[1] if len(d.split()) > 1 else "no-response")
PY
)" "411"

echo
echo "== RESULT: $PASS passed, $FAIL failed =="
rm -f "$HGTMP/body.$$" "$HGTMP/hdr.$$" "$HGTMP/h2.$$"
[ "$FAIL" -eq 0 ]
