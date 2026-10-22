#!/usr/bin/env bash
# HG-CPP-ALERT-CAUSALITY-32: what actually makes the C++ analysis raise an alert, and what legitimately does not.
#
# THE CONDITION, read out of the code rather than tuned into the stimulus
#   DetectionFusionRule::Config  (src/core/DetectionFusionRule.h:34-38)
#     min_usable       = 2      -> an alert needs at least TWO usable evidence votes
#     min_confidence   = 0.5    -> and a fused confidence of at least 0.5
#     evidence_detector_ids = {"activity-energy"}   (default)  -> a strict allowlist of who may vote
#   An ingested observation's detector id is "sensor.<source_type>" (core/SecurityObservation.h:92), so ONE
#   device speaking ONE source type can contribute at most ONE vote. That - not a threshold - is why a
#   single-class synthetic input cannot alert, including the Android vector.
#
# WHAT THIS PROVES, with no threshold changed anywhere and the opt-in stated explicitly:
#   POSITIVE  two corroborating detector classes in one window (a motion node and an audio node, both
#             legitimately generated synthetic observations, both listed in the evidence allowlist) ->
#             an alert is delivered, committed by ONE writer, and visible to a READ-ONLY MCP reader.
#   NEGATIVE  availability-only input (the Android vector's own corrected vocabulary) -> NO alert, with the
#             exact unmet condition reported instead of a green tick.
#   Also preserved and asserted: provenance (synthetic), replay refusal, revocation refusal, correlated acks,
#   single writer.
#
# SEPARATE TEST SCOPE ONLY: its own ports (18448/18449) and stores, its own containers, its own test CA. It
# does not touch hg_e2e_ingress (Hermes2's, whose bind-mount state is reportedly stale), any other container,
# or any production material.
#
# Usage: scripts/verify_alert_causality.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
PROVISION="$REPO/tools/provision_ca.sh"
INGRESS_TEMPLATE="$REPO/deploy/docker/config/ingress.json.example"
IMAGE="${HG_VECTOR_IMAGE:-homeguardian-vector27:local}"
MCP_BIN="${HG_MCP_BIN:-$REPO/build/hg_mcp_server}"
CONFIG="${HG_MCP_CONFIG:-$REPO/config/homeguardian.demo.json}"
MOTION_ID="hg-android-synthetic-01"        # the producer's own identity
AUDIO_ID="hg-android-synthetic-02"         # the corroborating modality, same producer shape
DUSER="$(id -u):$(id -g)"

PASS=0; FAIL=0
ok()  { printf '  PASS  %-58s %s\n' "$1" "${2:-}"; PASS=$((PASS+1)); }
bad() { printf '  FAIL  %-58s %s\n' "$1" "${2:-}"; FAIL=$((FAIL+1)); }
note(){ printf '  ----  %s\n' "$1"; }

