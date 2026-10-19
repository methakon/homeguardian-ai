#!/usr/bin/env bash
# coordinator_local_tests.sh — automated verification of scripts/coordinator_local.sh
#
# Covers the coordinator lifecycle and security properties that can be tested on this workstation:
#   T1  startup reaches ready
#   T2  HTTP listener bound to loopback
#   T3  WebSocket listener bound to loopback
#   T4  unauthenticated HTTP request refused
#   T5  bogus bearer refused
#   T6  offline-device fail-closed (refuses, does not hang or succeed)
#   T7  status reports running and loopback
#   T8  health check passes
#   T9  clean shutdown releases both ports
#   T10 repeated start/stop leaves no leaked processes
#   T11 restart works
#   T12 port-conflict detected rather than failing obscurely
#   T13 no dependence on Oracle or VPN (loopback only, zero external connections)
#
# Uses a dedicated port pair and run dir so it never collides with a real coordinator.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
CTL="$HERE/coordinator_local.sh"
WS=19994
HTTP=19995
RUN_DIR="${HG_COORD_TEST_RUN_DIR:-${TMPDIR:-/tmp}/hg-coordinator-test}"
SOURCE="${HG_COORD_SOURCE:-}"

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  [PASS] %s\n' "$*"; }
bad() { fail=$((fail+1)); printf '  [FAIL] %s\n' "$*"; }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 (got=$2 want=$3)"; fi; }

[ -f "$CTL" ] || { echo "controller not found: $CTL"; exit 2; }

ARGS=(--run-dir "$RUN_DIR" --ws-port "$WS" --http-port "$HTTP")
[ -n "$SOURCE" ] && ARGS+=(--source "$SOURCE")

cleanup() { bash "$CTL" stop "${ARGS[@]}" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "controller : $CTL"
echo "ports      : $WS / $HTTP   run-dir: $RUN_DIR"
echo

# ------------------------------------------------------------------ T12 (before start: ports must be free)
echo "=== T12 port conflict detection ==="
# Occupy the HTTP port with a throwaway listener, then expect `start` to refuse.
python3 -c "
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',$HTTP)); s.listen(1); time.sleep(6)
" &
BLOCKER=$!
sleep 1
out="$(bash "$CTL" start "${ARGS[@]}" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'already in use'; then
  ok "start refused with a clear port-conflict message"
else
  bad "port conflict not detected (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi
kill "$BLOCKER" 2>/dev/null || true
wait "$BLOCKER" 2>/dev/null || true
sleep 1

# ------------------------------------------------------------------ T1 startup
echo "=== T1 startup reaches ready ==="
out="$(bash "$CTL" start "${ARGS[@]}" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q 'loopback-only confirmed'; then
  ok "coordinator started and is loopback-only"
else
  bad "startup failed (rc=$rc)"
  printf '%s\n' "$out" | sed 's/^/       /'
fi
PID="$(tr -d '[:space:]' < "$RUN_DIR/coordinator.pid" 2>/dev/null || true)"
[ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && ok "pid file valid and process alive ($PID)" || bad "pid file missing or process dead"

# ------------------------------------------------------------------ T2/T3 listeners
echo "=== T2/T3 listener binding ==="
for p in "$WS" "$HTTP"; do
  a="$(ss -ltn 2>/dev/null | awk -v P=":$p" '$4 ~ P {print $4}' | head -1)"
  case "$a" in
    *127.0.0.1*) ok "port $p loopback ($a)" ;;
    0.0.0.0*|*':::') bad "port $p WILDCARD ($a)" ;;
    *) bad "port $p not listening (got '$a')" ;;
  esac
done

# ------------------------------------------------------------------ T4/T5 auth refusal
echo "=== T4/T5 authentication refusal ==="
c="$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP/mcp" \
  -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' 2>/dev/null || echo 000)"
chk "T4 unauthenticated refused" "$c" "401"
c="$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP/mcp" \
  -H 'Authorization: Bearer wrong-token' -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' 2>/dev/null || echo 000)"
chk "T5 bogus bearer refused" "$c" "401"

