#!/usr/bin/env bash
# H-03 follow-up: acceptance for the SERVICE-SPECIFIC ingress health probe.
#
# WHY: the ingress container inherited the hub's HEALTHCHECK (HTTP /api/health on 8080) and so
# reported "unhealthy" while it was serving mutual TLS correctly on 8443. The replacement probe must
# be a real TLS endpoint-identity check - not a TCP port check, which passes for ANY process holding
# the port - and it must FAIL when the endpoint is not our ingress. This script proves both
# directions against real sockets, a real CA and the real binaries.
#
# WHAT IT PROVES:
#   1. the probe health-passes against a real, correctly-provisioned ingress (exit 0);
#   2. ... and reports the mutual-TLS demand separately from the identity verification;
#   3. a WRONG expected name fails (identity verification is real, not decorative);
#   4. an UNRELATED process holding the port fails (it is not a port-open check);
#   5. a plaintext listener fails;
#   6. a missing / unspecified CA fails (chain verification cannot be skipped);
#   7. the probe authenticates NO device and leaves NO trace: the replay state file, the registry
#      and the ingress log are unchanged by a health check.
#
# Usage: bash scripts/verify_ingress_healthcheck.sh [workdir]
# Needs the TLS binaries, so it runs where they are built (the container image).
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
INGRESS="${HG_INGRESS_PATH:-$REPO/hg_build/hg_device_ingress}"
PROBE="${HG_PROBE_PATH:-$REPO/hg_build/hg_ingress_probe}"
PROVISION="$REPO/tools/provision_ca.sh"
WORK="${1:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-66s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-66s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-66s SKIP  %s\n' "$1" "${2:-}"; }

INGRESS_PID=""; PLAIN_PID=""
cleanup() {
  [[ -n "$PLAIN_PID" ]] && kill -TERM "$PLAIN_PID" 2>/dev/null
  [[ -n "$INGRESS_PID" ]] && kill -TERM "$INGRESS_PID" 2>/dev/null
  wait "$INGRESS_PID" 2>/dev/null; wait "$PLAIN_PID" 2>/dev/null
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

echo "== ingress service-specific health probe =="

if [[ ! -x "$INGRESS" || ! -x "$PROBE" ]]; then
  skip "TLS binaries present" "build with libssl-dev (the container image does)"
  echo "  --------------------------------------------------------------------"
  echo "  ingress health-probe acceptance: $PASS passed, $FAIL failed, $SKIP skipped"
  exit 0
fi

# Free port: ask the kernel, so this cannot collide with a real ingress or another test run.
PORT="$(python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()
PY
)"

mkdir -p "$WORK/certs"
CERTS="$WORK/certs"
CA_DIR="$WORK/ca-offline"            # signing material: never mounted into a container
DEVICE_DIR="$WORK/device-keys"
umask 077

bash "$PROVISION" init --ca-dir "$CA_DIR" --runtime-dir "$CERTS" --device-dir "$DEVICE_DIR" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
  && ok "private CA + server certificate provisioned (SAN DNS:localhost, IP:127.0.0.1)" \
  || bad "private CA + server certificate provisioned"

printf '[]\n' > "$WORK/devices.json"
printf '[]\n' > "$WORK/revocations.json"

cat > "$WORK/ingress.json" <<JSON
{
  "certificate_path": "$CERTS/server.crt",
  "private_key_path": "$CERTS/server.key",
  "ca_path": "$CERTS/ca/ca.crt",
  "bind_address": "127.0.0.1",
  "port": $PORT,
  "registry_path": "$WORK/devices.json",
  "revocation_path": "$WORK/revocations.json",
  "audit_db_path": "$WORK/audit.db",
  "replay_state_path": "$WORK/device_replay_state.json",
  "max_connections": 4,
  "max_workers": 1,
  "verbose": false
}
JSON

[[ -r "$CERTS/ca/ca.crt" ]] && ok "runtime CA certificate present at the path the probe consumes" \
                             || bad "runtime CA certificate present at the path the probe consumes"