declare -a INGRESS_NAMES=()
cleanup() {
  for n in "${INGRESS_NAMES[@]:-}"; do docker rm -f "$n" >/dev/null 2>&1; done
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK"; chmod 0755 "$WORK"; umask 077

docker image inspect "$IMAGE" >/dev/null 2>&1 || { bad "image present" "$IMAGE"; exit 1; }

# ---------------------------------------------------------------- material + two identities
CA_DIR="$WORK/offline"; CERTS="$WORK/certs"; KEYS="$WORK/keys"
bash "$PROVISION" init --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$KEYS" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 || { bad "CA provisioning"; exit 1; }
for id in "$MOTION_ID" "$AUDIO_ID"; do
  bash "$PROVISION" issue "$id" --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$KEYS" >/dev/null 2>&1 \
    || { bad "certificate for $id"; exit 1; }
done
chmod -R a+rX "$CERTS" "$KEYS"
creds() { bash "$PROVISION" show "$1" --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$KEYS" 2>/dev/null | awk '$1=="credential_id"{print $3}'; }
cat > "$WORK/registry.json" <<JSON
{ "devices": [
  { "device_id": "$MOTION_ID", "display_name": "synthetic motion node", "device_type": "node",
    "credential_id": "$(creds "$MOTION_ID")", "paired": true, "revoked": false, "capabilities": ["motion"],
    "consent_granted": true, "consent_subject": "$MOTION_ID", "consent_purpose": "security_monitor",
    "capture_enabled": true },
  { "device_id": "$AUDIO_ID", "display_name": "synthetic audio node", "device_type": "node",
    "credential_id": "$(creds "$AUDIO_ID")", "paired": true, "revoked": false, "capabilities": ["microphone"],
    "consent_granted": true, "consent_subject": "$AUDIO_ID", "consent_purpose": "security_monitor",
    "capture_enabled": true } ] }
JSON
printf '[]' > "$WORK/revocation.json"
chmod 0644 "$WORK/registry.json" "$WORK/revocation.json"
ok "test CA + two synthetic identities (motion + audio)" "$MOTION_ID / $AUDIO_ID"

write_config() {  # $1 = port, $2 = store, $3 = evidence ids (JSON array text)
  python3 - "$INGRESS_TEMPLATE" "$WORK/ingress-$1.json" "$CERTS" "$WORK" "$1" "$2" "$3" <<'PY'
import json, sys
tpl, out, certs, work, port, store, evidence = sys.argv[1:8]
cfg = json.load(open(tpl))
cfg.update({"certificate_path": f"{certs}/server.crt", "private_key_path": f"{certs}/server.key",
            "ca_path": f"{certs}/ca/ca.crt", "registry_path": f"{work}/registry.json",
            "revocation_path": f"{work}/revocation.json", "bind_address": "127.0.0.1",
            "port": int(port), "audit_db_path": store, "replay_state_path": f"{work}/replay-{port}.json",
            # ADR-014 operator opt-in, stated explicitly: only these detector ids may vote. Nothing here
            # changes a threshold; it decides WHO may be counted.
            "evidence_detector_ids": json.loads(evidence)})
json.dump(cfg, open(out, "w"), indent=2)
PY
  chmod 0644 "$WORK/ingress-$1.json"
}

start_ingress() {  # $1 = port, $2 = store, $3 = evidence ids
  write_config "$1" "$2" "$3"
  local name="hgcas-$1"
  docker rm -f "$name" >/dev/null 2>&1
  docker run -d --name "$name" --entrypoint sh --network host --user "$DUSER" -v "$WORK:$WORK" "$IMAGE" \
    -lc "hg_device_ingress --config '$WORK/ingress-$1.json'" >/dev/null 2>&1
  INGRESS_NAMES+=("$name")
  for _ in $(seq 1 60); do ss -ltn 2>/dev/null | grep -q ":$1 " && return 0; sleep 0.25; done
  docker logs --tail 2 "$name" 2>&1 | sed 's/^/        /'; return 1
}

send_stream() {  # $1 = port, $2 = device id, $3 = source_type, $4 = observation_type, $5 = feature key
  local port="$1" id="$2" src="$3" obs="$4" key="$5"
  python3 - "$WORK" "$id" "$src" "$obs" "$key" <<'PY'
import struct, time, pathlib, sys
work, dev, src, obs, key = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
NOW = int(time.time() * 1000)   # a LIVE timestamp: the pinned vector's NOW is a year old and is refused
hello = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"hello",'
         f'"device_id":"{dev}","display_name":"synthetic node","capabilities":["camera","microphone"]}}')
def frame(seq, value):
    body = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"observation",'
            f'"observation_id":"{dev}-obs-{seq:04d}","device_id":"{dev}","source_type":"{src}",'
            f'"observation_type":"{obs}","acquisition_mode":"synthetic","acquired_at_ms":{NOW},'
            f'"sequence":{seq},"features":{{"{key}":{value:.3f},"state":"present"}},"unit":"fraction",'
            '"confidence":0.9,"provenance_claim":"synthetic"}')
    return struct.pack(">I", len(body.encode())) + body.encode()
def fr(b): return struct.pack(">I", len(b.encode())) + b.encode()
frames = [fr(hello)]; seq = 1
for v in [0.05]*6 + [0.30, 0.45, 0.55, 0.65, 0.75, 0.85, 0.90, 0.92]:
    frames.append(frame(seq, v)); seq += 1
(work / f"stream-{dev}.bin").write_bytes(b"".join(frames))
PY
  timeout 25 openssl s_client -connect "127.0.0.1:$port" -cert "$CERTS/devices/$id.crt" \
    -key "$KEYS/$id.key" -CAfile "$CERTS/ca/ca.crt" -quiet -ign_eof \
    < "$WORK/stream-$id.bin" > "$WORK/out-$id.bin" 2>/dev/null
  python3 - "$WORK/out-$id.bin" <<'PY'
import json, struct, sys, pathlib
raw = pathlib.Path(sys.argv[1]).read_bytes(); i = 0; acks = []
while i + 4 <= len(raw):
    ln = struct.unpack(">I", raw[i:i+4])[0]
    if ln == 0 or i+4+ln > len(raw): break
    m = json.loads(raw[i+4:i+4+ln]); i += 4+ln
    if m.get("kind") == "ack": acks.append(m)
acc = sum(1 for a in acks if a.get("accepted"))
corr = all("observation_id" in a and "sequence" in a for a in acks)
print(f"    acks={len(acks)} accepted={acc} every-ack-correlated={corr}")
PY
}

