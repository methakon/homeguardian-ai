#!/usr/bin/env bash
# HG-CPP-FUSION-WINDOW-33: a TRUE in-window detected verdict -> alert -> equal read-only MCP answer.
#
# THE BLOCKER THAT WAS FOUND (and it was NOT a threshold)
#   src/core/SecurityObservation.cpp:75-97  security_status_for(source_type, state)
#     motion:  "on"/"detected"/"active" -> detected | "off"/"clear"/"idle" -> not_detected | ELSE inconclusive
#     presence: "on"/"present"/"occupied" -> detected | "off"/"absent"/"clear" -> not_detected | ELSE inconclusive
#     availability: "offline"/"unavailable"/"error"/"unknown" -> detected (the PROBLEM state) |
#                   "online"/"available"/"ok" -> not_detected | ELSE inconclusive
#   src/core/FusionEngine.cpp:24-27  "inconclusive is not a vote"
# So a frame carrying state "present" on a MOTION source is INCONCLUSIVE, casts no vote, and cannot raise an
# alert no matter how many are sent - which is exactly why every earlier positive attempt showed 0 alerts while
# every gate was already satisfied. The fix is the stimulus's own state field, not any threshold:
#   motion  + state "on"      -> detected vote
#   presence + state "present" -> detected vote
# Nothing here changes confidence, agreement, min_usable, or the episode opt-in, and nothing claims real
# provenance: every frame is acquisition_mode/provenance_claim "synthetic".
#
# The negative case, stated from the same mapping: availability is only DETECTED when it reports a problem.
# A healthy availability observation is either inconclusive (no vote) or not_detected (a NEGATIVE vote), so
# availability-only input cannot produce a positive verdict. That is the correct answer, not a defect.
#
# Isolation: own loopback-only ports, own stores, own containers, own test CA. 18443/18444/18445 belong to
# another lane and are not touched. One heavy build was already spent; this script only builds nothing.
#
# Usage: scripts/verify_fusion_window.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
PROVISION="$REPO/tools/provision_ca.sh"
TEMPLATE="$REPO/deploy/docker/config/ingress.json.example"
IMAGE="${HG_VECTOR_IMAGE:-homeguardian-vector27:local}"
POS_PORT=18461; NEG_PORT=18462; MCP_PORT=18811
MOTION_ID="hg-android-synthetic-01"; PRESENCE_ID="hg-android-synthetic-02"
DUSER="$(id -u):$(id -g)"

