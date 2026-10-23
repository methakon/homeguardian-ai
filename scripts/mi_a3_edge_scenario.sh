#!/usr/bin/env bash
# mi_a3_edge_scenario.sh — run a REAL synthetic scenario through the HomeGuardian
# intelligence engine ON the Mi A3, then read the resulting alert back through the
# MCP server's read-only tool.
#
# This is the joined end-to-end proof: the engine writes an alert into SQLite on
# the device, and the MCP tool reads that SAME database. Loopback only.
#
# Usage: scripts/mi_a3_edge_scenario.sh [serial] [scenario_id]
set -uo pipefail

SERIAL="${1:-edd53dbd04de}"
SCENARIO="${2:-synthetic-critical-impact}"
DEV_DIR=/data/local/tmp/hg_edge
DATA_DIR="$DEV_DIR/data"
BPORT=8791
MPORT=8790
LBPORT=18791
LMPORT=18790
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

adbs() { timeout 90 adb -s "$SERIAL" "$@"; }
ORIGIN="http://127.0.0.1:$BPORT"

echo "== stopping MCP server (the bridge will be the sole writer to the audit DB) =="
adbs shell 'for p in $(pidof hg_mcp_server); do kill "$p"; done' >/dev/null 2>&1
adbs shell 'for p in $(pidof hg_alexa_bridge); do kill "$p"; done' >/dev/null 2>&1
sleep 2

echo "== staging engine binary =="
adbs shell "mkdir -p $DATA_DIR"
adbs push "$ROOT/deploy/mi_a3_edge/hg_alexa_bridge"   "$DEV_DIR/hg_alexa_bridge"   >/dev/null
# The SCENARIO run uses the simulator-enabled config: the synthetic scenario needs
# observations to be delivered, and with media_capture_enabled=false the fail-closed
# consent/capture gate correctly blocks every one (verified: 0 events, 0 alerts).
# Nothing physical is opened either way - this build links the SIMULATOR backend only.
adbs push "$ROOT/deploy/mi_a3_edge/hg_engine_config.json" "$DEV_DIR/hg_engine_config.json" >/dev/null
adbs shell "chmod 755 $DEV_DIR/hg_alexa_bridge"

# Engine credential + launch secret: generated on the device, never on the host.
adbs shell "cd $DEV_DIR && umask 077 && TOK=\$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n') && \
  SEC=\$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n') && \
  printf '{\"%s\":{\"principal\":\"mi-a3-edge\",\"allow_status\":true,\"allow_alert_summary\":true,\"expires_at_ms\":0,\"revoked\":false}}\n' \"\$TOK\" > engine_creds.json && \
  chmod 600 engine_creds.json && printf '%s' \"\$SEC\" > .launch_secret && chmod 600 .launch_secret && echo provisioned"

TOK=$(adbs shell "cat $DEV_DIR/engine_creds.json" | python3 -c 'import sys,json;print(list(json.load(sys.stdin))[0])')
SEC=$(adbs shell "cat $DEV_DIR/.launch_secret")
SEC=$(printf '%s' "$SEC" | tr -d '\r\n')

echo "== starting the engine (loopback $BPORT) =="
adbs shell "cd $DATA_DIR && rm -f homeguardian_bridge_audit.db* && nohup setsid env LD_LIBRARY_PATH=$DEV_DIR $DEV_DIR/hg_alexa_bridge --http $DEV_DIR/hg_engine_config.json $DEV_DIR/engine_creds.json --port $BPORT --token '$TOK' --secret '$SEC' > $DEV_DIR/logs/engine.out 2>&1 < /dev/null & echo started" >/dev/null

adbs forward --remove tcp:$LBPORT >/dev/null 2>&1
adbs forward tcp:$LBPORT tcp:$BPORT >/dev/null 2>&1
B="http://127.0.0.1:$LBPORT"
for _ in $(seq 1 60); do
  curl -sf -o /dev/null "$B/api/health" && break
  sleep 0.25
done
echo "  health : $(curl -sS "$B/api/health")"
echo "  engine log:"; adbs shell "cat $DEV_DIR/logs/engine.out" | tail -3 | sed 's/^/    /'

echo
echo "== available synthetic scenarios =="
curl -sS "$B/api/scenarios" | python3 -c '
import sys,json
d=json.load(sys.stdin)
ids=[s.get("id") for s in d.get("scenarios",[])]
print("  ", ids)' 2>/dev/null || echo "  (could not parse)"

echo
echo "== status/alerts BEFORE the scenario =="
curl -sS "$B/api/status"  | head -c 300; echo
curl -sS "$B/api/alerts"  | head -c 300; echo

echo
echo "== POST /api/session (proves x-hg-secret, same-origin) =="
CSRF=$(curl -sS -X POST "$B/api/session" -H "Content-Type: application/json" \
       -H "Origin: $ORIGIN" -H "x-hg-secret: $SEC" --data '{}' \
       | python3 -c 'import sys,json;print(json.load(sys.stdin).get("csrf",""))')
[ -n "$CSRF" ] && echo "  csrf issued: yes (len ${#CSRF})" || { echo "  csrf FAILED"; exit 1; }