# ------------------------------------------------------------------ the real ingress
"$INGRESS" --config "$WORK/ingress.json" > "$WORK/ingress.log" 2>&1 &
INGRESS_PID=$!
for _ in $(seq 1 50); do
  grep -q 'hg_device_ingress ready' "$WORK/ingress.log" 2>/dev/null && break
  sleep 0.2
done
if grep -q 'hg_device_ingress ready' "$WORK/ingress.log"; then
  ok "ingress process reports readiness on port $PORT"
else
  bad "ingress process reports readiness on port $PORT" "$(tail -2 "$WORK/ingress.log" | tr '\n' ' ')"
fi

REPLAY_BEFORE="$(cat "$WORK/device_replay_state.json" 2>/dev/null | md5sum | cut -d' ' -f1)"
REGISTRY_BEFORE="$(md5sum < "$WORK/devices.json" | cut -d' ' -f1)"

# ---- 1/2. correct name -> healthy, mutual TLS reported separately
OUT="$("$PROBE" --ca "$CERTS/ca/ca.crt" --host 127.0.0.1 --port "$PORT" --verify-name localhost 2>&1)"; RC=$?
echo "      $OUT"
if [[ $RC -eq 0 ]]; then ok "probe HEALTHY against the real ingress (exit 0)"; else bad "probe HEALTHY against the real ingress" "rc=$RC"; fi
case "$OUT" in
  *"server_cert_verified=yes"*) ok "report states the server certificate was verified" ;;
  *) bad "report states the server certificate was verified" "$OUT" ;;
esac
case "$OUT" in
  *"device_auth=NOT_PERFORMED"*) ok "report states device authentication was NOT performed" ;;
  *) bad "report states device authentication was NOT performed" "$OUT" ;;
esac
case "$OUT" in
  *"mutual_tls_demanded=yes"*) ok "report states mutual TLS was demanded (reported separately)" ;;
  *) bad "report states mutual TLS was demanded" "$OUT" ;;
esac

# ---- 2b. THE WRAPPER the container actually runs, with only a config present.
# The live deployment caught a wrapper defect the probe-only checks could not: the wrapper looked for
# ${CERTS}/ca.crt while the layout keeps ${CERTS}/ca/ca.crt, so the container stayed unhealthy. This
# check runs the wrapper exactly as the container does - no --ca argument anywhere.
OUT="$(HG_INGRESS_PROBE="$PROBE" HG_INGRESS_CONFIG="$WORK/ingress.json" HG_INGRESS_CERTS="$CERTS" \
       HG_INGRESS_HEALTH_PORT="$PORT" \
       bash "$REPO/deploy/docker/ingress-healthcheck.sh" 2>&1)"; RC=$?
echo "      $OUT"
if [[ $RC -eq 0 ]]; then ok "wrapper HEALTHY with only a config (CA discovered via ca_path)"; else bad "wrapper HEALTHY with only a config" "rc=$RC: $OUT"; fi

# ---- 2b-ii. no port in the environment either: the config must supply it
OUT="$(HG_INGRESS_PROBE="$PROBE" HG_INGRESS_CONFIG="$WORK/ingress.json" HG_INGRESS_CERTS="$CERTS" \
       bash "$REPO/deploy/docker/ingress-healthcheck.sh" 2>&1)"; RC=$?
if [[ $RC -eq 0 ]]; then ok "wrapper HEALTHY with CA and port both taken from the config"; else bad "wrapper HEALTHY with CA and port from the config" "rc=$RC: $OUT"; fi

# ---- 2c. and it fails, loudly, when the config's CA is not there
printf '{"ca_path": "%s"}\n' "$WORK/absent-ca.crt" > "$WORK/bad-ca.json"
HG_INGRESS_PROBE="$PROBE" HG_INGRESS_CONFIG="$WORK/bad-ca.json" HG_INGRESS_CERTS="$WORK/nothing" \
  bash "$REPO/deploy/docker/ingress-healthcheck.sh" >/dev/null 2>&1; RC=$?
