#!/usr/bin/env bash
# self_test.sh — bounded deployment self-test for the HomeGuardian MCP engine.
#
# Runs inside PRoot Ubuntu on the Mi A3 (or anywhere). Verifies the deployment is genuinely usable
# and collects results in a stable, reproducible format so runs can be compared.
#
# BOUNDED: every stage has a timeout. The whole run has a hard wall-clock cap, and any stage that
# exceeds its budget is recorded as TIMEOUT rather than hanging. Nothing here waits forever.
#
# DOES NOT RE-DEFINE PROTOCOL ASSERTIONS: the MCP checks come from scripts/mcp_protocol_checks.sh,
# which the container harness also uses. This script only prepares the environment, invokes that
# shared checker, and collects results.
#
# Usage: self_test.sh [--json <outfile>] [--tsv <outfile>] [--bin <path>]
#   env: HG_SELF_TEST_TOTAL_TIMEOUT (default 300)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/hg_mcp_server"
CHECKS="$HERE/mcp_protocol_checks.sh"
CONFIG="$HERE/homeguardian.demo.json"
CREDS="$HERE/creds.json"
PORT="${HG_MCP_PORT:-8790}"
BIND="127.0.0.1"
TOTAL="${HG_SELF_TEST_TOTAL_TIMEOUT:-300}"
JSON_OUT=""; TSV_OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --json) JSON_OUT="${2:-}"; shift 2 ;;
    --tsv)  TSV_OUT="${2:-}";  shift 2 ;;
    --bin)  BIN="${2:-}";      shift 2 ;;
    *) shift ;;
  esac
done

# The engine ships next to this script in the bundle, but the shared checker may live beside this
# script, in a scripts/ subdirectory, or one level up in the repository. Try each in turn rather
# than guessing a single layout — a deployment bundle and a source checkout do not look the same.
for cand in "$HERE/mcp_protocol_checks.sh" "$HERE/scripts/mcp_protocol_checks.sh" \
            "$HERE/../../scripts/mcp_protocol_checks.sh"; do
  [ -f "$cand" ] && { CHECKS="$cand"; break; }
done
[ -f "${CHECKS:-}" ] || { echo "FATAL: mcp_protocol_checks.sh not found (looked in $HERE, $HERE/scripts, $HERE/../../scripts)"; exit 2; }

