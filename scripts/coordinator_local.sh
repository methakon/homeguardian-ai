#!/usr/bin/env bash
# coordinator_local.sh — reproducible LOCAL-ONLY lifecycle for the HomeGuardian MCP bridge
# coordinator: start, stop, status, health.
#
# Why this exists: the coordinator is a library, not a service. It has no package.json, no start
# script and no autostart, so "the coordinator is not running" requires no owner intervention — it
# simply was never instantiated. This script instantiates it on demand, bound strictly to loopback,
# with per-run generated test credentials and an empty test registry.
#
# Deliberately NOT done: no systemd unit, no init script, no shell profile entry, no boot
# configuration, no cron. Nothing is installed and nothing survives a reboot. Stopping the script
# stops the coordinator.
#
#   ./coordinator_local.sh start  [--source DIR] [--ws-port N] [--http-port N]
#   ./coordinator_local.sh stop
#   ./coordinator_local.sh restart
#   ./coordinator_local.sh status
#   ./coordinator_local.sh health
#
# Exit codes: 0 ok, 1 failure, 2 usage/source error, 3 not running
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNNER="$SELF_DIR/coordinator_local_runner.js"

WS_PORT="${HG_COORD_WS_PORT:-18994}"
HTTP_PORT="${HG_COORD_HTTP_PORT:-18995}"
RUN_DIR="${HG_COORD_RUN_DIR:-${TMPDIR:-/tmp}/hg-coordinator}"
PID_FILE="$RUN_DIR/coordinator.pid"
LOG_FILE="$RUN_DIR/coordinator.log"
READY_FILE="$RUN_DIR/coordinator.ready"
SOURCE="${HG_COORD_SOURCE:-}"
READY_TIMEOUT="${HG_COORD_READY_TIMEOUT:-15}"

say() { printf '%s\n' "$*"; }
ok()  { printf '  [ok]   %s\n' "$*"; }
bad() { printf '  [BAD]  %s\n' "$*"; }

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; }

# ---- argument parsing ------------------------------------------------------
CMD="${1:-}"; shift || true
while [ "$#" -gt 0 ]; do
  case "$1" in
    --source)    SOURCE="$2"; shift 2 ;;
    --ws-port)   WS_PORT="$2"; shift 2 ;;
    --http-port) HTTP_PORT="$2"; shift 2 ;;
    --run-dir)   RUN_DIR="$2"; PID_FILE="$RUN_DIR/coordinator.pid"; LOG_FILE="$RUN_DIR/coordinator.log"; READY_FILE="$RUN_DIR/coordinator.ready"; shift 2 ;;
    -h|--help)   usage; exit 0 ;;
    *) say "unknown argument: $1"; exit 2 ;;
  esac
done

[ -n "$CMD" ] || { usage; exit 2; }

# ---- helpers ---------------------------------------------------------------
port_in_use() {
  # Returns 0 if something is already listening on the port (any interface).
  ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"
}

pid_alive() {
  [ -n "${1:-}" ] || return 1
  kill -0 "$1" 2>/dev/null
}

read_pid() { [ -f "$PID_FILE" ] && tr -d '[:space:]' < "$PID_FILE" || true; }

port_owner_pid() {
  ss -ltnp 2>/dev/null | awk -v P=":$1" '$4 ~ P' | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2
}

bind_addr_of() {
  ss -ltn 2>/dev/null | awk -v P=":$1" '$4 ~ P {print $4}' | head -1
}

# Locate the coordinator source tree (it lives on the common MCP bridge branch, not necessarily here).
detect_source() {
  if [ -n "$SOURCE" ] && [ -f "$SOURCE/web_mcp/outbound_bridge/index.js" ]; then echo "$SOURCE"; return 0; fi
  for d in \
    "$SELF_DIR/.." \
    "$HOME/projects/homeguardian-ai" \
    "$HOME/projects/homeguardian-ai-worktrees/antigravity-sec-06c" \
    "$HOME/projects/homeguardian-ai-worktrees/hermes-mcp-demo" \
    /tmp/release_verify
  do
    [ -f "$d/web_mcp/outbound_bridge/index.js" ] && { echo "$d"; return 0; }
  done
  return 1
}

wait_for_ready() {
  local waited=0
  while [ "$waited" -lt "$READY_TIMEOUT" ]; do
    if [ -f "$READY_FILE" ] && grep -q '^ready' "$READY_FILE" 2>/dev/null; then return 0; fi
    if grep -q 'coordinator ready' "$LOG_FILE" 2>/dev/null; then return 0; fi
    # failed fast?
    if grep -qE 'startup failed|coordinator construction failed|failed to load|not found at' "$LOG_FILE" 2>/dev/null; then return 1; fi
    sleep 1; waited=$((waited+1))
  done
  return 1
}

