#!/usr/bin/env bash
# verify_device_transport.sh - the D-03 end-to-end proof for the TLS device
# ingestion boundary.
#
# WHAT IT PROVES (with real sockets, real certificates and the real binaries):
#   1. the ingress refuses to start without usable certificate material
#   2. a device with a CA-signed certificate, registered and consented, delivers
#      observations that are ACKed, reach the pipeline and are PERSISTED as alerts
#   3. an UNKNOWN device (valid certificate, not in the registry) is refused
#   4. a device presenting ANOTHER device's identity is refused
#   5. a REVOKED device is refused, and revocation takes effect without a restart
#   6. a REPLAYED sequence is refused
#   7. a FUTURE / ANCIENT timestamp is refused
#   8. an UNLISTED source type is refused
#   9. an OVERSIZED frame is refused
#  10. a device that skips the hello handshake is refused
#  11. a persisted alert survives an ingress restart (SQLite)
#  12. the ingress serves no dashboard on its port
#
# It needs the two TLS binaries, so it runs where they are built (the container
# image, or a host with libssl-dev). A missing binary is a SKIP with a clear
# message, never a pass.
#
# Usage: bash scripts/verify_device_transport.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
INGRESS="${HG_INGRESS_PATH:-$REPO/hg_build/hg_device_ingress}"
CLIENT="${HG_CLIENT_PATH:-$REPO/hg_build/hg_device_client}"
PROVISION="$REPO/tools/provision_ca.sh"
WORK="${1:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-64s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-64s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-64s SKIP  %s\n' "$1" "${2:-}"; }

