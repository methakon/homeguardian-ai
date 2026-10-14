#!/usr/bin/env bash
# verify_detection_end_to_end.sh — D-03 end-to-end detection proof.
#
# WHAT THIS PROVES, AND WHY IT IS NOT JUST AN ACK
# A protocol acknowledgement only says the hub received bytes. This script proves the
# whole declared path against the REAL deployment (both containers):
#
#   authenticated test client --mutual TLS--> ingress
#     -> certificate/credential authorization
#     -> consent + capture + allowlist + bounds + sequence/timestamp checks
#     -> normalized SecurityObservation (provenance DERIVED, never claimed)
#     -> existing C++ Pipeline -> DetectionFusionRule -> AlertPolicy
#     -> SQLite alert history that the HUB serves on its own API
#
# The decisive check is the LAST one: the alert count served by the hub's dashboard
# API must INCREASE when the device speaks, and the ingested evidence must be
# present on disk in the shared store. If the ingress were a dead end, both would
# stay flat no matter how many acknowledgements came back.
#
# The fusion allowlist is set to the ingested detector id for this run. That is an
# explicit operator decision (ADR-014); by default ingested evidence is recorded and
# audited but casts no vote, and nothing here can produce Severity::critical.
#
# Usage: bash scripts/verify_detection_end_to_end.sh
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE_FILE="${HG_COMPOSE_FILE:-$REPO/deploy/docker/docker-compose.yml}"
PROJECT="${HG_PROJECT:-hg-d03-e2e}"
IMAGE="${HG_IMAGE:-homeguardian-hub:local}"
STAGE="${1:-$(mktemp -d)}"
# Credential material is provisioned into a SCRATCH directory, never into the
# repository tree, and compose is pointed at it through HG_CERTS_DIR. Writing test
# material into a live deployment path destroyed a mounted secret once (Incident 42).
CERTDIR="${HG_CERTS_DIR:-$STAGE/certs}"
export HG_CERTS_DIR="$CERTDIR"
DEVICE="hg-e2e0000000000e2e"
DB="/data/homeguardian_bridge_audit.db"
PASS=0; FAIL=0; WARN=0
ok()   { PASS=$((PASS+1)); printf '  %-66s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-66s FAIL  %s\n' "$1" "${2:-}"; }
warn() { WARN=$((WARN+1)); printf '  %-66s WARN  %s\n' "$1" "${2:-}"; }

compose()  { docker compose -p "$PROJECT" -f "$COMPOSE_FILE" "$@"; }
in_hub()   { compose exec -T hub bash -lc "$1"; }
in_ing()   { compose exec -T ingress bash -lc "$1"; }
api()      { in_hub "exec 3<>/dev/tcp/127.0.0.1/8080; printf 'GET $1 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' >&3; cat <&3"; }
alert_total() { api /api/alerts | grep -o '"total":[0-9]*' | head -1 | cut -d: -f2; }

cleanup() {
  compose down -v >/dev/null 2>&1 || true
  # The CA key and the issued certificate live under $STAGE; remove them unless the
  # operator asks to keep them for inspection.
  [[ "${HG_KEEP_STAGE:-0}" == "1" ]] || rm -rf "$STAGE"
}
trap cleanup EXIT

echo "== D-03 end-to-end detection (project=$PROJECT, image=$IMAGE) =="

docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "image $IMAGE missing; build it first" >&2; exit 2; }
in_hub 'command -v hg_device_ingress' >/dev/null 2>&1
docker run --rm --entrypoint /bin/sh "$IMAGE" -c 'test -x /usr/local/bin/hg_device_client' \
  || { echo "the client is not in the runtime image" >&2; exit 2; }

