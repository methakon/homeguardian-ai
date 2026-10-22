#!/usr/bin/env bash
# run_real_ingress_e2e.sh - drive the PRODUCTION Android serializer against the REAL C++ D-03 ingress.
#
# WHAT THIS PROVES, and what it deliberately does not
#
#   (a) wire-compatible offline vectors      SyntheticVectorEmitTest - no socket
#   (b) real localhost mTLS C++ integration   THIS SCRIPT
#   (c) physical phone / real camera or mic   NOT DONE, still NO
#
# Everything below is loopback-only. The ingress binds 127.0.0.1 inside the container using
# --network host, and the ingress itself REFUSES a non-loopback bind when replay protection is
# ephemeral, so the isolation is enforced by the binary rather than by this script's good intentions.
#
# No production credential is issued, no APK is installed, no sensor is activated, nothing is
# published to a LAN, and Hermes 3's worktree is READ, never written: the ingress binary is run out of
# the repo's own prebuilt Docker image.
#
# WHY THE CONTAINER: this host has the OpenSSL runtime but not libssl-dev, so cmake reports that
# hg_device_ingress "is NOT built" and the TLS boundary cannot be compiled here. The repo's own
# Dockerfile installs libssl-dev for exactly this reason, and homeguardian-ai:d032test already
# contains the built binary. That is the documented prerequisite, not a workaround.
#
# All material is TEST-ONLY and lives in scratch: throwaway CA, server and client certificates valid
# for two days, a test identity enrolled in a test registry, and passwords that are the literal string
# 'testonly'. Nothing here is a secret and nothing here is production.

set -uo pipefail

REPO="${HG_REPO:-/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-s2-svc-transport06}"
E2E_DIR="${HG_E2E_DIR:-$HOME/.hermes/cache/scratch/hg_e2e}"
# The container is SHARED with Hermes 3. We never remove it; a run uses its own container name so a
# failure here cannot disturb theirs. Override with HG_E2E_CONTAINER / HG_E2E_DIR.
IMAGE="${HG_E2E_IMAGE:-homeguardian-ai:d032test}"
PORT="${HG_E2E_PORT:-18443}"
CONTAINER="${HG_E2E_CONTAINER:-hg_e2e_ingress_s2}"
JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
AJ="${ANDROID_JAR:-$HOME/Android/Sdk/platforms/android-34/android.jar}"
OUT="$E2E_DIR/classes"