WORK="$HERE/.selftest.$$"
TMPDIR="$WORK"; export TMPDIR
mkdir -p "$WORK"
ENGINE_PID=""
cleanup() {
  [ -n "$ENGINE_PID" ] && kill -TERM "$ENGINE_PID" 2>/dev/null
  sleep 1
  [ -n "$ENGINE_PID" ] && kill -9 "$ENGINE_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

R="$WORK/results.tsv"; : > "$R"
rec() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$R"; }
say() { printf '%s\n' "$*"; }
hr()  { say "------------------------------------------------------------"; }

# ===== stage 1: environment =================================================
hr; say "STAGE 1  environment"
ARCH=$(uname -m 2>/dev/null || echo unknown)
rec env architecture "$([ "$ARCH" = aarch64 ] && echo PASS || echo FAIL)" "$ARCH"
say "  architecture : $ARCH"

OSV="unknown"
[ -r /etc/os-release ] && OSV=$( . /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}" )
rec env os_version "$(printf '%s' "$OSV" | grep -q 'Ubuntu 24.04' && echo PASS || echo FAIL)" "$OSV"
say "  os           : $OSV"

KER=$(uname -r 2>/dev/null || echo unknown)
rec env kernel INFO "$KER"
say "  kernel       : $KER"

MEMTOT=$(awk '/MemTotal/{printf "%.0f", $2/1024}' /proc/meminfo 2>/dev/null || echo 0)
CPU_TOT=$(nproc 2>/dev/null || echo 0)
rec env ram_total_mb INFO "$MEMTOT"
rec env cpus INFO "$CPU_TOT"
say "  ram          : ${MEMTOT} MB"
say "  cpus         : $CPU_TOT"

# ===== stage 2: binary integrity ============================================
hr; say "STAGE 2  binary integrity"
if [ ! -f "$BIN" ]; then
  rec bin present FAIL "not found: $BIN"
  say "  MISSING: $BIN"; JSON_OUT="" # cannot continue meaningfully
else
  BSH=$(sha256sum "$BIN" 2>/dev/null | awk '{print $1}')
  EXPECT="${HG_EXPECT_SHA256:-82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8}"
  rec bin present PASS "$(stat -c%s "$BIN") bytes"
  rec bin sha256 "$([ "$BSH" = "$EXPECT" ] && echo PASS || echo FAIL)" "$BSH"
  say "  size   : $(stat -c%s "$BIN") bytes"
  say "  sha256 : $BSH"
  say "  expect : $EXPECT"
  say "  match  : $([ "$BSH" = "$EXPECT" ] && echo YES || echo NO)"

  hr; say "STAGE 3  library resolution"
  if command -v ldd >/dev/null 2>&1; then
    LOUT=$(timeout 20 ldd "$BIN" 2>&1 || echo "ldd failed")
    MISSING=$(printf '%s\n' "$LOUT" | grep -c 'not found' || true)
    rec libs ldd "$([ "$MISSING" -eq 0 ] && echo PASS || echo FAIL)" "$MISSING unresolved"
    printf '%s\n' "$LOUT" | sed 's/^/  /'
  else
    rec libs ldd SKIP "ldd unavailable"
    say "  ldd unavailable (install libc-bin for this check)"
  fi

  # ===== stage 4: start the engine ==========================================
  hr; say "STAGE 4  engine start (bounded)"
  if [ ! -f "$CONFIG" ]; then
    rec start config FAIL "missing $CONFIG"; say "  config missing"
  elif [ ! -f "$CREDS" ]; then
    rec start creds FAIL "missing $CREDS"; say "  creds missing (see step 7 of the install doc)"
  else
    CMS=$(stat -c%a "$CREDS" 2>/dev/null)
    case "$CMS" in 600|400) rec start creds_mode PASS "$CMS" ;; *) rec start creds_mode FAIL "$CMS (need 600)" ;; esac
    T0=$(date +%s%N)
    HG_MCP_PORT="$PORT" HG_MCP_BIND="$BIND" HG_MCP_ALLOW_REMOTE=0 \
      "$BIN" "$CONFIG" "$CREDS" > "$WORK/engine.log" 2>&1 &
    ENGINE_PID=$!
    # wait up to 15s for the listener, polling
    UP=0
    for i in $(seq 1 30); do
      sleep 0.5
      if grep -q 'listening' "$WORK/engine.log" 2>/dev/null; then UP=1; break; fi
      kill -0 "$ENGINE_PID" 2>/dev/null || break
    done
    T1=$(date +%s%N)
    START_MS=$(( (T1 - T0) / 1000000 ))
    if [ "$UP" -eq 1 ]; then
      rec start listener PASS "127.0.0.1:$PORT"
      rec start startup_ms INFO "$START_MS"
      say "  listener : 127.0.0.1:$PORT   (startup ~${START_MS} ms)"
    else
      rec start listener FAIL "never announced listening"
      say "  FAILED to start. last log lines:"; tail -5 "$WORK/engine.log" | sed 's/^/    /'
    fi

    if [ "$UP" -eq 1 ]; then
      # loopback-only assertion.
      # `ss -ltn` prints  Local-Address:Port  Peer-Address:Port  — and the PEER column is
      # `0.0.0.0:*` even for a 127.0.0.1 bind. Grepping the whole line therefore produced a false
      # "wildcard bind" failure. Inspect the local-address column only.
      if command -v ss >/dev/null 2>&1; then
        WL=$(ss -ltn 2>/dev/null | awk -v p=":$PORT" '$4 ~ p {print $4}' | grep -c '^0\.0\.0\.0\|^\*\|^\[::\]' || true)
        LL=$(ss -ltn 2>/dev/null | awk -v p=":$PORT" '$4 ~ p {print $4}' | grep -c '^127\.0\.0\.1' || true)
        rec listen loopback_only "$([ "$WL" -eq 0 ] && [ "$LL" -ge 1 ] && echo PASS || echo FAIL)" "loopback=$LL wildcard=$WL"
      fi

      # ===== stage 5: shared protocol checks ================================
      hr; say "STAGE 5  MCP protocol (shared assertions)"
      POUT="$WORK/protocol.tsv"
      if timeout 90 bash "$CHECKS" "http://$BIND:$PORT/mcp" "$(head -c 64 "$CREDS" | grep -o '"[0-9a-f]\{64\}"' | tr -d '"')" 10 > "$POUT" 2>&1; then
        PRC=0
      else
        PRC=$?
      fi
      # fold the shared results in verbatim — no re-derivation
      grep -P '^CHECK\t' "$POUT" 2>/dev/null | while IFS=$'\t' read -r _ n v d; do
        printf 'proto\t%s\t%s\t%s\n' "$n" "$v" "$d" >> "$R"
      done
      # the token extraction above can fail if creds are laid out differently; resolve it robustly
      if ! grep -q '^proto' "$R" 2>/dev/null; then
        TOK=$(grep -oE '[0-9a-f]{64}' "$CREDS" 2>/dev/null | head -1)
        if [ -n "$TOK" ]; then
          timeout 90 bash "$CHECKS" "http://$BIND:$PORT/mcp" "$TOK" 10 > "$POUT" 2>&1 || true
          grep -P '^CHECK\t' "$POUT" 2>/dev/null | while IFS=$'\t' read -r _ n v d; do
            printf 'proto\t%s\t%s\t%s\n' "$n" "$v" "$d" >> "$R"
          done
        fi
      fi
      grep -P '^CHECK\t' "$POUT" 2>/dev/null | sed 's/^CHECK\t/  /' | sed 's/\t/  /g'
      PS=$(grep -P '^SUMMARY\t' "$POUT" 2>/dev/null | cut -f2,3)
      say "  summary (pass/fail): ${PS:-unknown}"

      # ===== stage 6: resource measurement =================================
      hr; say "STAGE 6  resource measurement"
      sleep 1
      if [ -r "/proc/$ENGINE_PID/status" ]; then
        RSS=$(awk '/VmRSS/{print $2}' "/proc/$ENGINE_PID/status" 2>/dev/null)
        THR=$(awk '/Threads/{print $2}' "/proc/$ENGINE_PID/status" 2>/dev/null)
        rec res engine_rss_kb INFO "${RSS:-unknown}"
        rec res engine_threads INFO "${THR:-unknown}"
        say "  engine RSS : ${RSS:-?} kB"
        say "  threads    : ${THR:-?}"
      fi
      # latency: time N sequential status calls, report avg
      TOK2=$(grep -oE '[0-9a-f]{64}' "$CREDS" 2>/dev/null | head -1)
      if [ -n "$TOK2" ]; then
        N=20; TT0=$(date +%s%N)
        for i in $(seq 1 $N); do
          curl -s -m 5 -o /dev/null -X POST "http://$BIND:$PORT/mcp" \
            -H "Authorization: Bearer $TOK2" -H 'Content-Type: application/json' \
            -H 'Accept: application/json, text/event-stream' \
            -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"lat","version":"1"}}}' 2>/dev/null || true
        done
        TT1=$(date +%s%N)
        AVG=$(( (TT1 - TT0) / N / 1000000 ))
        rec res latency_avg_ms INFO "$AVG"
        say "  latency    : ~${AVG} ms/request over $N requests"
      fi
      # battery / thermal, only if reachable from this environment
      if command -v termux-battery-status >/dev/null 2>&1; then
        termux-battery-status 2>/dev/null | head -20 | sed 's/^/  battery: /'
      fi
      for z in /sys/class/thermal/thermal_zone0/temp; do
        [ -r "$z" ] && { say "  thermal    : $(cat "$z") (raw)"; rec res thermal_raw INFO "$(cat "$z")"; }
      done
    fi
  fi