PASS=0; FAIL=0
ok()  { printf '  PASS  %-60s %s\n' "$1" "${2:-}"; PASS=$((PASS+1)); }
bad() { printf '  FAIL  %-60s %s\n' "$1" "${2:-}"; FAIL=$((FAIL+1)); }
note(){ printf '  ----  %s\n' "$1"; }
cleanup() {
  docker rm -f hgfw-pos hgfw-neg hgfw-mcp >/dev/null 2>&1
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK"; chmod 0755 "$WORK"; umask 077

for p in "$POS_PORT" "$NEG_PORT" "$MCP_PORT"; do
  ss -ltn 2>/dev/null | grep -q ":$p " && { bad "port $p is free" "occupied - choose another"; exit 1; }
done
ok "loopback ports verified free (another lane owns 18443/18444/18445)" "$POS_PORT / $NEG_PORT / $MCP_PORT"
docker image inspect "$IMAGE" >/dev/null 2>&1 || { bad "image present" "$IMAGE"; exit 1; }

# ---------------------------------------------------------------- identities + registry
CA="$WORK/offline"; CERTS="$WORK/certs"; KEYS="$WORK/keys"
bash "$PROVISION" init --ca-dir "$CA" --runtime-dir "$CERTS" --device-dir "$KEYS" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 || { bad "CA provisioning"; exit 1; }
for id in "$MOTION_ID" "$PRESENCE_ID"; do
  bash "$PROVISION" issue "$id" --ca-dir "$CA" --runtime-dir "$CERTS" --device-dir "$KEYS" >/dev/null 2>&1 \
    || { bad "certificate for $id"; exit 1; }
done
chmod -R a+rX "$CERTS" "$KEYS"
cred() { bash "$PROVISION" show "$1" --ca-dir "$CA" --runtime-dir "$CERTS" --device-dir "$KEYS" 2>/dev/null | awk '$1=="credential_id"{print $3}'; }
python3 - "$WORK" "$MOTION_ID" "$(cred "$MOTION_ID")" "$PRESENCE_ID" "$(cred "$PRESENCE_ID")" <<'PY'
import json, sys
w, m, mc, p, pc = sys.argv[1:6]
json.dump({"devices": [
 {"device_id": m, "display_name": "synthetic motion node", "device_type": "node", "credential_id": mc,
  "paired": True, "revoked": False, "capabilities": ["motion"], "consent_granted": True,
  "consent_subject": m, "consent_purpose": "security_monitor", "capture_enabled": True},
 {"device_id": p, "display_name": "synthetic presence node", "device_type": "node", "credential_id": pc,
  "paired": True, "revoked": False, "capabilities": ["presence"], "consent_granted": True,
  "consent_subject": p, "consent_purpose": "security_monitor", "capture_enabled": True}]},
 open(f"{w}/registry.json", "w"))
PY
printf '[]' > "$WORK/revocation.json"; chmod 0644 "$WORK/registry.json" "$WORK/revocation.json"

config() {  # $1 = port, $2 = store, $3 = evidence ids JSON
  python3 - "$TEMPLATE" "$WORK/ingress-$1.json" "$CERTS" "$WORK" "$1" "$2" "$3" <<'PY'
import json, sys
tpl, out, certs, work, port, store, evidence = sys.argv[1:8]
cfg = json.load(open(tpl))
cfg.update({"certificate_path": f"{certs}/server.crt", "private_key_path": f"{certs}/server.key",
            "ca_path": f"{certs}/ca/ca.crt", "registry_path": f"{work}/registry.json",
            "revocation_path": f"{work}/revocation.json", "bind_address": "127.0.0.1", "port": int(port),
            "audit_db_path": store, "replay_state_path": f"{work}/replay-{port}.json", "verbose": True,
            "evidence_detector_ids": json.loads(evidence)})
json.dump(cfg, open(out, "w"), indent=2)
PY
  chmod 0644 "$WORK/ingress-$1.json"
}
start() {  # $1 = name, $2 = port, $3 = store, $4 = evidence
  config "$2" "$3" "$4"; docker rm -f "$1" >/dev/null 2>&1
  docker run -d --name "$1" --entrypoint sh --network host --user "$DUSER" -v "$WORK:$WORK" "$IMAGE" \
    -lc "hg_device_ingress --config '$WORK/ingress-$2.json'" >/dev/null 2>&1
  for _ in $(seq 1 60); do ss -ltn 2>/dev/null | grep -q ":$2 " && return 0; sleep 0.25; done
  return 1
}
stream() {  # $1 = device, $2 = source_type, $3 = observation_type, $4 = feature key, $5 = state
  python3 - "$WORK" "$1" "$2" "$3" "$4" "$5" <<'PY'
import struct, time, pathlib, sys
w, dev, src, obs, key, state = pathlib.Path(sys.argv[1]), *sys.argv[2:7]
NOW = int(time.time() * 1000)          # LIVE mode (e880/883397's live path): the pinned 2025 golden is refused
hello = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"hello",'
         f'"device_id":"{dev}","display_name":"synthetic node","capabilities":["camera","microphone"]}}')
def fr(b): return struct.pack(">I", len(b.encode())) + b.encode()
out = [fr(hello)]
for seq in range(1, 13):
    body = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"observation",'
            f'"observation_id":"{dev}-obs-{seq:04d}","device_id":"{dev}","source_type":"{src}",'
            f'"observation_type":"{obs}","acquisition_mode":"synthetic","acquired_at_ms":{NOW},'
            f'"sequence":{seq},"features":{{"{key}":0.900,"state":"{state}"}},"unit":"fraction",'
            '"confidence":0.9,"provenance_claim":"synthetic"}')
    out.append(fr(body))
(w / f"stream-{dev}.bin").write_bytes(b"".join(out))
PY
}
send() {  # $1 = port, $2 = device
  timeout 20 openssl s_client -connect "127.0.0.1:$1" -cert "$CERTS/devices/$2.crt" -key "$KEYS/$2.key" \
    -CAfile "$CERTS/ca/ca.crt" -quiet -ign_eof < "$WORK/stream-$2.bin" > "$WORK/out-$2.bin" 2>/dev/null
  python3 - "$WORK/out-$2.bin" <<'PY'
import json, struct, sys, pathlib
raw = pathlib.Path(sys.argv[1]).read_bytes(); i=0; n=0; okn=0
while i+4 <= len(raw):
    ln = struct.unpack(">I", raw[i:i+4])[0]
    if ln == 0 or i+4+ln > len(raw): break
    m = json.loads(raw[i+4:i+4+ln]); i += 4+ln
    if m.get("kind") == "ack": n += 1; okn += 1 if m.get("accepted") else 0
print(f"acks={n} accepted={okn}")
PY
}
count() { python3 -c "
import sqlite3,sys
try:
    c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True); print(len(c.execute('select alert_id from home_alerts').fetchall()))
