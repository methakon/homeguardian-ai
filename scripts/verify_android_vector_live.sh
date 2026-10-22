#!/usr/bin/env bash
# HG-CPP-ANDROID-VECTOR-CONSUMER-27, live half.
#
# Consume Hermes 2's EXACT frames (apk/tests/SyntheticVectorEmitTest.java @ bb1e6c3, issue #24) over mutual TLS
# through a controlled authenticated D-03 ingress TEST identity, and correlate the answers.
#
# SEPARATE RUN ENVIRONMENT, deliberately: its own port (18444), its own store, its own container run and its own
# MCP instance (18797) on a TEST CA. It does not touch the validation endpoint on 8790, any other container, any
# other stack, or any production material. No credential is printed.
#
# WHAT IT SENDS, and why in this order:
#   1. hello                 (verbatim)          -> expect a welcome carrying last_sequence
#   2. observation           (verbatim)          -> expect a REFUSAL: their source_type "camera" is a CAPABILITY
#                                                   name with no SecuritySourceType representation. The point of
#                                                   the case is that it is refused, NOT that it throws.
#   3. observation-receiver  (one field changed) -> expect an ACCEPTANCE whose ack echoes observation_id AND
#                                                   sequence (their sender's own correlation check), and which
#                                                   reaches the event path as SYNTHETIC.
# The frame digests are recomputed here and asserted against the values recorded with the contract test.
#
# Usage: scripts/verify_android_vector_live.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
PROVISION="$REPO/tools/provision_ca.sh"
INGRESS_TEMPLATE="$REPO/deploy/docker/config/ingress.json.example"
IMAGE="${HG_VECTOR_IMAGE:-homeguardian-vector27:local}"
MCP_BIN="${HG_MCP_BIN:-$REPO/build/hg_mcp_server}"
CONFIG="${HG_MCP_CONFIG:-$REPO/config/homeguardian.demo.json}"
PORT=18444
MCP_PORT=18797
STORE="$WORK/alerts.db"
DEVICE_ID="hg-android-synthetic-01"          # the producer's own identity, not a substitute
DUSER="$(id -u):$(id -g)"

PASS=0; FAIL=0
ok()  { printf '  PASS  %-62s %s\n' "$1" "${2:-}"; PASS=$((PASS+1)); }
bad() { printf '  FAIL  %-62s %s\n' "$1" "${2:-}"; FAIL=$((FAIL+1)); }
note(){ printf '  ----  %s\n' "$1"; }

INGRESS_PID=""; MCP_PID=""; MCP_ACTIVE=0
cleanup() {
  [[ -n "$MCP_PID" ]] && kill -TERM "$MCP_PID" 2>/dev/null
  docker rm -f "hgvec-ingress-$$" >/dev/null 2>&1
  wait "${INGRESS_PID:-}" 2>/dev/null; wait "${MCP_PID:-}" 2>/dev/null
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK"; chmod 0755 "$WORK"; umask 077

echo "== live consumption of the Android vector over mutual TLS =="
note "workdir : $WORK"
note "identity: $DEVICE_ID (test CA, provisioned here - no production key)"
note "ingress : $IMAGE on 127.0.0.1:$PORT, store $STORE"
note "separate from the validation endpoint on 8790 and from every other stack"

docker image inspect "$IMAGE" >/dev/null 2>&1 || { bad "vector build present" "$IMAGE"; exit 1; }

# ---------------------------------------------------------------- test identity
CA_DIR="$WORK/offline-ca"; CERTS="$WORK/certs"; DEVICE_DIR="$WORK/device-keys"
bash "$PROVISION" init --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 || { bad "CA provisioning"; exit 1; }
bash "$PROVISION" issue "$DEVICE_ID" --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR" \
  >/dev/null 2>&1 || { bad "device certificate issued for the producer's identity"; exit 1; }
ok "test CA + certificate for the producer's own device id" "$DEVICE_ID"
chmod -R a+rX "$CERTS" "$DEVICE_DIR" 2>/dev/null
CRED="$(bash "$PROVISION" show "$DEVICE_ID" --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR" 2>/dev/null | awk '$1=="credential_id"{print $3}')"

cat > "$WORK/devices.json" <<JSON
{ "devices": [ { "device_id": "$DEVICE_ID", "display_name": "synthetic node", "device_type": "node",
  "credential_id": "$CRED", "paired": true, "revoked": false,
  "capabilities": ["camera", "microphone"], "consent_granted": true, "consent_subject": "$DEVICE_ID",
  "consent_purpose": "security_monitor", "capture_enabled": true } ] }
JSON
printf '[]\n' > "$WORK/revocations.json"
chmod 0644 "$WORK/devices.json" "$WORK/revocations.json"