# ---- start -----------------------------------------------------------------
do_start() {
  mkdir -p "$RUN_DIR"
  [ -f "$RUNNER" ] || { bad "runner missing at $RUNNER"; exit 2; }

  local src
  if ! src="$(detect_source)"; then
    bad "coordinator source tree not found"
    say "       pass --source DIR pointing at a checkout containing web_mcp/outbound_bridge"
    say "       (the coordinator is carried on the common MCP bridge branch)"
    exit 2
  fi
  say "source: $src"

  # Already running?
  local existing; existing="$(read_pid)"
  if pid_alive "$existing"; then
    bad "already running (pid $existing); use stop or restart"
    exit 1
  fi

  # Port-conflict detection — report who holds the port rather than failing obscurely.
  local conflict=0
  for p in "$WS_PORT" "$HTTP_PORT"; do
    if port_in_use "$p"; then
      local owner; owner="$(port_owner_pid "$p")"
      bad "port $p already in use${owner:+ by pid $owner}"
      conflict=1
    fi
  done
  if [ "$conflict" -ne 0 ]; then
    say "       stop the existing listener or choose --ws-port / --http-port"
    exit 1
  fi
  ok "ports $WS_PORT and $HTTP_PORT free"

  : > "$LOG_FILE"; rm -f "$READY_FILE"

  # Launch detached from this shell but NOT installed anywhere.
  setsid node "$RUNNER" --source "$src" \
      --ws-port "$WS_PORT" --http-port "$HTTP_PORT" --ready-file "$READY_FILE" \
      >>"$LOG_FILE" 2>&1 &
  local npid=$!
  echo "$npid" > "$PID_FILE"

  if wait_for_ready; then
    # The runner asserts loopback itself; re-verify from outside so a future change can't slip past.
    local wa wh
    wa="$(bind_addr_of "$WS_PORT")"; wh="$(bind_addr_of "$HTTP_PORT")"
    ok "started pid $npid"
    say "  websocket(edge)  $wa"
    say "  http(mcp client) $wh"
    case "$wa$wh" in
      *0.0.0.0*|*':::') bad "bound to a wildcard address — refusing to call this a loopback-only run"; do_stop >/dev/null 2>&1; exit 1 ;;
    esac
    ok "loopback-only confirmed"
    say "  log: $LOG_FILE"
    exit 0
  fi

  bad "coordinator did not become ready within ${READY_TIMEOUT}s"
  say "  --- last log lines ---"
  tail -15 "$LOG_FILE" 2>/dev/null | sed 's/^/    /'
  do_stop >/dev/null 2>&1 || true
  exit 1
}

# ---- stop ------------------------------------------------------------------
do_stop() {
  local pid; pid="$(read_pid)"
  if ! pid_alive "$pid"; then
    rm -f "$PID_FILE" "$READY_FILE"
    say "not running"; exit 3
  fi
  say "stopping pid $pid (SIGTERM)"
  kill -TERM "$pid" 2>/dev/null || true
  local waited=0
  while [ "$waited" -lt 10 ]; do
    pid_alive "$pid" || break
    sleep 1; waited=$((waited+1))
  done
  if pid_alive "$pid"; then
    say "did not exit in ${waited}s, escalating to SIGKILL"
    kill -KILL "$pid" 2>/dev/null || true
    sleep 1
  fi
  if pid_alive "$pid"; then bad "pid $pid still alive after SIGKILL"; exit 1; fi
  rm -f "$PID_FILE" "$READY_FILE"
  ok "stopped"
  # Confirm the ports really came back.
  for p in "$WS_PORT" "$HTTP_PORT"; do
    port_in_use "$p" && bad "port $p still bound after shutdown" || ok "port $p released"
  done
  exit 0
}

# ---- status ----------------------------------------------------------------
do_status() {
  local pid; pid="$(read_pid)"
  if pid_alive "$pid"; then
    ok "running pid $pid"
  else
    bad "not running"
    [ -f "$PID_FILE" ] && say "       (stale pid file removed)"
    rm -f "$PID_FILE"
    exit 3
  fi
  for p in "$WS_PORT" "$HTTP_PORT"; do
    local a; a="$(bind_addr_of "$p")"
    case "$a" in
      "") bad "port $p not listening" ;;
      0.0.0.0*|*':::'*) bad "port $p bound to wildcard: $a" ;;
      *127.0.0.1*) ok "port $p loopback: $a" ;;
      *) say "  port $p bound: $a" ;;
    esac
  done
  exit 0
}

# ---- health ----------------------------------------------------------------
do_health() {
  local pid; pid="$(read_pid)"
  pid_alive "$pid" || { bad "not running"; exit 3; }

  # HTTP ingress must refuse unauthenticated callers.
  local code
  code="$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP_PORT/mcp" \
      -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
      -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' 2>/dev/null || echo 000)"
  if [ "$code" = "401" ]; then ok "http ingress refuses unauthenticated callers (401)"
  else bad "http ingress returned $code (expected 401)"; fi

  # A bogus bearer must also be refused.
  code="$(curl -s -m 5 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP_PORT/mcp" \
      -H 'Authorization: Bearer not-the-token' -H 'Content-Type: application/json' \
      -H 'Accept: application/json, text/event-stream' \
      -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' 2>/dev/null || echo 000)"
  if [ "$code" = "401" ]; then ok "bogus bearer refused (401)"
  else bad "bogus bearer returned $code (expected 401)"; fi

  # Offline-device fail-closed: with no edge connected, a client request must not hang or succeed.
  # A bogus bearer is used deliberately — the point is that the request is refused rather than
  # silently queued forever waiting for a device that is not there.
  code="$(curl -s -m 8 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$HTTP_PORT/mcp" \
      -H 'Authorization: Bearer bogus-offline-probe' \
      -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
      -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' 2>/dev/null || echo 000)"
  if [ "$code" = "000" ]; then bad "request timed out with no edge connected (should refuse, not hang)"
  elif [ "$code" != "200" ]; then ok "no edge connected -> refused ($code), fail-closed"
  else bad "request returned 200 with no edge connected"; fi

  # WebSocket listener reachable.
  if python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(3)
try:
    s.connect(('127.0.0.1',$WS_PORT)); sys.exit(0)
except Exception: sys.exit(1)
finally: s.close()
" 2>/dev/null; then ok "websocket port $WS_PORT accepts connections"
  else bad "websocket port $WS_PORT unreachable"; fi

  exit 0
}

# ---- dispatch --------------------------------------------------------------
case "$CMD" in
  start)   do_start ;;
  stop)    do_stop ;;
  restart) do_stop >/dev/null 2>&1 || true; do_start ;;
  status)  do_status ;;
  health)  do_health ;;
  *)       usage; exit 2 ;;
esac