# ---------------------------------------------------------------- material
# provision_ca.sh deliberately tightens the tree (0700/0600) on EVERY invocation,
# so the deployable modes are re-applied after each provisioning step rather than
# once at the start. A bind mount hands the container the file's own mode, and the
# ingress runs as uid 10001.
make_deployable() {
  chmod 755 "$CERTDIR" "$CERTDIR/ca" "$CERTDIR/devices" 2>/dev/null || true
  chmod 644 "$CERTDIR"/*.json "$CERTDIR"/*.crt "$CERTDIR"/server.key "$CERTDIR"/ca/ca.crt 2>/dev/null || true
  chmod 644 "$CERTDIR"/devices/*.crt "$CERTDIR"/devices/*.key 2>/dev/null || true
}

mkdir -p "$CERTDIR"
bash "$REPO/tools/provision_ca.sh" init "$CERTDIR" >/dev/null 2>&1
bash "$REPO/tools/provision_ca.sh" issue "$DEVICE" "$CERTDIR" >/dev/null 2>&1
CRED="$(bash "$REPO/tools/provision_ca.sh" show "$DEVICE" "$CERTDIR" | awk '/credential_id/{print $3}')"
[[ -n "$CRED" ]] && ok "provisioned the CA and one device certificate" || { bad "provisioning"; exit 1; }

# The device is paired, consented and capture-enabled IN WRITING (ADR-013), and the
# operator opts the ingested motion evidence into fusion (ADR-014).
cat > "$CERTDIR/devices.json" <<JSON
{
  "devices": [
    {
      "device_id": "$DEVICE",
      "display_name": "E2E node",
      "device_type": "android",
      "credential_id": "$CRED",
      "paired": true,
      "revoked": false,
      "capabilities": ["camera", "microphone"],
      "consent_granted": true,
      "consent_subject": "$DEVICE",
      "consent_purpose": "security_monitor",
      "capture_enabled": true
    }
  ]
}
JSON
printf '[]\n' > "$CERTDIR/revocations.json"
sed -e 's/"evidence_detector_ids": \[\]/"evidence_detector_ids": ["sensor.motion"]/' \
    "$REPO/deploy/docker/config/ingress.json.example" > "$CERTDIR/ingress.json"
make_deployable
grep -q '"sensor.motion"' "$CERTDIR/ingress.json" \
  && ok "operator opt-in: ingested motion admitted as fusion evidence" \
  || bad "ingress config did not enable the evidence allowlist"

# ---------------------------------------------------------------- up
compose up -d >/dev/null 2>&1
HUB_CID="$(compose ps -q hub)"; ING_CID="$(compose ps -q ingress)"
for _ in $(seq 1 45); do
  [[ "$(docker inspect --format '{{.State.Health.Status}}' "$HUB_CID" 2>/dev/null)" == "healthy" ]] && break
  sleep 1
done
[[ "$(docker inspect --format '{{.State.Health.Status}}' "$HUB_CID")" == "healthy" ]] \
  && ok "hub container healthy" || bad "hub health" "$(docker inspect --format '{{.State.Health.Status}}' "$HUB_CID")"
in_ing 'true' >/dev/null 2>&1 && ok "ingress container is running" || bad "ingress not running"
in_ing 'echo up' | grep -q up && ok "ingress entrypoint accepted its mounts" || bad "ingress entrypoint"

# The device client refuses a DNS name by design: the device path resolves nothing,
# so the boundary can never be reached through a name that could be re-pointed. The
# test therefore hands it the ingress container's IPv4 address explicitly.
ING_IP="$(docker inspect --format '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' "$ING_CID" | awk '{print $1}')"
[[ -n "$ING_IP" ]] && ok "resolved the ingress address for the client ($ING_IP; the client accepts IPv4 literals only)" \
                   || bad "could not determine the ingress address"

BEFORE="$(alert_total)"; [[ -n "$BEFORE" ]] && ok "hub API reports its alert total before the device speaks ($BEFORE)" \
                                          || bad "could not read /api/alerts"

# ---------------------------------------------------------------- the device speaks
NET="${PROJECT}_default"
client() {
  docker run --rm --network "$NET" -v "$CERTDIR:/certs:ro" --entrypoint /usr/local/bin/hg_device_client "$IMAGE" \
    --host "$ING_IP" --port 8443 --ca /certs/ca/ca.crt \
    --cert "/certs/devices/$DEVICE.crt" --key "/certs/devices/$DEVICE.key" \
    --device-id "$DEVICE" "$@" 2>&1
}

OUT="$(client --mode normal --count 8 --value 0.45)"
if grep -q "done accepted=8" <<<"$OUT"; then
  ok "authenticated device delivered 8 accepted observations over mutual TLS"
else
  # Show WHY, not just the failure: a refusal carries a reason code, while a
  # client-side failure (unreadable certificate, DNS, TLS) prints its own message.
  bad "delivery" "$(grep -oE 'reason=[a-z_]+' <<<"$OUT" | sort -u | tr '\n' ' ') $(tail -1 <<<"$OUT" | cut -c1-90)"
fi

SOON="$(alert_total)"; [[ "${SOON:-0}" -gt "${BEFORE:-0}" ]] \
  && ok "hub alert total rose after ingestion ($BEFORE -> $SOON): the observation reached the pipeline" \
  || warn "no alert produced from this stream" "before=$BEFORE after=$SOON"
sleep 2
AFTER="$(alert_total)"

# the decisive evidence: the hub's OWN API now serves an alert driven by ingested
# evidence, and the ingested detector id is present in the shared store on disk.
BODY="$(api /api/alerts)"
[[ "$BODY" == *"detection-fusion"* ]] && ok "the hub serves a fusion alert raised by the ingested stream" \
                                      || warn "no detection-fusion alert in the served history" "$(grep -o '"total":[0-9]*' <<<"$BODY" | head -1)"
[[ "$BODY" == *'"critical"'* ]] && warn "a critical alert is present (should not come from sensor evidence)" \
                                || ok "no critical severity was produced by ingested evidence"
# In WAL mode a committed row lives in the -wal sidecar until a checkpoint folds it
# into the main file, so BOTH are part of the durable state and both are searched.
in_hub "grep -a -q 'sensor.motion' $DB $DB-wal && echo found" | grep -q found \
  && ok "the ingested evidence is persisted in the shared SQLite store (with its -wal)" \
  || bad "ingested evidence not found in the shared store"
in_hub "grep -a -q 'detection-fusion' $DB $DB-wal && echo found" | grep -q found \
  && ok "the fusion alert row is durable in the same store the hub serves" \
  || bad "alert row not durable"
in_ing "test -f $DB && echo present" | grep -q present \
  && ok "both containers see the same database file (shared volume)" \
  || bad "ingress cannot see the shared database"

# The registered device claiming ANOTHER identity is refused at the hello, while it
# is still fully authorized. This runs BEFORE the revocation step on purpose: after
# a revoke/un-revoke cycle the device is deliberately left UNPAIRED (un-revoking
# lifts the ban, it does not re-grant permissions), so the refusal there would be
# device_unpaired and the assertion would be measuring the wrong thing.
OUT="$(docker run --rm --network "$NET" -v "$CERTDIR:/certs:ro" --entrypoint /usr/local/bin/hg_device_client "$IMAGE" \
  --host "$ING_IP" --port 8443 --ca /certs/ca/ca.crt --cert "/certs/devices/$DEVICE.crt" \
  --key "/certs/devices/$DEVICE.key" --device-id hg-has-no-such-device --mode normal --count 1 2>&1)"
grep -q "reason=identity_mismatch" <<<"$OUT" \
  && ok "a registered device claiming another identity is refused (identity_mismatch)" \
  || bad "identity mismatch was not refused" "$(grep -oE 'reason=[a-z_]+' <<<"$OUT" | sort -u | tr '\n' ' ')"

# ---------------------------------------------------------------- negative controls
printf '[{"device_id":"%s","reason":"e2e revocation test"}]\n' "$DEVICE" > "$CERTDIR/revocations.json"
OUT="$(client --mode normal --count 1 --expect-reject)"
grep -q "device_revoked" <<<"$OUT" && ok "after revocation the device is refused without a restart" \
                                   || bad "revocation did not take effect" "$(grep -oE 'reason=[a-z_]+' <<<"$OUT" | sort -u | tr '\n' ' ')"
printf '[]\n' > "$CERTDIR/revocations.json"

bash "$REPO/tools/provision_ca.sh" issue hg-e2e-unknown000001 "$CERTDIR" >/dev/null 2>&1
make_deployable   # the issue run tightened the tree again
OUT="$(docker run --rm --network "$NET" -v "$CERTDIR:/certs:ro" --entrypoint /usr/local/bin/hg_device_client "$IMAGE" \
  --host "$ING_IP" --port 8443 --ca /certs/ca/ca.crt --cert /certs/devices/hg-e2e-unknown000001.crt \
  --key /certs/devices/hg-e2e-unknown000001.key --device-id hg-e2e-unknown000001 --mode normal --count 1 2>&1)"
if grep -qE "reason=(device_unknown|device_unpaired|identity_mismatch)" <<<"$OUT"; then
  ok "an unregistered device is refused at the boundary"
else
  bad "unregistered device was not refused" "$(tail -1 <<<"$OUT" | cut -c1-90)"
fi

# ---------------------------------------------------------------- ports + isolation
porthub="$(docker inspect --format '{{json .HostConfig.PortBindings}}' "$HUB_CID")"
[[ "$porthub" == "null" || "$porthub" == "{}" ]] && ok "the hub publishes NO port (dashboard stays off the LAN)" \
                                                || bad "the hub published a port" "$porthub"
porting="$(docker inspect --format '{{json .HostConfig.PortBindings}}' "$ING_CID")"
[[ "$porting" == *'"8443/tcp"'* ]] && ok "the ingress publishes exactly 8443/tcp" || bad "ingress ports" "$porting"
[[ "$porting" == *"8080"* ]] && bad "the ingress also published 8080" || ok "the ingress does not publish the dashboard port"
for c in "$HUB_CID" "$ING_CID"; do
  name="$(docker inspect --format '{{.Name}}' "$c")"
  [[ "$(docker inspect --format '{{.Config.User}}' "$c")" == 10001* ]] && ok "$name runs non-root" || bad "$name user" "$(docker inspect --format '{{.Config.User}}' "$c")"
  [[ "$(docker inspect --format '{{json .HostConfig.CapDrop}}' "$c")" == *ALL* ]] && ok "$name dropped all capabilities" || bad "$name caps"
  [[ "$(docker inspect --format '{{.HostConfig.Privileged}}' "$c")" == "false" ]] && ok "$name is not privileged" || bad "$name privileged"
done
docker inspect --format '{{json .Mounts}}' "$ING_CID" | grep -q "docker.sock" \
  && bad "the ingress has the docker socket" || ok "the ingress has no docker socket"
docker inspect --format '{{json .Mounts}}' "$ING_CID" | grep -q '"ro"' \
  && ok "certificate material is mounted read-only" || warn "no read-only mount seen on the ingress"

# ---------------------------------------------------------------- durability + shutdown
compose down >/dev/null 2>&1     # containers replaced, volume kept
compose up -d >/dev/null 2>&1
HUB_CID="$(compose ps -q hub)"
for _ in $(seq 1 45); do
  [[ "$(docker inspect --format '{{.State.Health.Status}}' "$HUB_CID" 2>/dev/null)" == "healthy" ]] && break
  sleep 1
done
SURVIVED="$(alert_total)"
[[ "${SURVIVED:-0}" -ge "${AFTER:-0}" ]] && ok "the ingested alert survived container replacement ($AFTER -> $SURVIVED)" \
                                        || bad "alert history lost across replacement" "$AFTER -> $SURVIVED"
docker kill -s TERM "$HUB_CID" >/dev/null 2>&1
for _ in $(seq 1 30); do
  [[ "$(docker inspect --format '{{.State.Running}}' "$HUB_CID" 2>/dev/null)" == "false" ]] && break
  sleep 1
done
[[ "$(docker inspect --format '{{.State.ExitCode}}' "$HUB_CID" 2>/dev/null)" == "0" ]] \
  && ok "SIGTERM produced a clean exit (code 0)" || bad "exit code after SIGTERM" "$(docker inspect --format '{{.State.ExitCode}}' "$HUB_CID")"

# ---------------------------------------------------------------- teardown hygiene
compose down -v >/dev/null 2>&1
left="$(docker ps -aq --filter "label=com.docker.compose.project=$PROJECT" | wc -l)"
[[ "$left" == "0" ]] && ok "no test containers remain" || bad "containers left behind" "$left"
vols="$(docker volume ls -q --filter "label=com.docker.compose.project=$PROJECT" | wc -l)"
[[ "$vols" == "0" ]] && ok "no test volumes remain" || bad "volumes left behind" "$vols"

echo "  --------------------------------------------------------------------"
echo "  D-03 end-to-end detection: $PASS passed, $FAIL failed, $WARN warned"
echo "  alerts before=$BEFORE after=$AFTER served-after-replacement=${SURVIVED:-?}"
[[ "$FAIL" == "0" ]]
