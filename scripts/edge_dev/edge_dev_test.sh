#!/usr/bin/env bash
# MCP functional tests for the managed HomeGuardian edge service, over Wi-Fi.
#
#   edge_dev_test.sh
#
# Isolation: `adb forward tcp:18790 tcp:8790` exposes the DEVICE's
# 127.0.0.1:8790 on the HOST's 127.0.0.1:18790. Nothing binds a LAN address and
# the server still refuses a non-loopback bind.
#
# TWO EVIDENCE LAYERS, because they answer different questions:
#
#  A. HOST-SIDE, unauthenticated surface - proves the real server is reachable and
#     that its auth gate actually refuses. Needs no token, so it works against the
#     production (non-debuggable) build.
#
#  B. AUTHENTICATED protocol exchange (initialize -> tools/list -> tools/call).
#     The service's bearer token lives in the app's PRIVATE filesDir (0600, owned
#     by the app uid); `adb shell` cannot read it and this script will not weaken
#     that by copying a token to shared storage. Two supported ways to exercise it:
#       * the service's OWN watchdog performs exactly this cycle every 20 s against
#         its loopback server, using the real token, and logs the result - this
#         script pulls and validates that log (works on any build);
#       * on a DEBUGGABLE TEST BUILD, `run-as` can read the token, so set
#         HG_EDGE_DEBUGGABLE=1 and this script additionally performs the exchange
#         from the host. Never do that against a release build.
#
# Exit: 0 all checks passed, 7 any check failed.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

SERIAL="${HG_EDGE_TARGET:-$HG_EDGE_SERIAL}"
hg_assert_mi_a3 "$SERIAL"

HGTMP="${HGTMP:-$HOME/.hermes/cache/scratch/hg-edge}"
mkdir -p "$HGTMP"
BASE="http://127.0.0.1:${HG_EDGE_FWD_PORT}/mcp"
PASS=0; FAIL=0
ok()  { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
chk() { [[ "$2" == "$3" ]] && ok "$1 (=$2)" || bad "$1 (got '$2', want '$3')"; }

# ---- transport -------------------------------------------------------------
timeout 20 adb -s "$SERIAL" forward --remove "tcp:$HG_EDGE_FWD_PORT" >/dev/null 2>&1
timeout 20 adb -s "$SERIAL" forward "tcp:$HG_EDGE_FWD_PORT" "tcp:$HG_EDGE_MCP_PORT" >/dev/null 2>&1 \
  || hg_die "adb forward failed" 7

post() { # post <body> [curl args...]
  local body="$1"; shift
  curl -sS -o "$HGTMP/body.hg" -w '%{http_code}' --max-time 15 -X POST "$BASE" \
       -H 'Content-Type: application/json' \
       -H 'Accept: application/json, text/event-stream' "$@" -d "$body" 2>/dev/null
}
hdr() { # header of the last response
  curl -sS -D - -o /dev/null --max-time 15 -X POST "$BASE" \
       -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
       -d "${1:-{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}}" 2>/dev/null | tr -d '\r'
}

echo "== A. host-side surface (no token; production build safe) =="
# Order verified in src/mcp/McpHttpServer.cpp:
#   413 size (l.431) -> 501 chunked (l.441) -> 400 malformed (l.448/453)
#   -> 401 auth (l.480-504) -> 405 method (l.512) / 400 session (l.528)
# An UNAUTHENTICATED request can therefore only yield 413/501/400/401: method and
# session are validated AFTER auth, so asserting 405 here (as an earlier version of
# this script did) asserts behaviour that correctly cannot happen. Those two codes
# are checked in B2, where a real token is available.
chk "GET /mcp unauthenticated -> 401 (auth precedes method dispatch)" \
    "$(curl -sS -o /dev/null -w '%{http_code}' --max-time 15 "$BASE" 2>/dev/null)" "401"
chk "POST with no Authorization -> 401" \
    "$(post '{"jsonrpc":"2.0","id":1,"method":"initialize"}')" "401"
grep -qiE 'WWW-Authenticate: *Bearer' < <(hdr) && ok "401 advertises WWW-Authenticate: Bearer" \
                                              || bad "401 lacks WWW-Authenticate: Bearer"
chk "POST with a WRONG bearer -> 401" \
    "$(post '{"jsonrpc":"2.0","id":1,"method":"initialize"}' -H 'Authorization: Bearer not-the-real-token')" "401"
chk "DELETE unauthenticated -> 401 (session check is post-auth)" \
    "$(curl -sS -o /dev/null -w '%{http_code}' --max-time 15 -X DELETE "$BASE" 2>/dev/null)" "401"
python3 -c "print('x'*71234)" > "$HGTMP/big.hg" 2>/dev/null
chk "oversized body (70 KiB) -> 413 without needing auth (size guard precedes it)" \
    "$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 -X POST "$BASE" \
        -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
        --data-binary @"$HGTMP/big.hg" 2>/dev/null)" "413"