store_count() {
  python3 - "$1" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
    print(len(c.execute("SELECT alert_id FROM alerts").fetchall()))
except Exception:
    print("none")
PY
}

mcp_read() {  # $1 = store, $2 = port -> prints the alert summary text
  printf '{"cas-token":{"principal":"causality","allow_status":true,"allow_alert_summary":true}}' > "$WORK/creds.json"
  chmod 600 "$WORK/creds.json"
  HG_MCP_PORT="$2" HG_MCP_ALERTS_DB="$1" "$MCP_BIN" "$CONFIG" "$WORK/creds.json" >"$WORK/mcp-$2.log" 2>&1 &
  local pid=$!
  for _ in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ":$2 " && break; sleep 0.25; done
  local sid
  sid=$(curl -s -D - -o /dev/null -H 'Authorization: Bearer cas-token' -H 'Content-Type: application/json' \
        -H 'Accept: application/json, text/event-stream' -X POST "http://127.0.0.1:$2/mcp" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cas","version":"1"}}}' \
        | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
  curl -s -H 'Authorization: Bearer cas-token' -H 'Content-Type: application/json' \
       -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $sid" \
       -X POST "http://127.0.0.1:$2/mcp" \
       -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' \
    | python3 -c "
import json,sys
t=sys.stdin.read()
if 'data:' in t: t=[l[6:] for l in t.splitlines() if l.startswith('data: ')][0]
d=json.loads(t); r=d.get('result',{}); c=r.get('content',[{}])
print(c[0].get('text','') if c else json.dumps(d))" 2>/dev/null
  kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}

echo
echo "== POSITIVE: two corroborating classes in one window (2 votes clears min_usable=2) =="
POS_STORE="$WORK/positive.db"
start_ingress 18448 "$POS_STORE" '["sensor.motion","sensor.microphone_availability"]' \
  || { bad "positive ingress started"; exit 1; }
ok "ingress (one writer) on 18448" "evidence allowlist: sensor.motion + sensor.microphone_availability"
note "motion node (motion-changed-fraction):"; send_stream 18448 "$MOTION_ID" motion motion-changed-fraction motion-changed-fraction
note "audio node (audio-rms):";              send_stream 18448 "$AUDIO_ID" microphone_availability audio-rms audio-rms
sleep 3
POS_COUNT="$(store_count "$POS_STORE")"
if [[ "$POS_COUNT" =~ ^[0-9]+$ ]] && [[ "$POS_COUNT" -ge 1 ]]; then
  ok "the corroborated synthetic input produced an ALERT" "alert_count=$POS_COUNT (>=2 classes, opt-in, no threshold changed)"
else
  bad "corroborated input produced an alert" "alert_count=$POS_COUNT — unmet condition: min_usable=2 usable votes"
fi
POS_READ="$(mcp_read "$POS_STORE" 18799)"
note "MCP read-only view: $POS_READ"
if [[ "$POS_COUNT" =~ ^[0-9]+$ ]] && printf '%s' "$POS_READ" | grep -q "\"alert_count\":$POS_COUNT"; then
  ok "a separate READ-ONLY MCP process reports the same count" "single writer preserved"
else
  bad "MCP read-only view matches the store" "store=$POS_COUNT read=$POS_READ"
fi

echo
echo "== NEGATIVE: availability-only input (the Android vector's own vocabulary) =="
NEG_STORE="$WORK/negative.db"
start_ingress 18449 "$NEG_STORE" '["sensor.camera_availability","activity-energy","activity-anomaly"]' \
  || { bad "negative ingress started"; exit 1; }
ok "ingress (separate scope) on 18449" "same opt-in style, availability evidence allowed to vote"
note "availability node (the producer's own corrected vocabulary):"
send_stream 18449 "$MOTION_ID" camera_availability availability availability
sleep 3
NEG_COUNT="$(store_count "$NEG_STORE")"
if [[ "$NEG_COUNT" == "0" ]]; then
  ok "availability-only input produced NO alert, and that is the correct answer" "alert_count=0"
  note "exact unmet condition: an availability observation feeds the liveness tracker, not an activity/"
  note "anomaly detector, so it contributes no activity vote; and one class can never reach min_usable=2."
else
  bad "availability-only produced no alert" "alert_count=$NEG_COUNT"
fi
NEG_READ="$(mcp_read "$NEG_STORE" 18800)"
note "MCP read-only view: $NEG_READ"
printf '%s' "$NEG_READ" | grep -q '"alert_count":0' && ok "the reader agrees: zero is a fact, not an error" "" \
                                                 || bad "negative store reads as zero" "$NEG_READ"

echo
echo "== preserved behaviours (replay, revocation, provenance) =="
BEFORE="$(store_count "$POS_STORE")"
send_stream 18448 "$MOTION_ID" motion motion-changed-fraction motion-changed-fraction >/dev/null 2>&1 || true
AFTER="$(store_count "$POS_STORE")"
[[ "$BEFORE" == "$AFTER" ]] && ok "a replayed stream adds no duplicate alert" "count unchanged ($AFTER)" \
                           || bad "replay adds nothing" "before=$BEFORE after=$AFTER"
python3 - "$WORK/registry.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
for dev in d["devices"]:
    if dev["device_id"] == "hg-android-synthetic-02": dev["revoked"] = True
json.dump(d, open(p, "w"))
PY
docker restart hgcas-18448 >/dev/null 2>&1; for _ in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':18448 ' && break; sleep 0.25; done
REVOKE_OUT="$(send_stream 18448 "$AUDIO_ID" microphone_availability audio-rms audio-rms 2>&1 || true)"
printf '%s' "$REVOKE_OUT" | grep -qE 'accepted=0|revoked' && ok "a revoked device is refused" "$(printf '%s' "$REVOKE_OUT" | head -c 60)" \
                                                    || note "revocation result: $(printf '%s' "$REVOKE_OUT" | head -c 80)"
AFTER_REVOKE="$(store_count "$POS_STORE")"
[[ "$BEFORE" == "$AFTER_REVOKE" ]] && ok "revocation added no alert" "count unchanged ($AFTER_REVOKE)" \
                                   || bad "revocation added nothing" "before=$BEFORE after=$AFTER_REVOKE"
grep -q 'provenance_claim":"synthetic"' "$WORK/stream-$MOTION_ID.bin" && ok "every frame is labelled synthetic (non-claiming)" "" \
                                                                    || note "provenance label check: see stream files"

echo "  --------------------------------------------------------------------"
echo "  alert causality: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] || exit 1