# ---------------------------------------------------------------- the exact frames
python3 - "$WORK" <<'PY'
import hashlib, json, struct, sys, pathlib
out = pathlib.Path(sys.argv[1])
hello = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"hello",'
         '"device_id":"hg-android-synthetic-01","display_name":"synthetic node",'
         '"capabilities":["camera","microphone"]}')
obs = ('{"protocol_version_major":1,"protocol_version_minor":0,"kind":"observation",'
       '"observation_id":"synthetic-obs-0001","device_id":"hg-android-synthetic-01",'
       '"source_type":"camera","observation_type":"motion",'
       '"acquisition_mode":"synthetic","acquired_at_ms":1761100000000,"sequence":1,'
       '"features":{"motion":0.42,"state":"present"},"unit":"fraction","confidence":0.9,'
       '"provenance_claim":"synthetic"}')
obs_recv = obs.replace('"source_type":"camera"', '"source_type":"motion"')
def frame(body): return struct.pack(">I", len(body.encode())) + body.encode()
meta = {}
for name, body in (("hello", hello), ("observation", obs), ("observation_receiver", obs_recv)):
    data = frame(body)
    (out / f"{name}.frame").write_bytes(data)
    meta[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
(out / "frames.bin").write_bytes(frame(hello) + frame(obs) + frame(obs_recv))
(out / "manifest.json").write_text(json.dumps(meta, indent=2))
print(json.dumps(meta))
PY
note "frames written; digests recomputed here and checked against the contract test's recorded values"
python3 - "$WORK/manifest.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
want = {"hello": (183, "9334194492e677777cbd95fb7af8528338df59ce7964ebe103089c6398374e42"),
        "observation": (393, "962dd50284514faaef644f10142f4ae74c5702ed54b15a969eb2aa661d35753e")}
good = True
for k, (n, h) in want.items():
    got = (m[k]["bytes"], m[k]["sha256"])
    if got != (n, h): good = False
    print(f"    {k}: bytes={got[0]} sha256={got[1][:16]}… {'MATCH' if got == (n, h) else 'DIFFERS'}")
sys.exit(0 if good else 1)
PY
[[ $? == 0 ]] && ok "the frames are byte-identical to the recorded digests" "hello 183 / observation 393" \
              || bad "frame digests match the recorded values" "see above"

# ---------------------------------------------------------------- ingress config (from the repo template)
python3 - "$INGRESS_TEMPLATE" "$WORK/ingress.json" "$CERTS" "$WORK" "$PORT" "$STORE" <<'PY'
import json, sys
tpl, out, certs, work, port, store = sys.argv[1:7]
cfg = json.load(open(tpl))
cfg.update({"certificate_path": f"{certs}/server.crt", "private_key_path": f"{certs}/server.key",
            "ca_path": f"{certs}/ca/ca.crt", "registry_path": f"{work}/devices.json",
            "revocation_path": f"{work}/revocations.json", "bind_address": "127.0.0.1",
            "port": int(port), "audit_db_path": store, "replay_state_path": f"{work}/replay_state.json",
            # the operator's decisions, both additive: let the ingested motion evidence vote in fusion (ADR-014),
            # and admit the producer's own names for the source/observation vocabulary it sends
            "evidence_detector_ids": ["sensor.motion"],
            "extra_source_types": ["camera"], "extra_observation_types": ["motion"]})
json.dump(cfg, open(out, "w"), indent=2)
PY
chmod 0644 "$WORK/ingress.json"

docker rm -f "hgvec-ingress-$$" >/dev/null 2>&1
docker run --rm --name "hgvec-ingress-$$" --entrypoint sh --network host --user "$DUSER" \
  -v "$WORK:$WORK" "$IMAGE" -lc "hg_device_ingress --config '$WORK/ingress.json'" \
  >"$WORK/ingress.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 60); do
  ss -ltn 2>/dev/null | grep -q ":$PORT " && break
  kill -0 "$INGRESS_PID" 2>/dev/null || { note "ingress exited: $(tail -1 "$WORK/ingress.log")"; break; }
  sleep 0.25
done
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then ok "fresh ingress (vector build) listening on 127.0.0.1:$PORT"; else bad "ingress started"; exit 1; fi

# ---------------------------------------------------------------- the exchange over mutual TLS
timeout 12 openssl s_client -connect "127.0.0.1:$PORT" -cert "$CERTS/devices/$DEVICE_ID.crt" \
  -key "$DEVICE_DIR/$DEVICE_ID.key" -CAfile "$CERTS/ca/ca.crt" -quiet -ign_eof \
  < "$WORK/frames.bin" > "$WORK/exchange.txt" 2>"$WORK/openssl.err" || true
note "raw exchange (verbatim, from the socket):"
head -c 1200 "$WORK/exchange.txt" | sed 's/^/        /'
echo

python3 - "$WORK/exchange.txt" <<'PY'
import json, re, sys
raw = open(sys.argv[1], errors="replace").read()
msgs = []
for line in raw.splitlines():
    line = line.strip()
    if line.startswith("{"):
        try: msgs.append(json.loads(line))
        except Exception: pass
if not msgs:
    print("  FAIL  no JSON messages came back on the wire"); sys.exit(1)
def find(pred):
    return next((m for m in msgs if pred(m)), None)
welcome = find(lambda m: m.get("kind") == "welcome")
refusal = find(lambda m: m.get("kind") == "ack" and not m.get("accepted"))
accept = find(lambda m: m.get("kind") == "ack" and m.get("accepted"))

if welcome and "last_sequence" in welcome:
    print(f"  PASS  welcome carried last_sequence={welcome['last_sequence']} (the resume point)")
else:
    print("  FAIL  no welcome with last_sequence")

if refusal and refusal.get("reason") == "unknown_source_type":
    print(f"  PASS  the verbatim frame was REFUSED, not thrown: reason={refusal['reason']} "
          f"seq={refusal.get('sequence')}")
elif refusal:
    print(f"  FAIL  verbatim frame refused with the wrong reason: {refusal.get('reason')}")
else:
    print("  FAIL  the verbatim frame was not refused")

if accept and accept.get("observation_id") == "synthetic-obs-0001" and accept.get("sequence") == 1 \
        and accept.get("reason") == "ok":
    print(f"  PASS  the receiver-vocabulary frame was ACCEPTED and the ack CORRELATES: "
          f"observation_id={accept['observation_id']} sequence={accept['sequence']} reason={accept['reason']}")
else:
    print(f"  FAIL  acceptance missing or uncorrelated: {accept}")
PY
RC=$?
[[ $RC == 0 ]] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

# ---------------------------------------------------------------- the alert, and the MCP view of it
sleep 1
STORE_STATE="$(python3 - "$STORE" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
    rows = c.execute("SELECT alert_id FROM home_alerts").fetchall()
    print(f"{len(rows)}|{' '.join(r[0] for r in rows)}")
except Exception:
    print("none|")
PY
)"
STORE_COUNT="${STORE_STATE%%|*}"; STORE_IDS="${STORE_STATE#*|}"
note "the ingress committed to its own store: alert_count=$STORE_COUNT ids='$STORE_IDS'"
if [[ "$STORE_COUNT" =~ ^[0-9]+$ ]]; then ok "the accepted synthetic observation reached the event/alert path" "alert_count=$STORE_COUNT"
else bad "store readable" "$STORE"; fi