echo
echo "== loopback-only binding (on the device) =="
L="$(hg_adb "$SERIAL" shell 'cat /proc/net/tcp /proc/net/tcp6' | tr -d '\r' | awk '$4=="0A"{print $2}' | grep -i ':2256')"
if grep -q '0100007F:2256' <<<"$L"; then ok "MCP bound to 0100007F:2256 (127.0.0.1 only)"
else bad "MCP listener is not loopback-only: '$L'"; fi
# An all-interfaces bind would appear as 00000000:2256 (IPv4) or ::  [::]:2256.
if grep -qE '(^| )0{8}:2256|::]:2256|:{4}:2256' <<<"$L"; then
  bad "an all-interfaces listener exists - LAN EXPOSURE"
else ok "no all-interfaces MCP listener (LAN exposure absent)"; fi

echo
echo "== B1. authenticated exchange, as performed by the service's own watchdog =="
LOGF=/sdcard/Android/data/$HG_EDGE_PKG/files/probe.log
hg_adb "$SERIAL" shell "cat $LOGF" 2>/dev/null | tr -d '\r' > "$HGTMP/probe.hg"
N_OK=$(grep -c 'PROBE_OK' "$HGTMP/probe.hg" || true)
N_FAIL=$(grep -c 'PROBE_FAIL' "$HGTMP/probe.hg" || true)
(( N_OK > 0 )) && ok "watchdog completed $N_OK authenticated MCP cycles, $N_FAIL failures" \
               || bad "no PROBE_OK lines found in $LOGF"
grep -q 'proto=2025-11-25' "$HGTMP/probe.hg" && ok "negotiated MCP protocol version 2025-11-25" \
                                             || bad "no protocol version recorded"
grep -q 'tools=get_homeguardian_status,get_alert_summary' "$HGTMP/probe.hg" \
  && ok "tools/list returned both real tools" || bad "unexpected tools/list result"
grep -q 'status={"capture_enabled":false' "$HGTMP/probe.hg" \
  && ok "a real tools/call result was returned (capture_enabled=false as configured)" \
  || bad "no tools/call result visible"

echo
echo "== B2. host-side authenticated exchange (debuggable TEST build only) =="
if [[ "${HG_EDGE_DEBUGGABLE:-0}" == "1" ]]; then
  TOK="$(hg_adb "$SERIAL" shell "run-as $HG_EDGE_PKG cat files/mcp_credentials.json" 2>/dev/null \
         | tr -d '\r' | python3 -c 'import sys,json;print(list(json.load(sys.stdin))[0])' 2>/dev/null)"
  if [[ -z "$TOK" ]]; then
    bad "could not read the token via run-as (is this really a debuggable build?)"
  else
    ok "token read via run-as (never printed, never written to the log)"
    # initialize - capture the session id the server issues
    sid="$(curl -sS -D - -o "$HGTMP/init.hg" --max-time 20 -X POST "$BASE" \
           -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
           -H "Authorization: Bearer $TOK" \
           -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"edge-dev-test","version":"1"}}}' \
           2>/dev/null | tr -d '\r' | awk -F': ' '/^Mcp-Session-Id/{print $2}' | head -1)"
    [[ -n "$sid" ]] && ok "authenticated initialize returned a session id" \
                    || bad "authenticated initialize returned no session id"
    tl="$(curl -sS --max-time 20 -X POST "$BASE" -H 'Content-Type: application/json' \
          -H 'Accept: application/json, text/event-stream' -H "Authorization: Bearer $TOK" \
          ${sid:+-H "Mcp-Session-Id: $sid"} \
          -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' 2>/dev/null)"
    grep -q 'get_homeguardian_status' <<<"$tl" && ok "tools/list over the host returned the real tools" \
                                               || bad "tools/list over the host failed"
    tc="$(curl -sS --max-time 20 -X POST "$BASE" -H 'Content-Type: application/json' \
          -H 'Accept: application/json, text/event-stream' -H "Authorization: Bearer $TOK" \
          ${sid:+-H "Mcp-Session-Id: $sid"} \
          -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}' 2>/dev/null)"
    grep -q 'read_only' <<<"$tc" && ok "tools/call over the host returned a real status payload" \
                                 || bad "tools/call over the host failed"
    # Post-auth codes, only reachable with a valid token.
    chk "GET /mcp WITH a valid token -> 405 (method check is post-auth)" \
        "$(curl -sS -o /dev/null -w '%{http_code}' --max-time 15 "$BASE" \
            -H "Authorization: Bearer $TOK" 2>/dev/null)" "405"
    unset TOK sid
  fi
else
  echo "  SKIP  set HG_EDGE_DEBUGGABLE=1 to run this against a debuggable test build"
  echo "        (the production build's token is unreadable without root, by design)"
fi

echo
echo "== RESULT: $PASS passed, $FAIL failed =="
timeout 20 adb -s "$SERIAL" forward --remove "tcp:$HG_EDGE_FWD_PORT" >/dev/null 2>&1
[[ "$FAIL" -eq 0 ]] || exit 7
