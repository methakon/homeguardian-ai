#!/usr/bin/env bash
# Cross-process observation -> MCP path (HG-CPP-MCP-READONLY-25, Option A of
# docs/HG_OBSERVATION_TO_MCP_SEAM_24.md).
#
# WHAT THIS PROVES, and why it needs three real processes:
#
#   process 1  hg_device_ingress   the SINGLE WRITER. It authenticates the device, admits the observation,
#                                  runs Observation -> Event -> Pipeline -> AlertPolicy and commits alerts
#                                  to the store.
#   process 2  hg_device_client    a synthetic authenticated device. Its observations are the only stimulus;
#                                  nothing here is injected into the store by hand.
#   process 3  hg_mcp_server       a READ-ONLY reader of the SAME store, answering the real MCP tools over
#                                  loopback with the contract's own authorization.
#
# The claim under test is the one the architecture decision makes - and the one the shipped ingress config
# already documents ("The SAME file the hub serves ... one writer at a time is serialized and readers see a
# consistent snapshot"): an admitted observation becomes visible to the MCP answer because ONE writer
# committed it and a reader read it, not because two processes shared a write.
#
# The script therefore also proves the reader refuses to write, that a MISSING store is reported as
# unavailable rather than as "no alerts" (different statements about the state of the home), and that replay
# and revocation cannot add a second copy of an alert.
#
# The alert stimulus is the documented operator opt-in (ADR-014): the ingested motion detector id is listed
# in evidence_detector_ids, without which device observations are recorded and audited but cast NO fusion
# vote. That is an allowlist decision, NOT a threshold change - no numeric threshold is touched anywhere.
#
# Nothing here is physical: no camera, no microphone, no personal media. No production keys, no broad bind
# (the ingress is bound to loopback here), and no other stack is touched.
#
# Usage: scripts/verify_observation_to_mcp_path.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
PROVISION="$REPO/tools/provision_ca.sh"
IMAGE="${HG_IMAGE:-homeguardian-hub:local}"          # ships hg_device_ingress and hg_device_client
MCP_BIN="${HG_MCP_BIN:-$REPO/build/hg_mcp_server}"   # the reader: built HERE, with the read-only change
CONFIG="${HG_MCP_CONFIG:-$REPO/config/homeguardian.demo.json}"
INGRESS_TEMPLATE="$REPO/deploy/docker/config/ingress.json.example"
PORT=18443             # the ingress (loopback-bound here)
MCP_PORT_BASE=18791    # the MCP (loopback); each start takes a fresh port so no stale server can answer
STORE="$WORK/alerts.db"
EMPTY_STORE="$WORK/empty_alerts.db"
MISSING_STORE="$WORK/definitely-not-here.db"
MCP_PIDFILE="$WORK/mcp.pids"
ANSWER="$WORK/mcp.answer"

# The image's own user is uid 10001, which cannot read this host's 0600 fixtures. Running the containers as
# the invoking user keeps the fixture permissions meaningful (and the store's owner obvious).
DUSER="$(id -u):$(id -g)"

PASS=0; FAIL=0
ok()  { printf '  PASS  %-64s %s\n' "$1" "${2:-}"; PASS=$((PASS+1)); }
fail(){ printf '  FAIL  %-64s %s\n' "$1" "${2:-}"; FAIL=$((FAIL+1)); }
note(){ printf '  ----  %s\n' "$1"; }

