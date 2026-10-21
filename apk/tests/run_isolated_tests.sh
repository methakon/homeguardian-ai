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
    "$APK_DIR/tests/ObservationTransportPolicyTest.java"
echo "    test classes: $(find "$OUT/test" -name '*.class' | wc -l)"

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
echo
echo "==> exit code: $rc"
exit $rc