if [[ $RC -ne 0 ]]; then ok "wrapper FAILS when the configured CA is missing (exit $RC)"; else bad "wrapper FAILS when the configured CA is missing" "rc=0"; fi

# ---- 3. wrong expected name -> must FAIL (identity verification is real)
OUT="$("$PROBE" --ca "$CERTS/ca/ca.crt" --host 127.0.0.1 --port "$PORT" --verify-name not-our-endpoint.example 2>&1)"; RC=$?
if [[ $RC -ne 0 ]]; then ok "probe FAILS for a name the certificate does not cover (exit $RC)"; else bad "probe FAILS for an uncovered name" "rc=0: $OUT"; fi

# ---- 4. another process holding the port -> must FAIL (it is not a port check)
if command -v python3 >/dev/null 2>&1; then
  PLAIN_PORT="$(python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()
PY
)"
  ( cd "$WORK" && exec python3 -c "
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $PLAIN_PORT)); s.listen(4)
while True:
    try: c, _ = s.accept()
    except OSError: break
    c.close()
" ) >/dev/null 2>&1 &
  PLAIN_PID=$!
  sleep 1
  OUT="$("$PROBE" --ca "$CERTS/ca/ca.crt" --host 127.0.0.1 --port "$PLAIN_PORT" --verify-name localhost 2>&1)"; RC=$?
  if [[ $RC -ne 0 ]]; then ok "probe FAILS against a NON-TLS process holding a port (exit $RC)"; else bad "probe FAILS against a non-TLS listener" "rc=0: $OUT"; fi
  kill -TERM "$PLAIN_PID" 2>/dev/null; PLAIN_PID=""
else
  skip "probe FAILS against a non-TLS listener" "python3 unavailable"
fi

# ---- 5/6. missing CA, and no CA at all -> must FAIL (verification cannot be skipped)
OUT="$("$PROBE" --ca "$WORK/nonexistent-ca.crt" --host 127.0.0.1 --port "$PORT" --verify-name localhost 2>&1)"; RC=$?
if [[ $RC -ne 0 ]]; then ok "probe FAILS without a loadable CA (exit $RC)"; else bad "probe FAILS without a loadable CA" "rc=0: $OUT"; fi
"$PROBE" --host 127.0.0.1 --port "$PORT" --verify-name localhost >/dev/null 2>&1; RC=$?
if [[ $RC -eq 2 ]]; then ok "probe REFUSES to run with no CA configured (usage error)"; else bad "probe REFUSES to run with no CA configured" "rc=$RC"; fi

# ---- 7. no side effects: a health check is not a device connection
sleep 0.5
REPLAY_AFTER="$(cat "$WORK/device_replay_state.json" 2>/dev/null | md5sum | cut -d' ' -f1)"
REGISTRY_AFTER="$(md5sum < "$WORK/devices.json" | cut -d' ' -f1)"
[[ "$REPLAY_BEFORE" == "$REPLAY_AFTER" ]] && ok "replay state UNCHANGED by the health probe" \
                                          || bad "replay state unchanged by the health probe"
[[ "$REGISTRY_BEFORE" == "$REGISTRY_AFTER" ]] && ok "device registry UNCHANGED by the health probe" \
                                             || bad "device registry unchanged by the health probe"
if grep -qiE 'accepted=[1-9]|observation accepted' "$WORK/ingress.log"; then
  bad "no observation was accepted during a health check"
else
  ok "no observation was accepted during a health check"
fi

# ---- and the probe must not have needed a device key: none is mounted anywhere here
[[ -z "$(find "$CERTS" -name '*.p12' -o -name '*device*.key' 2>/dev/null)" ]] \
  && ok "no device key material present in the probe's credential view" \
  || bad "no device key material present in the probe's credential view"

echo "  --------------------------------------------------------------------"
echo "  ingress health-probe acceptance: $PASS passed, $FAIL failed, $SKIP skipped"
[[ $FAIL -eq 0 ]] || exit 1
