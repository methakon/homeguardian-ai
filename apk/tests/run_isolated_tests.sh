#!/usr/bin/env bash
# run_isolated_tests.sh — run the HG-PANASONIC-SERVICE-TRANSPORT-06 lifecycle tests off-device.
#
# WHAT THIS DOES, IN ORDER
#   1. compiles apk/src against android.jar  -> proves the production Java still builds
#   2. compiles the JVM stubs (android.util.Log / Context / PowerManager)
#   3. compiles the test against the production classes
#   4. runs the test with the STUBS FIRST on the classpath, so the host's Log calls resolve
#      off-device instead of hitting android.jar's "Stub!" runtime
#
# It does NOT install anything, touch a device, open a socket, or activate a sensor. Capture,
# permissions and installed APKs are untouched by design: the seam being tested is pure Java.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APK_DIR="$ROOT/apk"
OUT="${TMPDIR:-/tmp}/hg_svc_transport06_tests"

export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export PATH="$JAVA_HOME/bin:$PATH"

PLATFORM="${HG_PLATFORM:-$ANDROID_HOME/platforms/android-34}"
AJ="$PLATFORM/android.jar"
[ -f "$AJ" ] || { echo "missing android.jar at $AJ"; exit 1; }
[ -x "$JAVA_HOME/bin/javac" ] || { echo "missing javac at $JAVA_HOME/bin"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/prod" "$OUT/stubs" "$OUT/test"

echo "==> 1/4 compile apk/src against $(basename "$PLATFORM") (production build check)"
javac --release 8 -Xlint:-options -classpath "$AJ" -d "$OUT/prod" \
    $(find "$APK_DIR/src" -name '*.java')
echo "    production classes: $(find "$OUT/prod" -name '*.class' | wc -l)"

echo "==> 2/4 compile JVM stubs"
javac --release 8 -Xlint:-options -d "$OUT/stubs" \
    $(find "$APK_DIR/tests/stubs" -name '*.java')
echo "    stubs: $(find "$OUT/stubs" -name '*.class' | wc -l)"

echo "==> 3/4 compile the isolated tests"
javac --release 8 -Xlint:-options -classpath "$OUT/prod:$AJ" -d "$OUT/test" \
    "$APK_DIR/tests/NodeTransportHostTest.java" \
    "$APK_DIR/tests/ProducerSchemaConformanceTest.java" \
    "$APK_DIR/tests/ObservationTransportPolicyTest.java" \
    "$APK_DIR/tests/CrossReceiverContractTest.java" \
    "$APK_DIR/tests/TlsHandshakeE2ETest.java"
echo "    test classes: $(find "$OUT/test" -name '*.class' | wc -l)"

echo "==> 3b/4 generate the cross-receiver fixture from the C++ receiver source"
# The fixture is DERIVED state and lives under a gitignored path, so it is regenerated here rather
# than committed. If it cannot be generated the cross-receiver test would SKIP, and a skipped check
# must never read as cleaner than the check - so a failure here is a hard failure.
if ! python3 "$ROOT/scripts/gen_cross_receiver_fixture.py" > "$OUT/fixture-gen.log" 2>&1; then
    echo "    FIXTURE GENERATION FAILED - refusing to run with a silently-skipping cross-receiver test"
    sed 's/^/      /' "$OUT/fixture-gen.log"
    exit 1
fi
grep -E 'only on the|RESULT' "$OUT/fixture-gen.log" | sed 's/^/    /'
echo

echo "==> 3c/4 guard: the suite must never assert PHYSICAL media"
# These tests run on a host with no camera and no microphone. A test that asserts a real frame, a real
# audio sample or a physical capture would be asserting something the harness cannot do here - and a
# green run would then read as physical evidence. This guard fails the suite if such a claim appears,
# so "no assertion of physical media" is enforced rather than remembered.
# Scan ASSERTIONS, not prose. A comment that DISCLAIMS physical media ("must never be labelled as
# real camera evidence") contains the same words as a claim that MAKES one, and an earlier version of
# this guard failed the suite on exactly such a comment. Strip comments first, then require the phrase
# to appear inside an assertion call.
CLAIMS='real (camera|frame|video|audio|mic)|physical (camera|capture)|actual (camera|frame|video)|live camera|real device produced|captured (a )?(real|actual) (frame|video|audio)'
for f in "$APK_DIR"/tests/*.java; do
    # drop block comments, then trailing line comments, then keep only assertion lines
    sed -e ':a' -e 's|/\*[^*]*\*/||g' -e '/\/\*/,/\*\//d' -e 's|//.*||' "$f" \
      | grep -inE 'check\(|assert|System\.out' \
      | grep -iE "$CLAIMS" >> "$OUT/media-claims.txt" || true
done
if [ -s "$OUT/media-claims.txt" ]; then
    echo "    FAIL an assertion in the tests claims physical media:"
    sed 's/^/      /' "$OUT/media-claims.txt"
    exit 1
fi
echo "    OK: no ASSERTION claims physical media (a disclaimer comment is correctly ignored)"

echo "==> 3d/4 generate TEST-ONLY TLS material for the real-socket handshake test"
# The TLS test needs a CA, a server cert with an IP SAN and a client cert. Generated per run under a
# temp dir; never committed, never a production credential. If generation fails the TLS test would
# SKIP, and a skipped check must not read as a cleaner result than the check itself - so this is a
# hard failure.
if ! HG_TLS_DIR="$(bash "$ROOT/scripts/gen_test_tls_material.sh" 2>"$OUT/tls-gen.log")"; then
    echo "    TLS MATERIAL GENERATION FAILED - refusing to run with a silently-skipping TLS test"
    sed 's/^/      /' "$OUT/tls-gen.log"
    exit 1
fi
echo "    material: $HG_TLS_DIR"

echo "==> 4/4 run (stubs first on the classpath)"
echo
# The stubs MUST come first: that is what shadows android.jar's throwing stubs off-device.
CP="$OUT/stubs:$OUT/prod:$OUT/test"
# hg.root lets the schema test read the canonical contract and the emitter source and assert they
# agree. Without it the drift guard degrades to SKIP rather than failing spuriously.
rc=0
java -Dhg.root="$ROOT" -classpath "$CP" org.homeguardian.harness.NodeTransportHostTest || rc=1
echo
java -Dhg.root="$ROOT" -classpath "$CP" org.homeguardian.harness.ProducerSchemaConformanceTest || rc=1
java -Dhg.root="$ROOT" -classpath "$CP" org.homeguardian.harness.ObservationTransportPolicyTest || rc=1
java -Dhg.root="$ROOT" -classpath "$CP" org.homeguardian.harness.CrossReceiverContractTest || rc=1
java -Dhg.root="$ROOT" -Dhg.tls.dir="$HG_TLS_DIR" -classpath "$CP" org.homeguardian.harness.TlsHandshakeE2ETest || rc=1
echo
echo "==> exit code: $rc"
exit $rc
