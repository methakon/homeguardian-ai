#!/usr/bin/env bash
# verify_android_node.sh - D-02 acceptance for the Android acquisition node.
#
# Everything here is verifiable WITHOUT a phone attached:
#   1. the native node library builds for armeabi-v7a (API 27) and exports the
#      JNI entry points the app calls
#   2. the APK builds, is zipaligned and signature-verifies
#   3. the manifest declares exactly the permissions/features the node needs and
#      no foreground service (no background capture is implemented)
#   4. the dex carries no invokedynamic / invoke-custom, which the API 27 dex
#      verifier rejects (this is what forced the no-lambda Java style)
#   5. the host-side node contracts pass ([d02] tests): bounded/authorized
#      payloads, identity, consent/permission/lifecycle
#   6. the existing v21 host suite still passes (no regression)
#   7. footprints: APK size, native library size
#   8. device presence is REPORTED (physical acceptance is BLOCKED without one)
#
# Usage: bash scripts/verify_android_node.sh
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
APK_DIR="$REPO/apk"
BUILD_DIR="${HG_APK_BUILD_DIR:-$REPO/hg_apk_build}"
APK="$BUILD_DIR/homeguardian-harness.apk"
ABI="armeabi-v7a"
PASS=0; FAIL=0; WARN=0
ok()   { PASS=$((PASS+1)); printf '  %-64s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-64s FAIL  %s\n' "$1" "${2:-}"; }
warn() { WARN=$((WARN+1)); printf '  %-64s WARN  %s\n' "$1" "${2:-}"; }

export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export PATH="$JAVA_HOME/bin:$PATH"
BT="$ANDROID_HOME/build-tools/27.0.3"
[ -d "$BT" ] || BT="$ANDROID_HOME/build-tools/27.0.0"

echo "== D-02 Android acquisition node acceptance (no device required) =="

# --- 1. native library -----------------------------------------------------
SO="$BUILD_DIR/lib/libhg_harness.so"
if [ -f "$SO" ]; then
  ok "native node library built ($(stat -c%s "$SO") bytes)"
  if command -v "$ANDROID_HOME/ndk/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" >/dev/null 2>&1; then
    NM="$ANDROID_HOME/ndk/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
  else
    NM="nm"
  fi
  missing=""
  for sym in nativeSetIdentity nativeGrantConsent nativeWithdrawConsent nativeStartCamera \
             nativeStopCamera nativeStartMic nativeStopMic nativeMicStopRequested nativeStats \
             nativeStopAll nativeOnPreviewFrame; do
    "$NM" -D --defined-only "$SO" 2>/dev/null | grep -q "Java_org_homeguardian_harness_.*_${sym}" \
      || missing="$missing $sym"
  done
  [ -z "$missing" ] && ok "all app-facing JNI entry points are exported" \
                    || bad "missing JNI symbols" "$missing"
else
  bad "native library missing (run: bash apk/build_apk.sh)"
fi

# --- 2. APK + signature ----------------------------------------------------
if [ -f "$APK" ]; then
  ok "APK built ($(stat -c%s "$APK") bytes)"
  if "$BT/apksigner" verify "$APK" >/dev/null 2>&1; then
    ok "APK signature verifies"
  else
    bad "APK signature did not verify"
  fi
  "$BT/zipalign" -c 4 "$APK" >/dev/null 2>&1 && ok "APK is 4-byte aligned" || bad "zipalign check failed"
else
  bad "APK missing (run: bash apk/build_apk.sh)"
fi