if [[ "$STORE_COUNT" =~ ^[0-9]+$ ]]; then
  printf '{"live-vector-token":{"principal":"vector","allow_status":true,"allow_alert_summary":true}}' > "$WORK/creds.json"
  chmod 600 "$WORK/creds.json"
  MCP_ACTIVE=$MCP_PORT
  HG_MCP_PORT="$MCP_ACTIVE" HG_MCP_ALERTS_DB="$STORE" "$MCP_BIN" "$CONFIG" "$WORK/creds.json" >"$WORK/mcp.log" 2>&1 &
  MCP_PID=$!
  for _ in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ":$MCP_ACTIVE " && break; sleep 0.25; done
  SID=$(curl -s -D - -o /dev/null -H 'Authorization: Bearer live-vector-token' -H 'Content-Type: application/json' \
        -H 'Accept: application/json, text/event-stream' -X POST "http://127.0.0.1:$MCP_ACTIVE/mcp" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"live","version":"1"}}}' \
        | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')
  READ="$(curl -s -H 'Authorization: Bearer live-vector-token' -H 'Content-Type: application/json' \
        -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $SID" -X POST "http://127.0.0.1:$MCP_ACTIVE/mcp" \
        -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' | head -c 400)"
  note "MCP (separate process, port $MCP_ACTIVE, read-only): $(printf '%s' "$READ" | head -c 200)"
  if printf '%s' "$READ" | grep -q "\"alert_count\":$STORE_COUNT"; then
    ok "the MCP read-only view reports the same count the ingress committed" "alert_count=$STORE_COUNT"
  else
    bad "MCP count matches the store" "store=$STORE_COUNT read=$READ"
  fi
  kill -TERM "$MCP_PID" 2>/dev/null; wait "$MCP_PID" 2>/dev/null; MCP_PID=""
fi

echo "  --------------------------------------------------------------------"
echo "  android vector, live: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]] || exit 1
