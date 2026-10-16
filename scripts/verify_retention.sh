#!/usr/bin/env bash
# verify_retention.sh - regression for the bridge's own durable alert store.
#
# Defect this guards (fixed in v21): the native HTTP bridge writes persisted
# alerts to its own audit DB, but nothing ever applied the configured
# alert_retention_days window to that DB, so it grew without bound across runs
# and restarts. The bridge now prunes once at startup and, at most once per
# interval, after a scenario write - exactly like the application.
#
# What it proves:
#   1. an alert older than the configured window is pruned when the bridge starts
#   2. alerts inside the window survive the restart (recovery)
#   3. the delivered/persisted counts are unchanged by retention
#
# Usage: HG_BRIDGE_PATH=/path/to/hg_alexa_bridge bash scripts/verify_retention.sh
set -euo pipefail

BRIDGE="$(realpath "${HG_BRIDGE_PATH:?set HG_BRIDGE_PATH to a built hg_alexa_bridge}")"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${HG_RETENTION_PORT:-8531}"
WORK="$(mktemp -d /tmp/hg_retention.XXXXXX)"
PASS=0; FAIL=0
note() { printf '  %-58s %s\n' "$1" "$2"; }
ok()   { PASS=$((PASS+1)); note "$1" "OK"; }
bad()  { FAIL=$((FAIL+1)); note "$1" "FAIL"; }

cleanup() {
  [[ -n "${PID:-}" ]] && kill -TERM "$PID" 2>/dev/null || true
  sleep 1
  rm -rf "$WORK"
}
trap cleanup EXIT

cat > "$WORK/creds.json" <<'JSON'
{"demo-token":{"principal":"demo","allow_status":true,"allow_alert_summary":true,"expires_at_ms":0,"revoked":false}}
JSON
chmod 600 "$WORK/creds.json"
# 30-day window, matching config/homeguardian.demo.json
cat > "$WORK/config.json" <<'JSON'
{
  "log_level": "warn", "server_host": "127.0.0.1", "server_port": 8080,
  "data_dir": "/tmp", "max_event_history": 1000, "correlation_window_ms": 5000,
  "alert_confidence_threshold": 0.5, "media_capture_enabled": true,
  "cloud_processing_enabled": false, "consent_policy_version": "1.0.0",
  "routine_retention_days": 730, "profile_retention_days": 1825,
  "alert_retention_days": 30
}
JSON

RET_SECRET="retention-secret-2026"
start_bridge() {
  printf '%s' "$RET_SECRET" > "$WORK/ret_secret.txt"
  chmod 600 "$WORK/ret_secret.txt"
  printf '%s' "demo-token" > "$WORK/ret_token.txt"
  chmod 600 "$WORK/ret_token.txt"
  ( cd "$WORK" && HG_HTTP_SECRET="$RET_SECRET" HG_HTTP_TOKEN="demo-token" exec "$BRIDGE" --http "$WORK/config.json" "$WORK/creds.json" \
      --port "$PORT" --token-file "$WORK/ret_token.txt" --secret-file "$WORK/ret_secret.txt" ) > "$WORK/out.log" 2> "$WORK/err.log" &
  PID=$!
  for _ in $(seq 1 40); do
    curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/health" && return 0
    sleep 0.25
  done
  echo "bridge did not become ready"; cat "$WORK/out.log" "$WORK/err.log"; exit 1
}
stop_bridge() { kill -TERM "$PID" 2>/dev/null || true; sleep 2; PID=""; }
DB="$WORK/homeguardian_bridge_audit.db"
# sqlite3 may be installed outside PATH on this host
SQ="${HG_SQLITE3:-$(command -v sqlite3 || echo "$HOME/.local/bin/sqlite3")}"
count() { "$SQ" "$DB" "SELECT count(*) FROM alerts;"; }

run_scenario() {
  local sc="$1"
  local csrf
  csrf=$(curl -s -f -X POST "http://127.0.0.1:$PORT/api/session" \
    -H "Host: 127.0.0.1:$PORT" -H "Origin: http://127.0.0.1:$PORT" \
    -H "Content-Type: application/json" -H "X-HG-Secret: $RET_SECRET" -d "{}" \
    | grep -o '"csrf":"[^"]*"' | cut -d'"' -f4)
  curl -s -f -o /dev/null -X POST "http://127.0.0.1:$PORT/api/run-scenario?scenario=$sc" \
    -H "Host: 127.0.0.1:$PORT" -H "Origin: http://127.0.0.1:$PORT" \
    -H "Content-Type: application/json" -H "X-HG-CSRF: $csrf" -d "{}"
}

echo "== bridge alert-store retention regression =="
start_bridge
run_scenario "synthetic-critical-impact"
[[ "$(count)" == "2" ]] && ok "2 alerts persisted by the critical scenario" \
                        || bad "expected 2 persisted alerts, got $(count)"
stop_bridge

# seed one alert far outside the 30-day window and one just inside it
OLD_MS=$(( ($(date +%s) - 40*86400) * 1000 ))
RECENT_MS=$(( ($(date +%s) - 2*86400) * 1000 ))
"$SQ" "$DB" "INSERT INTO alerts VALUES('seed-old','info','seeded old','[]',$OLD_MS,0.5,'seeded','{}');"
"$SQ" "$DB" "INSERT INTO alerts VALUES('seed-recent','info','seeded recent','[]',$RECENT_MS,0.5,'seeded','{}');"
[[ "$(count)" == "4" ]] && ok "seeded 2 rows (4 total before restart)" \
                        || bad "expected 4 rows before restart, got $(count)"

start_bridge                      # startup retention runs here
grep -q "retention: pruned 1 alert(s) older than 30 day(s)" "$WORK/err.log" \
  && ok "startup retention reported pruning 1 out-of-window alert" \
  || bad "startup retention did not report a prune: $(grep -c retention "$WORK/err.log")"
[[ "$("$SQ" "$DB" "SELECT count(*) FROM alerts WHERE alert_id='seed-old';")" == "0" ]] \
  && ok "out-of-window alert pruned" || bad "out-of-window alert still present"
[[ "$("$SQ" "$DB" "SELECT count(*) FROM alerts WHERE alert_id='seed-recent';")" == "1" ]] \
  && ok "in-window alert survived the restart" || bad "in-window alert was pruned"
[[ "$(count)" == "3" ]] && ok "3 rows remain after retention" \
                        || bad "expected 3 rows after retention, got $(count)"

curl -sf "http://127.0.0.1:$PORT/api/alerts" | grep -q '"total": *3' \
  && ok "API reports the post-retention count" \
  || bad "API total does not reflect retention"
run_scenario "normal" \
  && ok "scenario still runs after retention" || bad "scenario failed after retention"

echo "  ----------------------------------------------"
echo "  retention regression: $PASS passed, $FAIL failed"
[[ "$FAIL" == "0" ]]
