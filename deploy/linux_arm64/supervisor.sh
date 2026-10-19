#!/usr/bin/env bash
# supervisor.sh — user-space process supervision prototype for the HomeGuardian MCP engine under
# Termux / PRoot Ubuntu.
#
# Why this is user-space and not a "service": under rootless Termux there is NO init, no systemd,
# and no ability to survive Android's process killer by privilege. This script restarts the engine
# if it dies and keeps a log, which is the honest limit of what user space can do. It is NOT
# equivalent to an Android foreground service, and it must not be described as one.
#
# Known limitation, stated up front: Android may kill the whole Termux session (and therefore this
# supervisor with it) at any time — on Doze, on memory pressure, or on battery optimisation. Nothing
# in user space can prevent that. Recovery depends on the user or an Android-side mechanism
# restarting Termux.
#
# Usage:  supervisor.sh start|stop|status|logs [config] [creds]
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"

# The command word comes first; config and creds follow. Extract the command BEFORE reading
# positional paths, otherwise `start` is consumed as the config filename.
CMD="${1:-}"
[ -n "$CMD" ] && shift
CONFIG="${1:-${HG_MCP_CONFIG:-$HERE/homeguardian.demo.json}}"
CREDS="${2:-${HG_MCP_CREDS:-$HERE/creds.json}}"

BIN="${HG_MCP_BIN:-$HERE/hg_mcp_server}"
PORT="${HG_MCP_PORT:-8790}"
BIND="127.0.0.1"                 # never configurable to 0.0.0.0 here
RUNDIR="${HG_MCP_RUNDIR:-$HERE/run}"
PIDFILE="$RUNDIR/supervisor.pid"
ENGINEPID="$RUNDIR/engine.pid"
LOG="$RUNDIR/engine.log"
SUPLOG="$RUNDIR/supervisor.log"
RESTART_DELAY="${HG_MCP_RESTART_DELAY:-5}"

mkdir -p "$RUNDIR"

say() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*"; }
suplog() { say "$*" >> "$SUPLOG"; }

preflight() {
  [ -x "$BIN" ]   || { say "engine not executable: $BIN"; return 1; }
  [ -f "$CONFIG" ] || { say "config missing: $CONFIG"; return 1; }
  [ -f "$CREDS" ]  || { say "creds missing: $CREDS (generate a per-run token; never copy production credentials)"; return 1; }
  case "$(stat -c%a "$CREDS" 2>/dev/null)" in
    600|400) ;;
    *) say "refusing: $CREDS must be mode 600 or stricter (found $(stat -c%a "$CREDS" 2>/dev/null))"; return 1 ;;
  esac
  return 0
}

start() {
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
    say "supervisor already running (pid $(cat "$PIDFILE"))"; return 1
  fi
  preflight || return 1
  : > "$LOG"
  # The supervisor loop itself is backgrounded; it owns the engine's lifecycle.
  (
    echo $$ > "$PIDFILE"
    suplog "supervisor started pid=$$ bin=$BIN port=$PORT bind=$BIND"
    while :; do
      HG_MCP_PORT="$PORT" HG_MCP_BIND="$BIND" HG_MCP_ALLOW_REMOTE=0 \
        "$BIN" "$CONFIG" "$CREDS" >> "$LOG" 2>&1 &
      ep=$!
      echo "$ep" > "$ENGINEPID"
      suplog "engine started pid=$ep"
      wait "$ep"
      rc=$?
      suplog "engine exited rc=$rc"
      # stop request?
      [ -f "$RUNDIR/stop" ] && { suplog "stop flag present, supervisor exiting"; break; }
      sleep "$RESTART_DELAY"
    done
    rm -f "$PIDFILE" "$ENGINEPID"
  ) >/dev/null 2>&1 &
  disown 2>/dev/null || true
  sleep 2
  say "started (supervisor pid $(cat "$PIDFILE" 2>/dev/null))"
  say "  engine pid: $(cat "$ENGINEPID" 2>/dev/null)"
  say "  log: $LOG    supervisor log: $SUPLOG"
}

stop() {
  touch "$RUNDIR/stop"
  if [ -f "$ENGINEPID" ]; then kill -TERM "$(cat "$ENGINEPID")" 2>/dev/null || true; fi
  sleep 2
  if [ -f "$PIDFILE" ]; then kill -TERM "$(cat "$PIDFILE")" 2>/dev/null || true; fi
  sleep 1
  rm -f "$RUNDIR/stop" "$PIDFILE" "$ENGINEPID"
  say "stopped"
}

status() {
  sp=$(cat "$PIDFILE" 2>/dev/null || echo "")
  ep=$(cat "$ENGINEPID" 2>/dev/null || echo "")
  if [ -n "$sp" ] && kill -0 "$sp" 2>/dev/null; then say "supervisor: RUNNING pid=$sp"; else say "supervisor: not running"; fi
  if [ -n "$ep" ] && kill -0 "$ep" 2>/dev/null; then say "engine    : RUNNING pid=$ep"; else say "engine    : not running"; fi
  say "listener  : $( (command -v ss >/dev/null && ss -ltn 2>/dev/null | grep ":$PORT") || echo "port $PORT not detectable (ss unavailable)")"
  echo "--- last 5 supervisor events ---"
  tail -5 "$SUPLOG" 2>/dev/null || echo "  (none)"
  echo "--- last 5 engine lines ---"
  tail -5 "$LOG" 2>/dev/null || echo "  (none)"
}

case "${CMD:-}" in
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  logs)   tail -40 "$LOG" 2>/dev/null; echo "--- supervisor ---"; tail -20 "$SUPLOG" 2>/dev/null ;;
  *) echo "usage: $0 start|stop|status|logs [config] [creds]"; exit 2 ;;
esac