# ------------------------------------------------------------------ T6 offline fail-closed
echo "=== T6 offline-device fail-closed ==="
t0=$(date +%s)
c="$(curl -s -m 10 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP/mcp" \
  -H 'Authorization: Bearer offline-probe' -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}' 2>/dev/null || echo 000)"
t1=$(date +%s)
if [ "$c" != "200" ] && [ "$c" != "000" ]; then ok "no edge connected -> refused ($c) in $((t1-t0))s"
elif [ "$c" = "000" ]; then bad "request hung with no edge connected"
else bad "request succeeded with no edge connected"; fi

# ------------------------------------------------------------------ T7 status
echo "=== T7 status ==="
out="$(bash "$CTL" status "${ARGS[@]}" 2>&1)"; rc=$?
[ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q 'running pid' && ok "status reports running" || bad "status wrong (rc=$rc)"

# ------------------------------------------------------------------ T8 health
echo "=== T8 health check ==="
out="$(bash "$CTL" health "${ARGS[@]}" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q 'fail-closed'; then
  ok "health passed incl. offline fail-closed"
else
  bad "health check failed (rc=$rc)"; printf '%s\n' "$out" | sed 's/^/       /'
fi
printf '%s\n' "$out" | grep -q 'websocket port' && ok "health confirms websocket listener"

# ------------------------------------------------------------------ T13 no Oracle/VPN dependence
echo "=== T13 no Oracle/VPN dependence ==="
ext="$(ss -tnp 2>/dev/null | awk -v P="pid=$PID," '$0 ~ P' | grep -vE '127\.0\.0\.1|\[::1\]' | wc -l)"
chk "external (non-loopback) connections from the coordinator" "$ext" "0"
# Check the LOCAL address column (field 4) only. For a LISTEN socket the peer column is always
# 0.0.0.0:* — matching the whole line would flag every listening socket as wildcard-bound.
wild="$(ss -ltnp 2>/dev/null | awk -v P="pid=$PID," '$0 ~ P' | awk '{print $4}' | grep -cE '^0\.0\.0\.0:|^\[::\]:')"
chk "wildcard-bound listening sockets" "$wild" "0"

# ------------------------------------------------------------------ T9 clean shutdown
echo "=== T9 clean shutdown ==="
out="$(bash "$CTL" stop "${ARGS[@]}" 2>&1)"; rc=$?
[ "$rc" -eq 0 ] && ok "stop returned cleanly" || bad "stop rc=$rc"
sleep 1
kill -0 "$PID" 2>/dev/null && bad "process $PID survived shutdown" || ok "process exited"
for p in "$WS" "$HTTP"; do
  ss -ltn 2>/dev/null | awk -v P=":$p" '$4 ~ P' | grep -q . && bad "port $p still bound" || ok "port $p released"
done

# ------------------------------------------------------------------ T10/T11 repeated start/stop
echo "=== T10/T11 repeated cycles (leak check) + restart ==="
# `pgrep -c` exits non-zero when nothing matches, so `|| echo 0` would emit a second line and
# break the later integer comparison. Count via wc -l instead: always exactly one number.
count_runners() { pgrep -f 'coordinator_local_runner[.]js' 2>/dev/null | wc -l | tr -d ' '; }
before=$(count_runners)
for i in 1 2 3; do
  bash "$CTL" start "${ARGS[@]}" >/dev/null 2>&1
  sleep 1
  bash "$CTL" stop  "${ARGS[@]}" >/dev/null 2>&1
  sleep 1
done
after=$(count_runners)
if [ "$after" -le "$before" ]; then ok "no leaked processes after 3 cycles (before=$before after=$after)"
else bad "processes leaked: before=$before after=$after"; fi

bash "$CTL" start "${ARGS[@]}" >/dev/null 2>&1
out="$(bash "$CTL" restart "${ARGS[@]}" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then ok "restart works"; else bad "restart failed (rc=$rc)"; fi
bash "$CTL" stop "${ARGS[@]}" >/dev/null 2>&1 || true

echo
echo "=== coordinator local tests: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] || exit 1