say() { printf '  %s\n' "$*"; }
die() { printf '  FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------------------------
# 0. Preconditions, checked rather than assumed.
# ---------------------------------------------------------------------------------------------
say "==> 0/6 preconditions"
[ -d "$REPO" ] || die "no repo at $REPO"
[ -x "$JAVA_HOME/bin/javac" ] || die "no javac at $JAVA_HOME/bin"
[ -f "$AJ" ] || die "no android.jar at $AJ"

docker image inspect "$IMAGE" >/dev/null 2>&1 || die "docker image $IMAGE not present"
docker run --rm --entrypoint test "$IMAGE" -x /src/build/hg_device_ingress \
  || die "image $IMAGE has no built /src/build/hg_device_ingress (the TLS boundary)"

# libssl-dev is NOT required on the host - say so, because its absence is otherwise confusing.
if [ ! -f /usr/include/openssl/ssl.h ]; then
  say "note: libssl-dev absent on this host, which is why the ingress runs from the image."
fi

mkdir -p "$E2E_DIR/tls" "$E2E_DIR/state" "$OUT"
D="$E2E_DIR/tls"

# NEVER `rm -rf` THIS TREE. The container bind-mounts it, and a bind mount holds the INODE: deleting the
# directory leaves a RUNNING container attached to the unlinked one, so its /state reads as empty and
# every connection is refused with `certificate verify failed` - a symptom that looks like a trust
# problem and is actually a filesystem one. Regenerating the CA is just as bad: the running ingress
# loaded its CA at startup, so a new one cannot verify a new client.
#
# So the material is generated ONCE and then REUSED, and a fresh generation happens only when there is
# nothing to reuse. Use HG_E2E_FRESH=1 (with a container of your own) to force it.
FRESH=0
if [ "${HG_E2E_FRESH:-0}" = "1" ] || [ ! -f "$D/ca.pem" ] || [ ! -f "$E2E_DIR/ingress.json" ]; then
  FRESH=1
fi

if [ "$FRESH" = "0" ]; then
  say "==> 1-4/6 reusing the existing TEST-ONLY material and registry (regenerating would break the"
  say "           running container's loaded CA). Fingerprint on disk:"
  say "           $(openssl x509 -in "$D/client.pem" -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2- | tr -d ':' | tr 'A-Z' 'a-z' | cut -c1-24)..."
fi

if [ "$FRESH" = "1" ]; then

# ---------------------------------------------------------------------------------------------
# 1. TEST-ONLY TLS material. The server needs an IP SAN for 127.0.0.1, because the transport sets
#    endpointIdentificationAlgorithm=HTTPS BEFORE the handshake and would reject a DNS-only SAN.
# ---------------------------------------------------------------------------------------------
say "==> 1/6 test-only TLS material"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/ca.key" -out "$D/ca.pem" -days 2 \
  -subj "/CN=HG TEST-ONLY CA (e2e)" >/dev/null 2>&1 || die "ca generation failed"
openssl req -newkey rsa:2048 -nodes -keyout "$D/server.key" -out "$D/server.csr" \
  -subj "/CN=127.0.0.1" >/dev/null 2>&1 || die "server csr failed"
printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\nextendedKeyUsage=serverAuth\n' > "$D/server.ext"
openssl x509 -req -in "$D/server.csr" -CA "$D/ca.pem" -CAkey "$D/ca.key" -CAcreateserial \
  -out "$D/server.pem" -days 2 -extfile "$D/server.ext" >/dev/null 2>&1 || die "server cert failed"
openssl req -newkey rsa:2048 -nodes -keyout "$D/client.key" -out "$D/client.csr" \
  -subj "/CN=hg-android-synthetic-01" >/dev/null 2>&1 || die "client csr failed"
printf 'extendedKeyUsage=clientAuth\n' > "$D/client.ext"
openssl x509 -req -in "$D/client.csr" -CA "$D/ca.pem" -CAkey "$D/ca.key" -CAcreateserial \
  -out "$D/client.pem" -days 2 -extfile "$D/client.ext" >/dev/null 2>&1 || die "client cert failed"

# A SECOND client identity. device_id is derived from the certificate CN, and the detector fixture needs
# two distinct source types on two distinct registered devices - a single identity could not legally
# present both without the hub seeing one device make contradictory claims.
openssl req -newkey rsa:2048 -nodes -keyout "$D/client2.key" -out "$D/client2.csr" \
  -subj "/CN=hg-android-presence-01" >/dev/null 2>&1 || die "client2 csr failed"
openssl x509 -req -in "$D/client2.csr" -CA "$D/ca.pem" -CAkey "$D/ca.key" -CAcreateserial \
  -out "$D/client2.pem" -days 2 -extfile "$D/client.ext" >/dev/null 2>&1 || die "client2 cert failed"

# The registry binds a device to the certificate FINGERPRINT, and the format matters:
# TlsTransport.cpp builds it as lowercase hex with NO colons. A colons-separated fingerprint
# authenticates as a different device and is refused.
FP=$(openssl x509 -in "$D/client.pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':' | tr 'A-Z' 'a-z')
FP2=$(openssl x509 -in "$D/client2.pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':' | tr 'A-Z' 'a-z')
say "client  fingerprint: ${FP:0:24}..."
say "client2 fingerprint: ${FP2:0:24}..."

# ---------------------------------------------------------------------------------------------
# 2. Test registry. Field names and bounds come from DeviceRegistry.cpp:
#    device_id <=96, display_name <=64, consent_purpose <=32 (kMaxTypeChars), consent_subject <=96.
#    A purpose of 37 characters is REJECTED and the ingress exits - fail-closed, correctly.
# ---------------------------------------------------------------------------------------------
say "==> 2/6 test registry"
cat > "$E2E_DIR/registry.json" <<JSON
{
  "devices": [
    {
      "device_id": "hg-android-synthetic-01",
      "display_name": "HG TEST-ONLY android node",
      "device_type": "android",
      "credential_id": "$FP",
      "paired": true,
      "revoked": false,
      "has_camera": true,
      "has_microphone": false,
      "consent_granted": true,
      "consent_subject": "test-harness",
      "consent_purpose": "loopback protocol verification",
      "capture_enabled": true,
      "capabilities": ["camera", "microphone"]
    },
    {
      "device_id": "hg-android-presence-01",
      "display_name": "HG TEST-ONLY presence node",
      "device_type": "android",
      "credential_id": "$FP2",
      "paired": true,
      "revoked": false,
      "has_camera": false,
      "has_microphone": false,
      "consent_granted": true,
      "consent_subject": "test-harness",
      "consent_purpose": "loopback protocol verification",
      "capture_enabled": true,
      "capabilities": ["presence"]
    }
  ]
}
JSON

# The revocation file must be a bounded ARRAY. An object {"revoked": []} is refused at runtime with
# "revocation state unusable", surfaced to the device as reason=unauthorized - which looks like an
# authorization problem and is actually a malformed file.
printf '[]\n' > "$E2E_DIR/revocation.json"

# ---------------------------------------------------------------------------------------------
# 3. Ingress config: LOOPBACK ONLY, durable replay state.
# ---------------------------------------------------------------------------------------------
say "==> 3/6 ingress config (loopback only)"
cat > "$E2E_DIR/ingress.json" <<JSON
{
  "certificate_path": "/pki/server.pem",
  "private_key_path": "/pki/server.key",
  "ca_path": "/pki/ca.pem",
  "bind_address": "127.0.0.1",
  "port": $PORT,
  "registry_path": "/state/registry.json",
  "revocation_path": "/state/revocation.json",
  "replay_state_path": "/state/replay.json",
  "audit_db_path": "/state/audit.db",
  "max_connections": 4,
  "max_workers": 2,
  "evidence_detector_ids": ${HG_E2E_EVIDENCE:-["sensor.motion", "sensor.presence"]},
  "verbose": true
}
JSON

# ---------------------------------------------------------------------------------------------
# 4. PKCS12 for the production transport. The CA truststore is built with keytool, NOT
#    `openssl pkcs12 -nokeys`: that writes a cert-only file that JAVA READS AS EMPTY, and the
#    failure surfaces as "the trustAnchors parameter must be non-empty" inside the TLS stack.
# ---------------------------------------------------------------------------------------------
say "==> 4/6 transport keystores"
openssl pkcs12 -export -in "$D/client.pem" -inkey "$D/client.key" -certfile "$D/ca.pem" \
  -out "$E2E_DIR/device.p12" -passout pass:testonly -name hgtest >/dev/null 2>&1 \
  || die "device.p12 failed"
printf 'testonly' > "$E2E_DIR/device.p12.pass"
openssl pkcs12 -export -in "$D/client2.pem" -inkey "$D/client2.key" -certfile "$D/ca.pem" \
  -out "$E2E_DIR/device2.p12" -passout pass:testonly -name hgtest2 >/dev/null 2>&1 \
  || die "device2.p12 failed"
printf 'testonly' > "$E2E_DIR/device2.p12.pass"
# Remove first: keytool refuses to overwrite an existing alias, and in FRESH mode the old keystore is
# still on disk holding the old CA under the same alias.
rm -f "$E2E_DIR/ca.p12"
"$JAVA_HOME/bin/keytool" -importcert -noprompt -alias hgtestca -file "$D/ca.pem" \
  -keystore "$E2E_DIR/ca.p12" -storetype PKCS12 -storepass testonly >/dev/null 2>&1 \
  || die "ca.p12 failed"
printf 'testonly' > "$E2E_DIR/ca.p12.pass"

fi  # end FRESH generation

# ---------------------------------------------------------------------------------------------
# 5. Start the REAL ingress.
# ---------------------------------------------------------------------------------------------
say "==> 5/6 start the real D-03 ingress on 127.0.0.1:$PORT"
# REUSE a healthy container rather than recreating it. The container is shared with another lane, and
# removing it would drop whatever watermark and audit state they rely on.
#
# BUT a FRESH generation and an existing container are INCOMPATIBLE: the running ingress loaded its CA
# and registry at startup, so newly generated material cannot be verified by it. So when FRESH=1 this
# script recreates ITS OWN container - and never the shared one, whatever FRESH is set to. Getting this
# wrong is not a loud failure: the container stays up and every connection is refused, which reads as a
# trust problem rather than a stale-process one.
SHARED_CONTAINER="hg_e2e_ingress"
if [ "$FRESH" = "1" ] && [ "$CONTAINER" != "$SHARED_CONTAINER" ]; then
  say "FRESH=1: recreating $CONTAINER so it loads the newly generated material"
  docker rm -f "$CONTAINER" >/dev/null 2>&1
elif [ "$FRESH" = "1" ]; then
  die "FRESH=1 with CONTAINER=$SHARED_CONTAINER: refusing to recreate a container this lane does not own"
fi

if docker inspect -f '{{.State.Status}}' "$CONTAINER" 2>/dev/null | grep -q running; then
  say "reusing the running container $CONTAINER (not removed: it is shared)"
  docker logs "$CONTAINER" 2>&1 | grep 'ready' | tail -1 | sed 's/^/    /' || true
else
  say "no running $CONTAINER, starting one"
  docker rm -f "$CONTAINER" >/dev/null 2>&1
  docker run -d --name "$CONTAINER" --network host \
    -v "$E2E_DIR/tls":/pki:ro -v "$E2E_DIR":/state:rw \
    --entrypoint /src/build/hg_device_ingress "$IMAGE" --config /state/ingress.json >/dev/null 2>&1 \
    || die "container did not start"
  sleep 4
  docker logs "$CONTAINER" 2>&1 | grep -q 'hg_device_ingress ready' \
    || { docker logs "$CONTAINER" 2>&1 | tail -5 | sed 's/^/    /'; die "ingress did not report ready"; }
  docker logs "$CONTAINER" 2>&1 | grep 'ready' | sed 's/^/    /'
fi

# Assert the isolation rather than claiming it.
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
  ss -ltn | grep ":$PORT " | sed 's/^/    listening: /'
  ss -ltn | grep ":$PORT " | grep -q '127.0.0.1' || die "ingress is not bound to loopback only"
fi

# ---------------------------------------------------------------------------------------------
# 6. Compile and run the E2E.
# ---------------------------------------------------------------------------------------------
say "==> 6/6 compile + run the production serializer against it"
mkdir -p "$OUT/prod" "$OUT/stubs" "$OUT/test"
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -d "$OUT/stubs" \
  $(find "$REPO/apk/tests/stubs" -name '*.java') >/dev/null 2>&1 || die "stub compile failed"
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$AJ" -d "$OUT/prod" \
  "$REPO"/apk/src/org/homeguardian/harness/*.java >/dev/null 2>&1 || die "prod compile failed"
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$OUT/prod:$AJ" -d "$OUT/test" \
  "$REPO"/apk/tests/RealIngressE2ETest.java >/dev/null 2>&1 || die "test compile failed"
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$OUT/prod:$AJ" -d "$OUT/test" \
  "$REPO"/apk/tests/SyntheticVectorEmitTest.java >/dev/null 2>&1 || die "vector compile failed"

CP="$OUT/stubs:$OUT/prod:$OUT/test:$AJ"
"$JAVA_HOME/bin/java" -Dhg.e2e.dir="$E2E_DIR" -Dhg.e2e.host=127.0.0.1 -Dhg.e2e.port="$PORT" \
  -classpath "$CP" org.homeguardian.harness.RealIngressE2ETest
RC=$?

# ---------------------------------------------------------------------------------------------
# 7. BOTH VECTOR MODES.
#    pinned = byte-reproducible offline golden, NOT live-valid (its timestamp ages out of the 24h
#             window and the hub refuses it - that refusal is asserted as E1 above).
#    live   = fresh timestamp inside the window, same shape, same synthetic provenance.
#    The digests differ between modes BY DESIGN; within a mode the pinned one must not.
# ---------------------------------------------------------------------------------------------
say ""
say "==> 7/7 emit both vector modes"
"$JAVA_HOME/bin/java" -Dhg.vector.out="$E2E_DIR/vector-pinned" -classpath "$CP" \
  org.homeguardian.harness.SyntheticVectorEmitTest | sed 's/^/    /' || RC=1
"$JAVA_HOME/bin/java" -Dhg.vector.out="$E2E_DIR/vector-live" -Dhg.vector.mode=live -classpath "$CP" \
  org.homeguardian.harness.SyntheticVectorEmitTest | sed 's/^/    /' || RC=1

say ""
say "pinned observation.frame sha256: $(sha256sum "$E2E_DIR/vector-pinned/observation.frame" 2>/dev/null | cut -c1-32)..."
say "live   observation.frame sha256: $(sha256sum "$E2E_DIR/vector-live/observation.frame" 2>/dev/null | cut -c1-32)..."
say "both manifests record mode, live_valid and the ingress timestamp windows."

# ---------------------------------------------------------------------------------------------
# 8. DETECTOR-CAPABLE FIXTURE.
#    The pinned/live vectors above carry camera_availability, which is wire-valid but casts no vote.
#    These two streams carry the types and states the C++ actually maps to `detected`, on two distinct
#    registered identities, driven by the same production serializer.
# ---------------------------------------------------------------------------------------------
say ""
say "==> 8/8 detector-capable fixture (motion + presence)"
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$OUT/prod:$AJ" -d "$OUT/test" \
  "$REPO"/apk/tests/DetectorProducerFixtureTest.java >/dev/null 2>&1 || die "detector fixture compile failed"
"$JAVA_HOME/bin/java" -Dhg.e2e.dir="$E2E_DIR" -Dhg.e2e.host=127.0.0.1 -Dhg.e2e.port="$PORT" \
  -Dhg.detector.out="$E2E_DIR/detector-fixture" -classpath "$CP" \
  org.homeguardian.harness.DetectorProducerFixtureTest | sed 's/^/    /' || RC=1

say ""
say "this run used $CONTAINER on 127.0.0.1:$PORT. It is left RUNNING for inspection:"
say "  docker rm -f $CONTAINER"

# ---------------------------------------------------------------------------------------------
# LIFECYCLE NOTE — read before restarting anything.
#
# A container must not be restarted in isolation from the tree it mounts. `rm -rf` on a bind-mounted
# directory leaves the running container attached to the UNLINKED inode: its /state then reads as
# empty and every connection fails `certificate verify failed`, which looks like a trust problem and
# is actually a filesystem one. Restarting it is not enough either - the ingress loads its CA at
# startup, so it must be restarted together with the material it was started from.
#
# So the CLEAN START for a container that reports that symptom is a fresh generation plus a fresh
# container, in that order:
#
#   HG_E2E_FRESH=1 bash scripts/run_real_ingress_e2e.sh
#
# That regenerates the TEST-ONLY material and starts a new container bound to it. The shipped
# hg_e2e_ingress (a shared container) is deliberately NOT touched by this script: it is out of this
# lane's scope, and a lifecycle change to it is an owner decision. If it is in the broken state above,
# the owner's remedy is the same recipe with HG_E2E_CONTAINER=hg_e2e_ingress after stopping it.
# ---------------------------------------------------------------------------------------------
say ""
say "clean start after a stale-inode break (see the lifecycle note in this script):"
say "  HG_E2E_FRESH=1 bash scripts/run_real_ingress_e2e.sh"

exit $RC