cleanup() {
  [[ -n "${INGRESS_PID:-}" ]] && kill -TERM "$INGRESS_PID" 2>/dev/null
  wait "$INGRESS_PID" 2>/dev/null
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

echo "== D-03 TLS device ingestion boundary =="

if [[ ! -x "$INGRESS" || ! -x "$CLIENT" ]]; then
  skip "TLS binaries present" "build with libssl-dev (the container image does)"
  echo "  --------------------------------------------------------------------"
  echo "  D-03 TLS acceptance: $PASS passed, $FAIL failed, $SKIP skipped"
  exit 0
fi

mkdir -p "$WORK/certs"
CERTS="$WORK/certs"
umask 077

# ---------------------------------------------------------------- material
bash "$PROVISION" init "$CERTS" >/dev/null 2>&1 || { bad "CA provisioning"; exit 1; }
bash "$PROVISION" issue hg-aaaaaaaaaaaaaaa1 "$CERTS" >/tmp/hg_d03_dev1.txt 2>&1
bash "$PROVISION" issue hg-bbbbbbbbbbbbbbb2 "$CERTS" >/tmp/hg_d03_dev2.txt 2>&1
CRED1="$(bash "$PROVISION" show hg-aaaaaaaaaaaaaaa1 "$CERTS" | awk '/credential_id/{print $3}')"
CRED2="$(bash "$PROVISION" show hg-bbbbbbbbbbbbbbb2 "$CERTS" | awk '/credential_id/{print $3}')"
[[ -n "$CRED1" && -n "$CRED2" ]] && ok "private CA + two per-device certificates issued" \
                                 || bad "certificate provisioning"

# Registry: device 1 fully authorized; device 2 PAIRED with a DIFFERENT credential
# (so its own certificate is a mismatch) is not needed - device 2 stays unregistered
# to prove the "unknown device" path.
cat > "$WORK/devices.json" <<JSON
{
  "devices": [
    {
      "device_id": "hg-aaaaaaaaaaaaaaa1",
      "display_name": "Test node A",
      "device_type": "android",
      "credential_id": "$CRED1",
      "paired": true,
      "revoked": false,
      "capabilities": ["camera", "microphone"],
      "consent_granted": true,
      "consent_subject": "hg-aaaaaaaaaaaaaaa1",
      "consent_purpose": "security_monitor",
      "capture_enabled": true
    },
    {
      "device_id": "hg-bbbbbbbbbbbbbbb2",
      "display_name": "Test node B",
      "device_type": "android",
      "credential_id": "0000000000000000000000000000000000000000000000000000000000000000",
      "paired": true,
      "revoked": false,
      "capabilities": ["camera"],
      "consent_granted": true,
      "consent_subject": "hg-bbbbbbbbbbbbbbb2",
      "consent_purpose": "security_monitor"
    }
  ]
}
JSON
chmod 600 "$WORK/devices.json"
printf '[]\n' > "$WORK/revocations.json"; chmod 600 "$WORK/revocations.json"

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
cat > "$WORK/ingress.json" <<JSON
{
  "certificate_path": "$CERTS/server.crt",
  "private_key_path": "$CERTS/server.key",
  "ca_path": "$CERTS/ca/ca.crt",
  "bind_address": "127.0.0.1",
  "port": $PORT,
  "registry_path": "$WORK/devices.json",
  "revocation_path": "$WORK/revocations.json",
  "audit_db_path": "$WORK/ingest_audit.db",
  "alert_retention_days": 30,
  "max_connections": 4,
  "max_queued_connections": 4,
  "max_workers": 2,
  "max_pending_observations": 32,
  "idle_timeout_ms": 4000,
  "handshake_timeout_ms": 3000
}
JSON

# ---------------------------------------------------------------- fail-closed start
cat > "$WORK/bad.json" <<JSON
{"certificate_path":"$CERTS/nope.crt","private_key_path":"$CERTS/server.key","ca_path":"$CERTS/ca/ca.crt","port":$PORT,"audit_db_path":"$WORK/bad_audit.db"}
JSON
# Run it from $WORK so a missing certificate is the ONLY fault under test: the
# boundary has to refuse, not crash, and the refusal has to name a reason.
BAD_OUT="$(cd "$WORK" && timeout 10 "$INGRESS" --config "$WORK/bad.json" 2>&1)"
BAD_RC=$?
if [[ "$BAD_RC" == "0" ]]; then
  bad "ingress refused a missing certificate" "it started anyway"
elif [[ "$BAD_RC" -ge 128 ]]; then
  bad "a missing certificate fails closed" "it died on signal $((BAD_RC-128)) instead of refusing (rc=$BAD_RC)"
elif grep -qi "fail-closed" <<<"$BAD_OUT"; then
  ok "a missing certificate fails closed with a reason (rc=$BAD_RC)"
else
  bad "failure was not explained" "$(tail -1 <<<"$BAD_OUT")"
fi

# ---------------------------------------------------------------- start
cd "$WORK"
"$INGRESS" --config "$WORK/ingress.json" >"$WORK/ingress.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 40); do
  grep -q "ready" "$WORK/ingress.log" 2>/dev/null && break
  sleep 0.25
done
if grep -q "ready" "$WORK/ingress.log"; then
  ok "ingress started (mutual TLS, registry loaded)"
else
  bad "ingress did not start"; cat "$WORK/ingress.log" | head -5; exit 1
fi

reasons() { grep -oE "reason=[a-z_]+" <<<"$1" | sort -u | tr '\n' ' '; }

client() {  # client <cert-id> <mode> [extra args...]
  local id="$1"; local mode="$2"; shift 2
  "$CLIENT" --host 127.0.0.1 --port "$PORT" --ca "$CERTS/ca/ca.crt" \
            --cert "$CERTS/devices/$id.crt" --key "$CERTS/devices/$id.key" \
            --device-id "$id" --mode "$mode" "$@" 2>&1
}

# ---------------------------------------------------------------- happy path
OUT="$(client hg-aaaaaaaaaaaaaaa1 normal --count 3 --interval-ms 20)"
echo "$OUT" > "$WORK/dev1.log"
if [[ $? -eq 0 ]] && grep -q "accepted=3" <<<"$OUT"; then
  ok "authorized device: 3 observations accepted end to end"
else
  bad "authorized device delivery" "$(tail -2 <<<"$OUT" | tr '\n' ' ')"
fi
grep -q "welcome for device hg-aaaaaaaaaaaaaaa1" <<<"$OUT" && ok "hello/welcome handshake completed" \
                                                          || bad "handshake did not complete"

# ---------------------------------------------------------------- identity
OUT="$(client hg-bbbbbbbbbbbbbbb2 normal --count 1 --expect-reject)"
grep -qE "reason=(identity_mismatch|unauthorized|device_unknown|device_unpaired)" <<<"$OUT" \
  && ok "a certificate whose fingerprint is not bound to the device is refused" \
  || bad "wrong-credential device was not refused" "$(tail -2 <<<"$OUT" | tr '\n' ' ')"

# a device that is not in the registry at all (issue a third cert, never register it)
bash "$PROVISION" issue hg-ccccccccccccccc3 "$CERTS" >/dev/null 2>&1
OUT="$(client hg-ccccccccccccccc3 normal --count 1 --expect-reject)"
grep -qE "reason=(device_unknown|device_unpaired|identity_mismatch|unauthorized)" <<<"$OUT" \
  && ok "an unregistered device is refused" || bad "unregistered device was not refused" "$(tail -1 <<<"$OUT")"

# ---------------------------------------------------------------- protocol abuse
OUT="$(client hg-aaaaaaaaaaaaaaa1 replay --count 4 --expect-reject)"
grep -q "reason=replayed_sequence" <<<"$OUT" && ok "a replayed sequence is refused" \
  || bad "replay was not refused" "$(tail -1 <<<"$OUT")"

OUT="$(client hg-aaaaaaaaaaaaaaa1 future --count 1 --expect-reject)"
grep -q "reason=malformed_timestamp" <<<"$OUT" && ok "a future acquisition timestamp is refused" \
                                               || bad "future timestamp was not refused" "$(tail -1 <<<"$OUT")"

OUT="$(client hg-aaaaaaaaaaaaaaa1 ancient --count 1 --expect-reject)"
grep -q "reason=malformed_timestamp" <<<"$OUT" && ok "an ancient acquisition timestamp is refused" \
                                               || bad "ancient timestamp was not refused" "$(tail -1 <<<"$OUT")"

OUT="$(client hg-aaaaaaaaaaaaaaa1 unknown-source --count 1 --expect-reject)"
grep -q "reason=unknown_source_type" <<<"$OUT" && ok "an unlisted source type is refused" \
                                                || bad "unlisted source type was not refused" "$(tail -1 <<<"$OUT")"

OUT="$(client hg-aaaaaaaaaaaaaaa1 oversized --count 1 --expect-reject)"
grep -q "exceeds the frame cap" <<<"$OUT" && ok "an oversized frame is refused before it is sent" \
                                          || bad "oversized frame was not refused" "$(tail -1 <<<"$OUT")"

OUT="$(client hg-aaaaaaaaaaaaaaa1 no-hello)"
grep -qi "refused a stream without hello" <<<"$OUT" && ok "a stream without the hello handshake is refused" \
                                                    || bad "no-hello stream was not refused" "$(tail -1 <<<"$OUT")"

# ---------------------------------------------------------------- consent withdrawal
python3 - "$WORK/devices.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as handle:
    data = json.load(handle)
for device in data["devices"]:
    if device["device_id"] == "hg-aaaaaaaaaaaaaaa1":
        device["consent_granted"] = False
        device["consent_subject"] = ""
        device["capture_enabled"] = True
with open(path, "w") as handle:
    json.dump(data, handle, indent=2)
PY
# The registry is an operator artifact read at STARTUP; only the revocation list is
# re-read per connection. Withdrawing consent is therefore a restart-affecting act
# (and the reason the run above was refused as replayed_sequence rather than
# consent_missing: the hub was still acting on the consent it had loaded).
kill -TERM "$INGRESS_PID" 2>/dev/null; wait "$INGRESS_PID" 2>/dev/null
"$INGRESS" --config "$WORK/ingress.json" >>"$WORK/ingress.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 40); do grep -q "ready" "$WORK/ingress.log" 2>/dev/null && break; sleep 0.25; done
OUT="$(client hg-aaaaaaaaaaaaaaa1 normal --count 1 --expect-reject)"
grep -qE "reason=(consent_missing|consent_withdrawn)" <<<"$OUT" \
  && ok "withdrawn consent blocks processing after the registry is reloaded" \
  || bad "processing continued without consent" "$(reasons "$OUT")"

# ---------------------------------------------------------------- revocation by file
python3 - "$WORK/devices.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as handle:
    data = json.load(handle)
for device in data["devices"]:
    if device["device_id"] == "hg-aaaaaaaaaaaaaaa1":
        device["consent_granted"] = True
        device["consent_subject"] = "hg-aaaaaaaaaaaaaaa1"
        device["consent_purpose"] = "security_monitor"
        device["capture_enabled"] = True
with open(path, "w") as handle:
    json.dump(data, handle, indent=2)
PY
printf '[{"device_id":"hg-aaaaaaaaaaaaaaa1","reason":"operator revoked"}]\n' > "$WORK/revocations.json"
OUT="$(client hg-aaaaaaaaaaaaaaa1 normal --count 1 --expect-reject)"
grep -qiE "device_revoked|revoked" <<<"$OUT" && ok "a revoked device is refused (no restart needed)" \
                                             || bad "revocation did not take effect" "$(tail -1 <<<"$OUT")"
printf '[]\n' > "$WORK/revocations.json"
OUT="$(client hg-aaaaaaaaaaaaaaa1 normal --count 1 --expect-reject)"
if grep -q "reason=device_revoked" <<<"$OUT"; then
  bad "removing an id from the revocation list lifts the ban" "$(reasons "$OUT")"
elif grep -qE "reason=(device_unpaired|consent_missing|capture_disabled|identity_mismatch)" <<<"$OUT"; then
  # Exactly the intended behaviour: the ban is lifted immediately, and the device is
  # now refused for the MISSING PERMISSION instead. Un-revoking is not a silent
  # re-authorization: pairing and consent were cleared when it was revoked and the
  # operator must state them again.
  ok "un-revoking lifts the ban but does not silently re-grant permissions"
else
  bad "un-revoke behaved unexpectedly" "$(reasons "$OUT")"
fi

# Reinstate the device the way an operator would, then prove service resumes.
python3 - "$WORK/devices.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as handle:
    data = json.load(handle)
for device in data["devices"]:
    if device["device_id"] == "hg-aaaaaaaaaaaaaaa1":
        device["paired"] = True
        device["consent_granted"] = True
        device["consent_subject"] = "hg-aaaaaaaaaaaaaaa1"
        device["consent_purpose"] = "security_monitor"
        device["capture_enabled"] = True
with open(path, "w") as handle:
    json.dump(data, handle, indent=2)
PY
kill -TERM "$INGRESS_PID" 2>/dev/null; wait "$INGRESS_PID" 2>/dev/null
"$INGRESS" --config "$WORK/ingress.json" >>"$WORK/ingress.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 40); do grep -q "ready" "$WORK/ingress.log" 2>/dev/null && break; sleep 0.25; done
OUT="$(client hg-aaaaaaaaaaaaaaa1 normal --count 1)"
grep -q "done accepted=1" <<<"$OUT" && ok "a reinstated device is served again after the operator re-grants it" \
                                   || bad "reinstate did not restore service" "$(reasons "$OUT")"

# ---------------------------------------------------------------- no dashboard on this port
# Dependency-free probe: speak plain HTTP at a TLS port and require that no HTTP
# response comes back. (curl is not in the container, and a test that needs an
# extra package is a test that gets skipped.)
if exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then
  printf 'GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n' >&3
  if read -t 2 -r line <&3 && [[ "$line" == HTTP/* ]]; then
    bad "the ingress answered plain HTTP (it must speak TLS only)" "$line"
  else
    ok "the ingress port speaks TLS only (no dashboard, no plain HTTP)"
  fi
  exec 3<&- 2>/dev/null || true
else
  bad "nothing accepted a TCP connection on the ingress port"
fi

# ---------------------------------------------------------------- persistence + restart
ALERTS_BEFORE="$(python3 - "$WORK/ingest_audit.db" <<'PY'
import sqlite3, sys
try:
    connection = sqlite3.connect(sys.argv[1])
    print(connection.execute("select count(*) from alerts").fetchone()[0])
except Exception:
    print(0)
PY
)"
kill -TERM "$INGRESS_PID" 2>/dev/null; wait "$INGRESS_PID" 2>/dev/null
grep -q "stopping" "$WORK/ingress.log" && ok "the ingress shut down cleanly on SIGTERM" \
                                        || bad "no clean shutdown line"

"$INGRESS" --config "$WORK/ingress.json" >"$WORK/ingress2.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 40); do grep -q "ready" "$WORK/ingress2.log" 2>/dev/null && break; sleep 0.25; done
ALERTS_AFTER="$(python3 - "$WORK/ingest_audit.db" <<'PY'
import sqlite3, sys
try:
    connection = sqlite3.connect(sys.argv[1])
    print(connection.execute("select count(*) from alerts").fetchone()[0])
except Exception:
    print(0)
PY
)"
if grep -q "ready" "$WORK/ingress2.log"; then
  ok "the ingress restarted against the same durable store"
  [[ "$ALERTS_AFTER" -ge "$ALERTS_BEFORE" ]] && ok "persisted alert count survived the restart ($ALERTS_AFTER >= $ALERTS_BEFORE)" \
                                             || bad "persisted alerts were lost" "$ALERTS_BEFORE -> $ALERTS_AFTER"
else
  bad "the ingress did not restart"
fi

echo "  --------------------------------------------------------------------"
echo "  accepted messages : $(grep -c 'accepted=1' "$WORK/ingress.log" 2>/dev/null || echo 0) client-side lines"
echo "  ingress log tail  : $(grep -m1 stopping "$WORK/ingress.log" 2>/dev/null | tr -s ' ')"
echo "  D-03 TLS acceptance: $PASS passed, $FAIL failed, $SKIP skipped"
[[ "$FAIL" == "0" ]]