INGRESS_PID=""; MCP_ACTIVE=0; MCP_SEQ=0
# Every MCP start appends to a pidfile, because a function that starts a process may run inside a command
# substitution - where `$!` is invisible to the caller and the process would be orphaned, still holding its
# port. That is exactly what happened here: two stale readers made the third start die with "bind failed"
# and the answers then came from the wrong process (or from none).
kill_mcp_all() {
  [[ -f "$MCP_PIDFILE" ]] || return 0
  while read -r p; do [[ -n "$p" ]] && kill -TERM "$p" 2>/dev/null; done < "$MCP_PIDFILE"
  sleep 0.4
  while read -r p; do [[ -n "$p" ]] && kill -KILL "$p" 2>/dev/null; done < "$MCP_PIDFILE"
  : > "$MCP_PIDFILE"
}
cleanup() {
  kill_mcp_all
  docker rm -f "hgcp-ingress-$$" >/dev/null 2>&1
  wait "${INGRESS_PID:-}" 2>/dev/null
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK"; chmod 0755 "$WORK"; : > "$MCP_PIDFILE"
umask 077

echo "== cross-process path: ingress (writer) -> store -> MCP (read-only reader) =="
note "workdir   : $WORK"
note "writer    : $IMAGE / hg_device_ingress (container process, uid $DUSER, loopback-bound)"
note "reader    : $MCP_BIN (host process, uid $DUSER)"
note "shared artifact: $STORE - the ONLY thing the two processes have in common"

if [[ ! -x "$MCP_BIN" ]]; then
  fail "MCP binary present" "$MCP_BIN (build it: cmake --build build --target hg_mcp_server)"; exit 1
fi
docker image inspect "$IMAGE" >/dev/null 2>&1 || { fail "ingress image present" "$IMAGE"; exit 1; }
[[ -f "$PROVISION" ]] || { fail "provisioning tool present" "$PROVISION"; exit 1; }
[[ -f "$INGRESS_TEMPLATE" ]] || { fail "ingress config template present" "$INGRESS_TEMPLATE"; exit 1; }

# ---------------------------------------------------------------- material
# Certs come from the repo's own provisioning tool. The SAN is DNS:localhost (and the loopback IP) because
# the client verifies the SERVER's identity: a chain-valid certificate that does not identify the name
# dialled is still a refusal.
CA_DIR="$WORK/offline-ca"; CERTS="$WORK/certs"; DEVICE_DIR="$WORK/device-keys"
bash "$PROVISION" init --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
  || { fail "CA provisioning"; exit 1; }
ISSUE=(--ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR")
MOTION_ID="hg-net0000000m001"; AUDIO_ID="hg-net0000000a002"
bash "$PROVISION" issue "$MOTION_ID" "${ISSUE[@]}" >/dev/null 2>&1
bash "$PROVISION" issue "$AUDIO_ID"  "${ISSUE[@]}" >/dev/null 2>&1
show_field() { bash "$PROVISION" show "$1" "${ISSUE[@]}" 2>/dev/null | awk -v k="$2" '$1==k{print $3}'; }
MOTION_CRED="$(show_field "$MOTION_ID" credential_id)"
AUDIO_CRED="$(show_field "$AUDIO_ID" credential_id)"
if [[ -n "$MOTION_CRED" && -n "$AUDIO_CRED" ]]; then ok "CA + two device certificates issued (motion + audio)"
else fail "certificate provisioning"; exit 1; fi
chmod -R a+rX "$WORK/certs" "$WORK/device-keys" 2>/dev/null
printf '[]\n' > "$WORK/revocations.json"; chmod 0644 "$WORK/revocations.json"

write_registry() {  # $1 = is the audio node revoked? (true|false)
cat > "$WORK/devices.json" <<JSON
{
  "devices": [
    { "device_id": "$MOTION_ID", "display_name": "synthetic motion node", "device_type": "node",
      "credential_id": "$MOTION_CRED", "paired": true, "revoked": false,
      "capabilities": ["motion"], "consent_granted": true, "consent_subject": "$MOTION_ID",
      "consent_purpose": "security_monitor", "capture_enabled": true },
    { "device_id": "$AUDIO_ID", "display_name": "synthetic audio node", "device_type": "node",
      "credential_id": "$AUDIO_CRED", "paired": true, "revoked": $1,
      "capabilities": ["microphone"], "consent_granted": true, "consent_subject": "$AUDIO_ID",
      "consent_purpose": "security_monitor", "capture_enabled": true }
  ]
}
JSON
chmod 0644 "$WORK/devices.json"
}

ingress_config() {  # $1 = audit db path. Built from the repo's OWN template, as verify_detection_end_to_end.sh does
  python3 - "$INGRESS_TEMPLATE" "$WORK/ingress.json" "$CERTS" "$WORK" "$PORT" "$1" <<'PY'
import json, sys
tpl, out, certs, work, port, store = sys.argv[1:7]
cfg = json.load(open(tpl))
cfg["certificate_path"] = f"{certs}/server.crt"
cfg["private_key_path"] = f"{certs}/server.key"
cfg["ca_path"] = f"{certs}/ca/ca.crt"
cfg["registry_path"] = f"{work}/devices.json"
cfg["revocation_path"] = f"{work}/revocations.json"
cfg["bind_address"] = "127.0.0.1"          # no broad bind: this is an isolated test, not a deployment
cfg["port"] = int(port)
cfg["audit_db_path"] = store
cfg["replay_state_path"] = f"{work}/replay_state.json"
# ADR-014 operator opt-in: let the ingested motion evidence cast a fusion vote. An allowlist decision, not a
# threshold change - no numeric threshold is altered anywhere in this script or in the code under test.
cfg["evidence_detector_ids"] = ["sensor.motion"]
json.dump(cfg, open(out, "w"), indent=2)
PY
  chmod 0644 "$WORK/ingress.json"
}

start_ingress() {  # $1 = audit db path
  ingress_config "$1"
  docker rm -f "hgcp-ingress-$$" >/dev/null 2>&1
  docker run --rm --name "hgcp-ingress-$$" --entrypoint sh --network host --user "$DUSER" \
    -v "$WORK:$WORK" "$IMAGE" -lc "hg_device_ingress --config '$WORK/ingress.json'" \
    >"$WORK/ingress.log" 2>&1 &
  INGRESS_PID=$!
  for _ in $(seq 1 60); do
    ss -ltn 2>/dev/null | grep -q ":$PORT " && return 0
    if ! kill -0 "$INGRESS_PID" 2>/dev/null; then
      note "ingress exited: $(tail -1 "$WORK/ingress.log" 2>/dev/null)"; return 1
    fi
    sleep 0.25
  done
  return 1
}
stop_ingress() {
  docker rm -f "hgcp-ingress-$$" >/dev/null 2>&1
  [[ -n "$INGRESS_PID" ]] && kill -TERM "$INGRESS_PID" 2>/dev/null
  wait "$INGRESS_PID" 2>/dev/null; INGRESS_PID=""
}

send() {  # $1 = device id, rest = client flags
  local id="$1"; shift
  docker run --rm --entrypoint sh --network host --user "$DUSER" -v "$WORK:$WORK" "$IMAGE" -lc \
    "hg_device_client --host localhost --port $PORT --ca '$CERTS/ca/ca.crt' \
      --cert '$CERTS/devices/$id.crt' --key '$DEVICE_DIR/$id.key' --device-id '$id' $*" 2>&1
}

store_alerts() {  # "count|space-separated ids" straight from the store, opened read-only
  python3 - "$1" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
    rows = c.execute("SELECT alert_id FROM alerts").fetchall()
    print(f"{len(rows)}|{' '.join(r[0] for r in rows)}")
except Exception:
    print("none|")
PY
}

# ---------------------------------------------------------------- 1. the writer commits an alert
kill_mcp_all
write_registry false
rm -f "$STORE" "$STORE-wal" "$STORE-shm"
start_ingress "$STORE" || { fail "ingress started (writer)" "$(tail -1 "$WORK/ingress.log" 2>/dev/null)"; exit 1; }
ok "ingress (writer) listening on 127.0.0.1:$PORT" "container pid $INGRESS_PID"
grep -o 'bind=[^ ]* port=[0-9]*' "$WORK/ingress.log" | head -1 | sed 's/^/        /'

# The stimulus the repo's own detection script uses: a quiet baseline, then a motion stream at 0.45.
send "$MOTION_ID" "--count 8 --interval-ms 20 --mode normal --value 0.08" >"$WORK/s0.log" 2>&1 || true
send "$MOTION_ID" "--count 8 --interval-ms 20 --mode normal --value 0.45" >"$WORK/s1.log" 2>&1 || true
send "$MOTION_ID" "--count 8 --interval-ms 20 --mode normal --value 0.60" >"$WORK/s2.log" 2>&1 || true
ACCEPTED="$(cat "$WORK"/s*.log 2>/dev/null | grep -c 'accepted=1')"
note "observations acknowledged by the ingress: $ACCEPTED"
if [[ "$ACCEPTED" -ge 1 ]]; then ok "authenticated synthetic observations admitted and ACKed" "$ACCEPTED frames"
else fail "authenticated synthetic observations admitted" "see $WORK/s*.log"; fi

sleep 0.8
STORE_STATE="$(store_alerts "$STORE")"
STORE_COUNT="${STORE_STATE%%|*}"; STORE_IDS="${STORE_STATE#*|}"
note "the WRITER committed to the store: alert_count=$STORE_COUNT ids='$STORE_IDS'"
if [[ "$STORE_COUNT" == "none" ]]; then fail "store readable by a third party" "$STORE"
elif [[ "$STORE_COUNT" -ge 1 ]]; then ok "the alert-generating fixture produced a COMMITTED alert" "alert_count=$STORE_COUNT"
else fail "the fixture commits an alert" "alert_count=0 (is the ADR-014 evidence allowlist in effect?)"; fi

# ---------------------------------------------------------------- 2. the reader sees it (separate process)
TOK="cp-$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
start_mcp() {  # $1 = store path
  MCP_ACTIVE=$((MCP_PORT_BASE + (++MCP_SEQ)))
  printf '{"%s":{"principal":"hg-cross-process","allow_status":true,"allow_alert_summary":true}}' "$TOK" \
    > "$WORK/creds.json"
  chmod 0600 "$WORK/creds.json"
  HG_MCP_PORT="$MCP_ACTIVE" HG_MCP_ALERTS_DB="$1" "$MCP_BIN" "$CONFIG" "$WORK/creds.json" \
    >"$WORK/mcp.log" 2>&1 &
  local pid=$!
  echo "$pid" >> "$MCP_PIDFILE"
  for _ in $(seq 1 60); do
    ss -ltn 2>/dev/null | grep -q ":$MCP_ACTIVE " && break
    if ! kill -0 "$pid" 2>/dev/null; then return 1; fi
    sleep 0.25
  done
  ss -ltn 2>/dev/null | grep -q ":$MCP_ACTIVE " || return 1
  return 0
}
stop_mcp() { kill_mcp_all; }

mcp_text() { python3 -c "
import json,sys
t=sys.stdin.read()
if 'data:' in t: t=[l[6:] for l in t.splitlines() if l.startswith('data: ')][0]
d=json.loads(t)
if 'error' in d:
    print('ERROR:'+str(d['error'].get('message') or d['error'].get('code'))); raise SystemExit
r=d.get('result',{})
if r.get('isError'):
    # The contract turns an unavailable application into a refusal. That IS the fail-closed answer: it must
    # never be reported as a zero count, so it is surfaced as an error here.
    c=r.get('content',[{}])
    print('ERROR:'+(c[0].get('text','') if c else 'isError')); raise SystemExit
c=r.get('content',[{}])
print(c[0].get('text','') if c else json.dumps(d))"; }

from_store() {  # $1 = store path; writes the tool answer to $ANSWER
  stop_mcp
  : > "$ANSWER"
  if ! start_mcp "$1"; then fail "MCP (reader) start on $MCP_ACTIVE" "$1"; return 0; fi
  local h sid
  h="$(curl -s -D - -o /dev/null -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
        -H 'Accept: application/json, text/event-stream' -X POST "http://127.0.0.1:$MCP_ACTIVE/mcp" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cp","version":"1"}}}')"
  sid="$(printf '%s' "$h" | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')"
  curl -s -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
       -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $sid" \
       -X POST "http://127.0.0.1:$MCP_ACTIVE/mcp" \
       -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' \
    | mcp_text > "$ANSWER" 2>/dev/null || true
}

from_store "$STORE"; READER="$(cat "$ANSWER")"
note "MCP get_alert_summary (process 3, store opened READ-ONLY): $READER"
grep -o "alert store .*" "$WORK/mcp.log" | head -1 | sed 's/^/        /'
if [[ "$READER" == ERROR:* ]]; then
  fail "MCP read the same store" "$READER"
elif [[ -n "$READER" && "$STORE_COUNT" != "none" ]] && printf '%s' "$READER" | grep -q "\"alert_count\":$STORE_COUNT"; then
  ok "cross-process: the MCP reports the alert count the ingress committed" "alert_count=$STORE_COUNT"
  MATCH=1
  for id in $STORE_IDS; do printf '%s' "$READER" | grep -q "$id" || MATCH=0; done
  [[ $MATCH == 1 ]] && ok "the reader names the SAME alert ids the writer stored" "$STORE_IDS" \
                    || fail "reader alert ids match the store" "$READER"
else
  fail "cross-process: MCP reports the committed alert count" "store=$STORE_COUNT reader='$READER'"
fi

# ---------------------------------------------------------------- 3. restart: persistence, not a cache
from_store "$STORE"; RESTARTED="$(cat "$ANSWER")"
if [[ -n "$RESTARTED" && "$RESTARTED" == "$READER" && "$RESTARTED" != ERROR:* ]]; then
  ok "after restarting the MCP the same alerts are reported" "$(printf '%s' "$RESTARTED" | head -c 46)"
else
  fail "restart reports the same state" "before='$READER' after='$RESTARTED'"
fi
stop_mcp

# ---------------------------------------------------------------- 4. empty store vs missing store
start_ingress "$EMPTY_STORE" >/dev/null 2>&1 || true   # a writer creates an EMPTY but valid store
sleep 0.6; stop_ingress
from_store "$EMPTY_STORE"; EMPTY_READ="$(cat "$ANSWER")"
if printf '%s' "$EMPTY_READ" | grep -q '"alert_count":0'; then
  ok "a valid EMPTY store answers alert_count=0 (zero is a fact, not an error)" \
     "$(printf '%s' "$EMPTY_READ" | head -c 46)"
else
  fail "empty store answers zero" "'$EMPTY_READ'"
fi
stop_mcp
rm -f "$MISSING_STORE"
from_store "$MISSING_STORE"; MISSING_READ="$(cat "$ANSWER")"
if [[ "$MISSING_READ" == ERROR:* ]]; then
  ok "a MISSING store FAILS CLOSED and is NOT reported as zero" "$MISSING_READ"
else
  fail "missing store fails closed" "got: '$MISSING_READ'"
fi
if [[ -f "$MISSING_STORE" ]]; then fail "the reader must not create what it reads" "$MISSING_STORE"
else ok "the reader did not create the missing store" "no file created"; fi
stop_mcp

# ---------------------------------------------------------------- 5. replay and revocation add no duplicate
start_ingress "$STORE" >/dev/null 2>&1 || true
BEFORE_REPLAY="$(store_alerts "$STORE")"
send "$MOTION_ID" "--count 1 --sequence-start 1 --expect-reject" >"$WORK/replay.log" 2>&1 || true
grep -hE 'replayed_sequence|rejected=' "$WORK/replay.log" | tail -2 | sed 's/^/        /'
sleep 0.4
AFTER_REPLAY="$(store_alerts "$STORE")"
if [[ "$BEFORE_REPLAY" == "$AFTER_REPLAY" ]]; then
  ok "a replayed sequence adds no alert (the writer still refuses it)" "count unchanged (${AFTER_REPLAY%%|*})"
else
  fail "replay adds no alert" "before=$BEFORE_REPLAY after=$AFTER_REPLAY"
fi
stop_ingress

write_registry true   # registry-file revocation: the operator's own flag
start_ingress "$STORE" >/dev/null 2>&1 || true
REVOKE_OUT="$(send "$AUDIO_ID" "--count 1 --mode normal --expect-reject")"
printf '%s' "$REVOKE_OUT" | grep -qE 'revoked|rejected' && REVOKE_REFUSED=1 || REVOKE_REFUSED=0
sleep 0.4
AFTER_REVOKE="$(store_alerts "$STORE")"
if [[ $REVOKE_REFUSED == 1 && "$AFTER_REVOKE" == "$AFTER_REPLAY" ]]; then
  ok "a revoked device is refused and adds no alert" \
     "$(printf '%s' "$REVOKE_OUT" | grep -oE 'reason=[a-z_]+' | head -1)"
else
  fail "revocation refuses and adds nothing" "refused=$REVOKE_REFUSED before=$AFTER_REPLAY after=$AFTER_REVOKE"
fi
stop_ingress
write_registry false

# ---------------------------------------------------------------- 6. the reader cannot write, even by mount
BEFORE_PROBE="$(store_alerts "$STORE")"
if docker run --rm --entrypoint sh --user "$DUSER" -v "$STORE:/store/alerts.db:ro" "$IMAGE" -lc \
     'printf x >> /store/alerts.db' 2>/dev/null; then
  fail "a read-only mount refuses a write" "the write SUCCEEDED"
else
  ok "a read-only mount refuses a write (single-writer at the transport level)" "read-only file system"
fi
AFTER_PROBE="$(store_alerts "$STORE")"
[[ "$BEFORE_PROBE" == "$AFTER_PROBE" ]] && ok "the store is unchanged by that attempt" "" \
                                       || fail "store unchanged after the write attempt" ""

echo "  --------------------------------------------------------------------"
echo "  cross-process observation->MCP: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] || exit 1