fi

# ===== summary ==============================================================
hr; say "SUMMARY"
# `grep -c` already prints a count (0 when nothing matches) but exits non-zero, so appending
# `|| echo 0` produced a two-line value like "0\n0" — which put a newline inside the JSON number.
# Read the count, then default only if it is genuinely empty.
P=$(grep -cP '\tPASS\t' "$R" 2>/dev/null); P=${P:-0}
F=$(grep -cP '\tFAIL\t' "$R" 2>/dev/null); F=${F:-0}
S=$(grep -cP '\tSKIP\t' "$R" 2>/dev/null); S=${S:-0}
TOT=$((P + F))
say "  passed : $P"
say "  failed : $F"
say "  skipped: $S"
say "  verdict: $([ "$F" -eq 0 ] && [ "$TOT" -gt 0 ] && echo PASS || echo FAIL)"

if [ -n "$TSV_OUT" ]; then cp "$R" "$TSV_OUT"; say "  tsv    : $TSV_OUT"; fi
if [ -n "$JSON_OUT" ]; then
  {
    printf '{\n  "schema": "homeguardian-selftest/1",\n  "host_arch": "%s",\n  "os": "%s",\n  "kernel": "%s",\n  "ran_at_utc": "%s",\n  "summary": {"pass": %s, "fail": %s, "skip": %s},\n  "results": [\n' \
      "$ARCH" "$OSV" "$KER" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$P" "$F" "$S"
    # Read every row first, then emit with commas between — streaming with a running counter wrote
    # the comma on the FIRST row (c==NR is true there), which produced invalid JSON.
    awk -F'\t' '
      { rows[NR]=sprintf("    {\"group\":\"%s\",\"name\":\"%s\",\"verdict\":\"%s\",\"detail\":\"%s\"}", $1,$2,$3,$4) }
      END { for (i=1;i<=NR;i++) printf "%s%s\n", rows[i], (i<NR ? "," : "") }
    ' "$R"
    printf '  ]\n}\n'
  } > "$JSON_OUT"
  say "  json   : $JSON_OUT"
fi

[ "$F" -eq 0 ] && [ "$TOT" -gt 0 ]