except Exception: print('none')" "$1"; }
row() { python3 -c "
import sqlite3,sys
try:
    c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True)
    for r in c.execute('select alert_id,severity,rule_name,round(confidence,3) from home_alerts'): print('   %s | %s | %s | confidence=%s' % r)
except Exception as e: print('   (none)')" "$1"; }

echo "== POSITIVE: two allowlisted detectors, detection-mapped states, one live window =="
POS_STORE="$WORK/alerts.db"
start hgfw-pos "$POS_PORT" "$POS_STORE" '["sensor.motion","sensor.presence"]' || { bad "positive ingress"; exit 1; }
ok "ingress (single writer) on $POS_PORT" "evidence allowlist: sensor.motion + sensor.presence (ADR-014)"
stream "$MOTION_ID" motion motion-changed-fraction motion-changed-fraction on
stream "$PRESENCE_ID" presence availability availability present
note "motion   (state=on):      $(send "$POS_PORT" "$MOTION_ID")"
note "presence (state=present): $(send "$POS_PORT" "$PRESENCE_ID")"
sleep 3
POS_COUNT="$(count "$POS_STORE")"
note "event-window outcome in the app DB:"; row "$POS_STORE"
if [[ "$POS_COUNT" =~ ^[0-9]+$ ]] && [[ "$POS_COUNT" -ge 1 ]]; then
  ok "a detected verdict became an ALERT the writer committed" "alert_count=$POS_COUNT"
else
  bad "detected verdict became an alert" "alert_count=$POS_COUNT"
fi

printf '{"fw33":{"principal":"fusion-window","allow_status":true,"allow_alert_summary":true}}' > "$WORK/creds.json"
chmod 600 "$WORK/creds.json"
docker run -d --name hgfw-mcp --entrypoint hg_mcp_server --network host --user "$DUSER" -v "$WORK:$WORK" \
  -e HG_MCP_PORT="$MCP_PORT" -e HG_MCP_ALERTS_DB="$POS_STORE" "$IMAGE" /etc/homeguardian/hub.json \
  "$WORK/creds.json" >/dev/null 2>&1
for _ in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ":$MCP_PORT " && break; sleep 0.25; done
SID=$(curl -s -D - -o /dev/null -H 'Authorization: Bearer fw33' -H 'Content-Type: application/json' \
      -H 'Accept: application/json, text/event-stream' -X POST "http://127.0.0.1:$MCP_PORT/mcp" \
      -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"fw","version":"1"}}}' \
      | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
READ="$(curl -s -H 'Authorization: Bearer fw33' -H 'Content-Type: application/json' \
      -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $SID" \
      -X POST "http://127.0.0.1:$MCP_PORT/mcp" \
      -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' \
      | python3 -c "
import json,sys
t=sys.stdin.read()
if 'data:' in t: t=[l[6:] for l in t.splitlines() if l.startswith('data: ')][0]
d=json.loads(t); r=d.get('result',{}); c=r.get('content',[{}]); print(c[0].get('text','') if c else d)")"
note "separate READ-ONLY MCP process on $MCP_PORT: $READ"
if printf '%s' "$READ" | grep -q "\"alert_count\":$POS_COUNT" && [[ "$POS_COUNT" -ge 1 ]]; then
  ok "the MCP answer equals what the writer committed" "single writer, separate reader"
else
  bad "MCP answer equals the store" "store=$POS_COUNT read=$READ"
fi
docker rm -f hgfw-mcp >/dev/null 2>&1

echo
echo "== NEGATIVE: availability-only, the state the mapping does NOT call a detection =="
NEG_STORE="$WORK/negative.db"
start hgfw-neg "$NEG_PORT" "$NEG_STORE" '["sensor.camera_availability","sensor.microphone_availability"]' \
  || { bad "negative ingress"; exit 1; }
ok "ingress (separate scope) on $NEG_PORT" "availability evidence allowed to vote, and it still must not"
stream "$MOTION_ID" camera_availability availability availability present
note "availability (state=present -> inconclusive; 'online' would be not_detected): $(send "$NEG_PORT" "$MOTION_ID")"
sleep 3
NEG_COUNT="$(count "$NEG_STORE")"
if [[ "$NEG_COUNT" == "0" ]]; then
  ok "availability-only produced NO alert - the correct answer" "alert_count=0"
  note "unmet condition: no usable positive vote exists for a healthy availability report, so the fused verdict"
  note "can never be 'detected' from this input (SecurityObservation.cpp:89-97 + FusionEngine.cpp:26)."
else
  bad "availability-only produced no alert" "alert_count=$NEG_COUNT"
fi

echo "  --------------------------------------------------------------------"
echo "  fusion window: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] || exit 1
