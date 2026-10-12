#!/usr/bin/env bash
# HomeGuardian AI — demo footprint benchmark (native vs legacy Node).
#
# Measures, for each implementation, over an identical workload:
#   - cold startup time (exec -> DEMO_READY)
#   - idle RSS and peak RSS
#   - process count attributable to the demo
#   - HTTP request latency (/api/health, /api/status, /api/run-scenario)
#   - executable size and runtime shared-library dependencies
#
# Usage:  bash scripts/bench_demo.sh
# Requires: a built hg_build/hg_alexa_bridge, curl, /usr/bin/time, node (legacy only).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Which native binary to measure (default: the local build dir). Point
# HG_BRIDGE_PATH at a Release binary for a fair comparison — a Debug build has
# very different size/RSS.
BRIDGE="${HG_BRIDGE_PATH:-$REPO_ROOT/hg_build/hg_alexa_bridge}"
CONFIG="$REPO_ROOT/config/homeguardian.demo.json"
DEMO_SERVER="$REPO_ROOT/src/bridge/hg_demo_server.js"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; kill %1 %2 2>/dev/null' EXIT

[[ -x "$BRIDGE" ]] || { echo "bridge not built at $BRIDGE"; exit 2; }
cat > "$WORK/creds.json" <<'JSON'
{"demo-token":{"principal":"demo","allow_status":true,"allow_alert_summary":true,"expires_at_ms":0,"revoked":false}}
JSON

port_of() { grep -oE 'DEMO_READY [0-9]+' "$1" | grep -oE '[0-9]+' | head -1; }
wait_ready() { for _ in $(seq 1 100); do grep -q DEMO_READY "$1" 2>/dev/null && return 0; sleep 0.05; done; return 1; }

# $(1)=label $(2)=cmd-string
measure_impl() {
  local label="$1"; shift
  local out="$WORK/$label.out" err="$WORK/$label.err"
  local t0 t1 startup pid port rss_idle rss_peak
  echo "==================== $label ===================="
  t0=$(date +%s.%N)
  ( eval "cd \"$WORK\" && exec $*" ) >"$out" 2>"$err" &
  pid=$!
  wait_ready "$out" || { echo "  did not become ready"; cat "$err"; return; }
  t1=$(date +%s.%N)
  startup=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
  port=$(port_of "$out")

  echo "  cold startup : ${startup} s"
  echo "  port         : $port"
  echo "  demo procs   : $(pgrep -fc "$label" 2>/dev/null || echo 0) matching '$label'"

  # Idle RSS
  if [ -n "$pid" ]; then
    rss_idle=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    echo "  idle RSS     : ${rss_idle:-?} kB"
  fi

  # HTTP latency (100 health requests)
  local lat
  lat=$( { for _ in $(seq 1 100); do curl -s -o /dev/null -w "%{time_total}\n" "http://127.0.0.1:$port/api/health"; done; } \
         | awk '{s=$1; n++; if($1>m)m=$1} END{printf "avg=%.4fs max=%.4fs n=%d", s/n, m, n}')
  echo "  /api/health  : $lat"
  lat=$( { for _ in $(seq 1 20); do curl -s -o /dev/null -w "%{time_total}\n" "http://127.0.0.1:$port/api/status"; done; } \
         | awk '{s=$1; n++; if($1>m)m=$1} END{printf "avg=%.4fs max=%.4fs n=%d", s/n, m, n}')
  echo "  /api/status  : $lat"
  curl -s -o /dev/null "http://127.0.0.1:$port/api/run-scenario?cycles=5"
  lat=$(curl -s -o /dev/null -w "%{time_total}" "http://127.0.0.1:$port/api/run-scenario?cycles=5")
  echo "  /api/run-scenario: ${lat}s (one run)"

  # Peak RSS after the scenario (VmHWM = high-water mark)
  if [ -n "$pid" ]; then
    rss_peak=$(awk '/VmHWM/{print $2}' "/proc/$pid/status" 2>/dev/null)
    echo "  peak RSS     : ${rss_peak:-?} kB (VmHWM)"
    echo "  CPU% (sample): $(ps -o %cpu= -p "$pid" 2>/dev/null | tr -d ' ')"
  fi

  kill "$pid" 2>/dev/null
  pkill -f "$label" 2>/dev/null
  sleep 0.3
}

measure_impl hg_native \
  "\"$BRIDGE\" --http \"$CONFIG\" \"$WORK/creds.json\" --port 8320 --token demo-token"

if command -v node >/dev/null 2>&1; then
  measure_impl node_legacy \
    "node \"$DEMO_SERVER\" --bridge \"$BRIDGE\" --config \"$CONFIG\" --creds \"$WORK/creds.json\" --port 8321 --token demo-token"
else
  echo "==================== node_legacy ===================="
  echo "  NOT RUN (node not installed in this environment)"
fi

echo "==================== static footprint ===================="
echo "  native binary : $(stat -c%s "$BRIDGE" 2>/dev/null) bytes"
echo "  native ldd    :"
ldd "$BRIDGE" 2>/dev/null | sed 's/^/      /'
echo "  dashboard     : embedded in the native binary (0 runtime web files)"
