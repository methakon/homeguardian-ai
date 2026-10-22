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
IMAGE="${HG_E2E_IMAGE:-homeguardian-ai:d032test}"
PORT="${HG_E2E_PORT:-18443}"
CONTAINER="hg_e2e_ingress"
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

rm -rf "$E2E_DIR"
mkdir -p "$E2E_DIR/tls" "$E2E_DIR/state" "$OUT"
D="$E2E_DIR/tls"

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

# The registry binds a device to the certificate FINGERPRINT, and the format matters:
# TlsTransport.cpp builds it as lowercase hex with NO colons. A colons-separated fingerprint
# authenticates as a different device and is refused.
FP=$(openssl x509 -in "$D/client.pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':' | tr 'A-Z' 'a-z')
say "client fingerprint: ${FP:0:24}..."

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
"$JAVA_HOME/bin/keytool" -importcert -noprompt -alias hgtestca -file "$D/ca.pem" \
  -keystore "$E2E_DIR/ca.p12" -storetype PKCS12 -storepass testonly >/dev/null 2>&1 \
  || die "ca.p12 failed"
printf 'testonly' > "$E2E_DIR/ca.p12.pass"

# ---------------------------------------------------------------------------------------------
# 5. Start the REAL ingress.
# ---------------------------------------------------------------------------------------------
say "==> 5/6 start the real D-03 ingress on 127.0.0.1:$PORT"
docker rm -f "$CONTAINER" >/dev/null 2>&1
docker run -d --name "$CONTAINER" --network host \
  -v "$E2E_DIR/tls":/pki:ro -v "$E2E_DIR":/state:rw \
  --entrypoint /src/build/hg_device_ingress "$IMAGE" --config /state/ingress.json >/dev/null 2>&1 \
  || die "container did not start"
sleep 4
docker logs "$CONTAINER" 2>&1 | grep -q 'hg_device_ingress ready' \
  || { docker logs "$CONTAINER" 2>&1 | tail -5 | sed 's/^/    /'; die "ingress did not report ready"; }
docker logs "$CONTAINER" 2>&1 | grep 'ready' | sed 's/^/    /'

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

CP="$OUT/stubs:$OUT/prod:$OUT/test:$AJ"
"$JAVA_HOME/bin/java" -Dhg.e2e.dir="$E2E_DIR" -Dhg.e2e.host=127.0.0.1 -Dhg.e2e.port="$PORT" \
  -classpath "$CP" org.homeguardian.harness.RealIngressE2ETest
RC=$?

say ""
say "the container is LEFT RUNNING for inspection. Stop it with:"
say "  docker rm -f $CONTAINER"
exit $RC