# --- 3. manifest -----------------------------------------------------------
if [ -f "$APK" ]; then
  BADGING="$("$BT/aapt" dump badging "$APK" 2>/dev/null)"
  echo "$BADGING" | grep -q "package: name='org.homeguardian.harness'" \
    && ok "package id is org.homeguardian.harness" || bad "unexpected package id"
  echo "$BADGING" | grep -q "sdkVersion:'27'" && ok "minSdkVersion 27 (target device)" \
    || warn "minSdkVersion is not 27" "$(echo "$BADGING" | grep sdkVersion)"
  echo "$BADGING" | grep -q "targetSdkVersion:'27'" && ok "targetSdkVersion 27" \
    || warn "targetSdkVersion is not 27"
  echo "$BADGING" | grep -q "android.permission.CAMERA" && ok "declares CAMERA (requested at runtime)" \
    || bad "CAMERA permission not declared"
  echo "$BADGING" | grep -q "android.permission.RECORD_AUDIO" && ok "declares RECORD_AUDIO (requested at runtime)" \
    || bad "RECORD_AUDIO permission not declared"
  echo "$BADGING" | grep -q "android.permission.FOREGROUND_SERVICE" \
    && warn "declares FOREGROUND_SERVICE but no background capture is implemented" \
    || ok "no FOREGROUND_SERVICE permission (no background capture)"
  # aapt reports required="false" features as uses-feature-not-required, so both
  # spellings must be accepted (the node must stay installable on a device with
  # no camera at all).
  if echo "$BADGING" | grep -qE "uses-feature(-not-required)?: name='android.hardware.camera'"; then
    ok "declares the camera feature (installable without one)"
  else
    bad "camera uses-feature missing from the manifest"
  fi
  if echo "$BADGING" | grep -qE "uses-feature(-not-required)?: name='android.hardware.microphone'"; then
    ok "declares the microphone feature (installable without one)"
  else
    bad "microphone uses-feature missing from the manifest"
  fi
  echo "$BADGING" | grep -q "native-code: '$ABI'" && ok "packages the $ABI native library" \
    || bad "ABI not packaged" "$(echo "$BADGING" | grep native-code)"
  echo "$BADGING" | grep -q "launchable-activity: name='org.homeguardian.harness.MainActivity'" \
    && ok "launchable activity is the node UI" || bad "launchable activity missing"
fi

# --- 4. dex must be API-27 compatible (no invokedynamic) -------------------
DEX="$BUILD_DIR/dex/classes.dex"
if [ -f "$DEX" ] && [ -x "$BT/dexdump" ]; then
  if "$BT/dexdump" -d "$DEX" 2>/dev/null | grep -qiE "invoke-custom|invoke-polymorphic"; then
    bad "dex contains invoke-custom/invoke-polymorphic (API 27 verifier rejects it)"
  else
    ok "dex has no invokedynamic-style opcodes (API 27 compatible)"
  fi
else
  warn "dexdump or classes.dex unavailable; skipped the dex opcode check"
fi

# --- 5./6. host tests ------------------------------------------------------
TESTS="${HG_TESTS_PATH:-$REPO/hg_build/homeguardian_tests}"
if [ -x "$TESTS" ]; then
  out="$(HG_SOURCE_DIR="$REPO" "$TESTS" "[d02]" 2>&1 | tail -2)"
  echo "$out" | grep -q "All tests passed" && ok "D-02 node contracts pass ($(echo "$out" | head -1))" \
    || bad "D-02 node contract tests failed" "$(echo "$out" | head -1)"
  out="$(HG_SOURCE_DIR="$REPO" "$TESTS" 2>&1 | tail -2)"
  echo "$out" | grep -q "All tests passed" && ok "existing host suite still passes ($(echo "$out" | head -1))" \
    || bad "host suite regression" "$(echo "$out" | head -1)"
else
  bad "host test binary not found at $TESTS"
fi

# --- 7. footprints ---------------------------------------------------------
[ -f "$APK" ] && printf '  %-64s %s bytes (%.2f MiB)\n' "APK size" "$(stat -c%s "$APK")" \
  "$(awk "BEGIN{printf \"%.2f\", $(stat -c%s "$APK")/1048576}")"
[ -f "$SO" ] && printf '  %-64s %s bytes (%.2f MiB)\n' "native library size ($ABI)" "$(stat -c%s "$SO")" \
  "$(awk "BEGIN{printf \"%.2f\", $(stat -c%s "$SO")/1048576}")"
LIBCXX="$BUILD_DIR/gen/lib/$ABI/libc++_shared.so"
[ -f "$LIBCXX" ] && printf '  %-64s %s bytes\n' "bundled libc++_shared.so" "$(stat -c%s "$LIBCXX")"

# --- 8. device presence (physical acceptance) -----------------------------
ADB="$(command -v adb || echo "$ANDROID_HOME/platform-tools/adb")"
devices="$("$ADB" devices 2>/dev/null | tail -n +2 | grep -w device | wc -l)"
if [ "$devices" -gt 0 ]; then
  printf '  %-64s %s\n' "attached devices" "$devices"
  "$ADB" devices -l 2>/dev/null | tail -n +2 | sed 's/^/      /'
  echo "  -> a device is present: physical capture acceptance can be attempted by the"
  echo "     operator (install, grant permissions, press Start) - never automatically."
else
  printf '  %-64s %s\n' "attached devices" "0"
  echo "  -> D-02 PHYSICAL CAPTURE ACCEPTANCE: BLOCKED - NO DEVICE ATTACHED"
fi

echo "  --------------------------------------------------------------------"
echo "  D-02 software acceptance: $PASS passed, $FAIL failed, $WARN warned"
[ "$FAIL" == "0" ]
