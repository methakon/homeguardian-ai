#!/usr/bin/env bash
# Compile and run the off-device RouteSecurityPolicy suite against the REAL policy class.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${HG_ROUTE_TEST_OUT:-$HOME/.hermes/cache/scratch/hg-route-policy-test}"
JAVAC="${JAVAC:-$HOME/android-jdk/bin/javac}"
JAVA="${JAVA:-$HOME/android-jdk/bin/java}"
rm -rf "$OUT"; mkdir -p "$OUT"
"$JAVAC" -nowarn -d "$OUT" \
  $(find "$ROOT/scripts/bridge_ws_compat/stubs" -name '*.java') \
  "$ROOT/apk_service/src/org/homeguardian/edgeservice/RouteSecurityPolicy.java" \
  "$ROOT/scripts/bridge_ws_compat/RouteSecurityPolicyTest.java"
"$JAVA" -cp "$OUT" RouteSecurityPolicyTest