echo "== a second session POST with a WRONG secret must be 401 =="
echo "  -> $(curl -sS -o /dev/null -w '%{http_code}' -X POST "$B/api/session" -H 'Content-Type: application/json' -H "Origin: $ORIGIN" -H 'x-hg-secret: wrong' --data '{}')"
echo "== run-scenario with a WRONG Origin must be 403 =="
echo "  -> $(curl -sS -o /dev/null -w '%{http_code}' -X POST "$B/api/run-scenario?scenario=$SCENARIO" -H 'Content-Type: application/json' -H 'Origin: http://evil.example' -H "x-hg-csrf: $CSRF" --data '{}')"

echo
echo "== POST /api/run-scenario?scenario=$SCENARIO (real engine work on-device) =="
T0=$(date +%s%N)
curl -sS -X POST "$B/api/run-scenario?scenario=$SCENARIO" -H "Content-Type: application/json" \
     -H "Origin: $ORIGIN" -H "x-hg-csrf: $CSRF" --data '{}' \
     | python3 -c 'import sys,json;d=json.load(sys.stdin);print(json.dumps(d,indent=2)[:1800])' 2>/dev/null
T1=$(date +%s%N)
echo "  scenario wall time: $(( (T1-T0)/1000000 )) ms"

echo
echo "== status/alerts AFTER =="
curl -sS "$B/api/alerts" | python3 -c '
import sys,json
d=json.load(sys.stdin)
a=d.get("alerts",[])
print(f"  alerts returned: {len(a)}")
for x in a[:5]:
    print("   ", {k:x.get(k) for k in ("alert_id","severity","rule","confidence") if k in x})' 2>/dev/null || curl -sS "$B/api/alerts" | head -c 500

echo
echo "== SQLite written by the ENGINE on the device =="
adbs shell "ls -l $DATA_DIR/"
# The engine's store is WAL-journalled. SIGTERM on the bridge does not necessarily
# checkpoint, so pull the -wal/-shm alongside the main file and let the host's sqlite3
# replay the WAL. Pulling the main file alone reads an empty database (observed:
# 4096 bytes, 0 rows) while the real rows sat in a 78 KB -wal.
mkdir -p "$ROOT/logs/device-artifacts/engine"
E_DB=homeguardian_bridge_audit.db
for f in "$E_DB" "$E_DB-wal" "$E_DB-shm"; do
  adbs pull "$DATA_DIR/$f" "$ROOT/logs/device-artifacts/engine/$f" >/dev/null 2>&1
done
ls -l "$ROOT/logs/device-artifacts/engine/"
python3 - "$ROOT/logs/device-artifacts/engine/$E_DB" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print("  journal_mode:", c.execute("PRAGMA journal_mode").fetchone()[0])
for t in [r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'")]:
    n = c.execute(f"SELECT COUNT(*) FROM '{t}'").fetchone()[0]
    print(f"  {t}: {n} rows")
    if t == "alerts" and n:
        cols = [d[1] for d in c.execute("PRAGMA table_info(alerts)")]
        for row in c.execute("SELECT * FROM alerts"):
            d = dict(zip(cols, row))
            print("    ", {k: d.get(k) for k in ("alert_id","severity","rule") if k in d})
print("  integrity_check:", c.execute("PRAGMA integrity_check").fetchone()[0])
PY

echo
echo "== JOINED PROOF: the MCP read-only tool now sees the engine's alert =="
adbs shell 'for p in $(pidof hg_alexa_bridge); do kill "$p"; done' >/dev/null 2>&1
sleep 2
adbs shell "cd $DEV_DIR && nohup setsid env LD_LIBRARY_PATH=$DEV_DIR HG_MCP_AUDIT_DB=$DATA_DIR/homeguardian_bridge_audit.db ./hg_mcp_server hg_config.json mcp_credentials.json > logs/hg_mcp.out 2>&1 < /dev/null & echo started" >/dev/null
for _ in $(seq 1 60); do
  adbs shell 'cat /proc/net/tcp' 2>/dev/null | grep -qi ':2256' && break
  sleep 0.25
done
adbs forward --remove tcp:$LMPORT >/dev/null 2>&1
adbs forward tcp:$LMPORT tcp:$MPORT >/dev/null 2>&1
M="http://127.0.0.1:$LMPORT/mcp"
MTOK=$(adbs shell "cat $DEV_DIR/mcp_credentials.json" | python3 -c 'import sys,json;print(list(json.load(sys.stdin))[0])')
SID=$(curl -sS -D - -o /dev/null -X POST "$M" -H "Content-Type: application/json" \
      -H 'Accept: application/json, text/event-stream' -H "Authorization: Bearer $MTOK" \
      --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"join","version":"1"}}}' \
      | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
curl -sS -o /dev/null -X POST "$M" -H "Content-Type: application/json" -H 'Accept: application/json, text/event-stream' \
     -H "Authorization: Bearer $MTOK" -H "Mcp-Session-Id: $SID" --data '{"jsonrpc":"2.0","method":"notifications/initialized"}'
for tool in get_homeguardian_status get_alert_summary; do
  echo "  tools/call $tool:"
  curl -sS -X POST "$M" -H "Content-Type: application/json" -H 'Accept: application/json, text/event-stream' \
       -H "Authorization: Bearer $MTOK" -H "Mcp-Session-Id: $SID" \
       --data "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"$tool\",\"arguments\":{}}}" \
    | python3 -c 'import sys,json;d=json.load(sys.stdin);print("   ",d["result"]["content"][0]["text"])' 2>/dev/null
done
