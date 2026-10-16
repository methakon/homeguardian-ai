#!/usr/bin/env bash
# build_service_apk.sh — build and debug-sign the HomeGuardian edge-service APK.
#
# This app is a MANAGED Android service around the genuine C++ engine and MCP
# server. It builds a JNI shared library (libhg_edge_service.so) that links the
# REAL sources of the hg_mcp_server target, plus a small Java layer that starts,
# stops, supervises and health-checks it.
#
# Why a JNI library instead of shipping the hg_mcp_server executable:
#   Android 10+ (API 29+) blocks executing files from app-writable directories, so
#   "push a binary and exec it" is not a supported app architecture. A shared
#   library loaded with System.loadLibrary is the supported way to run native code
#   in an app, and it needs no ADB privileges and no /data/local/tmp.
#
# The source list is DERIVED from CMakeLists.txt (the add_executable(hg_mcp_server)
# block) minus src/main_mcp_server.cpp — which owns main() and cannot live in a
# library — so it cannot silently drift from the binary the HG-EDGE-01 work
# validated. If that target gains a file, this build picks it up.
#
# Safety: this script only BUILDS and SIGNS. It never installs, never grants a
# permission and never starts anything.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK_DIR="$ROOT/apk_service"
BUILD_DIR="$ROOT/hg_service_apk_build"

export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/android-ndk-r26d}"
export PATH="$JAVA_HOME/bin:$PATH"

NDK="$ANDROID_NDK_HOME"
SDK="$ANDROID_HOME"
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34"
# Two distinct roots on purpose: TC_ROOT for sysroot paths (libc++_shared.so),
# TC for the compiler binaries. Conflating them made the libc++ copy look missing.
TC_ROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
TC="$TC_ROOT/bin"

# ---- The five SDK fields, kept as five SEPARATE values on purpose -----------
# They are routinely conflated in this project's docs. See
# docs/EDGE_DEVICE_MI_A3.md section 8 ("Android SDK field reconciliation").
COMPILE_SDK_LEVEL=34            # android.jar used to compile (newest available here)
MIN_SDK_LEVEL=30                # oldest device this app will install on
TARGET_SDK_LEVEL=30             # behaviour contract; matches the only device we target
NDK_API_LEVEL=30                # platform level the NDK compiles against
ABI="arm64-v8a"                 # the Mi A3's 64-bit ABI
# The device's own API level is NOT a build input; it is read from the phone:
#   adb shell getprop ro.build.version.sdk   -> 30

KEYSTORE="$HOME/.homeguardian-apk-keystore/harness-debug.keystore"
KS_ALIAS="harnessdebug"
KS_PASS="homeguardian-harness"   # local debug key only; never committed

echo "==> SDK field reconciliation"
{
  printf '    %-22s %s\n' "compileSdk / android.jar" "$COMPILE_SDK_LEVEL"
  printf '    %-22s %s\n' "minSdkVersion" "$MIN_SDK_LEVEL"
  printf '    %-22s %s\n' "targetSdkVersion" "$TARGET_SDK_LEVEL"
  printf '    %-22s %s\n' "NDK API level" "$NDK_API_LEVEL"
  printf '    %-22s %s\n' "NDK ABI" "$ABI"
}

echo "==> Toolchain check"
CXX="$TC/aarch64-linux-android${NDK_API_LEVEL}-clang++"
CC="$TC/aarch64-linux-android${NDK_API_LEVEL}-clang"
[ -x "$CXX" ] || { echo "missing NDK compiler $CXX"; exit 1; }
[ -f "$PLATFORM/android.jar" ] || { echo "missing android.jar at $PLATFORM"; exit 1; }
[ -x "$BT/aapt" ] || { echo "missing aapt"; exit 1; }
[ -x "$BT/d8" ] || { echo "missing d8"; exit 1; }
[ -x "$BT/apksigner" ] || { echo "missing apksigner"; exit 1; }
"$JAVA_HOME/bin/java" -version 2>&1 | head -1

DEPS="${HG_FETCHCONTENT_CACHE:-$HOME/projects/homeguardian-ai/build/_deps}"
SPDLOG_INC="$DEPS/spdlog-src/include"
JSON_INC="$DEPS/nlohmann_json-src/include"
[ -f "$SPDLOG_INC/spdlog/spdlog.h" ] || { echo "spdlog headers not found at $SPDLOG_INC"; exit 1; }
[ -f "$JSON_INC/nlohmann/json.hpp" ] || { echo "nlohmann headers not found at $JSON_INC"; exit 1; }

echo "==> Derive the native source list from the hg_mcp_server CMake target"
MCP_SRC="$(awk '/add_executable\(hg_mcp_server/{f=1;next} f&&/^[[:space:]]*\)/{exit} f' "$ROOT/CMakeLists.txt" \
           | grep -oE 'src/[A-Za-z0-9_/]+\.cpp' | grep -v '^src/main_mcp_server\.cpp$')"
N="$(printf '%s\n' "$MCP_SRC" | grep -c .)"
[ "$N" -ge 30 ] || { echo "source extraction looks wrong ($N files) - refusing to build a partial engine"; exit 1; }
# The terminator is indented ("    )"), so an unanchored /^\)/ would run past the
# target and sweep up the test sources. Guard against that with a shape check:
# every entry must be under src/ and the main() file must have been the only removal.
if printf '%s\n' "$MCP_SRC" | grep -qE '^(tests|test)/'; then
  echo "source extraction leaked test sources - refusing to build"; exit 1
fi
if printf '%s\n' "$MCP_SRC" | grep -qx 'src/main_mcp_server\.cpp'; then
  echo "source extraction failed to exclude main_mcp_server.cpp - refusing to build"; exit 1
fi
echo "    $N translation units from the hg_mcp_server target (main excluded), + 1 JNI file"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"/{obj,lib,classes,dex,gen}
mkdir -p "$BUILD_DIR/gen/lib/$ABI"

echo "==> Compile SQLite amalgamation (C, not C++)"
"$CC" -std=c11 -O2 -fPIC -DNDEBUG \
  -DSQLITE_OMIT_SHELL=1 -DSQLITE_OMIT_FTS3=1 -DSQLITE_OMIT_FTS4=1 -DSQLITE_OMIT_FTS5=1 \
  -DSQLITE_THREADSAFE=1 -Wno-unused-parameter \
  -c "$ROOT/third_party/sqlite/sqlite3.c" -o "$BUILD_DIR/obj/sqlite3.o"

echo "==> Compile libhg_edge_service.so ($ABI, API $NDK_API_LEVEL)"
INC=( -I"$ROOT/src" -I"$ROOT/src/core" -I"$ROOT/third_party/sqlite" -I"$SPDLOG_INC" -I"$JSON_INC" )
# shellcheck disable=SC2086
"$CXX" -std=c++20 -O2 -fPIC -DNDEBUG -DHOMEGUARDIAN_ANDROID=1 -Wall -Wextra \
  "${INC[@]}" \
  "$APK_DIR/jni/hg_edge_service_jni.cpp" $MCP_SRC \
  "$BUILD_DIR/obj/sqlite3.o" \
  -shared -o "$BUILD_DIR/lib/libhg_edge_service.so" \
  -llog -pthread

echo "==> Verify JNI exports against the Java native declarations (from the compiled ELF)"
bash "$ROOT/scripts/verify_jni_exports.sh" "$BUILD_DIR/lib/libhg_edge_service.so" \
  "$APK_DIR/src/org/homeguardian/edgeservice" "$TC/llvm-nm" || exit 1

# Nothing in this library may open a non-loopback listener. Assert the allow_remote
# escape hatch is not reachable from the app's own code path.
if "$TC/llvm-nm" -C --defined-only "$BUILD_DIR/lib/libhg_edge_service.so" 2>/dev/null | grep -qi 'allow_remote.*true'; then
  echo "WARNING: a symbol mentioning allow_remote=true exists; review before deploying"
fi

echo "==> Compile Java (Java 8 bytecode; no invokedynamic, no lambdas)"
JAVAC_SRC=$(find "$APK_DIR/src" -name '*.java')
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$PLATFORM/android.jar" \
  -d "$BUILD_DIR/classes" $JAVAC_SRC

echo "==> Dex (min-api $MIN_SDK_LEVEL)"
( cd "$BUILD_DIR/classes" && "$JAVA_HOME/bin/jar" cf "$BUILD_DIR/dex/in.jar" . )
"$BT/d8" --min-api "$MIN_SDK_LEVEL" --output "$BUILD_DIR/dex" "$BUILD_DIR/dex/in.jar"

echo "==> Package resources + manifest with aapt"
"$BT/aapt" package -f \
  -M "$APK_DIR/AndroidManifest.xml" \
  -S "$APK_DIR/res" \
  -I "$PLATFORM/android.jar" \
  -F "$BUILD_DIR/gen/base.apk"

echo "==> Add classes.dex and native libs"
cp "$BUILD_DIR/dex/classes.dex" "$BUILD_DIR/gen/classes.dex"
cp "$BUILD_DIR/lib/libhg_edge_service.so" "$BUILD_DIR/gen/lib/$ABI/libhg_edge_service.so"
LIBCXX="$TC_ROOT/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
[ -f "$LIBCXX" ] || { echo "missing libc++_shared.so for $ABI (looked in $LIBCXX)"; exit 1; }
cp "$LIBCXX" "$BUILD_DIR/gen/lib/$ABI/libc++_shared.so"

( cd "$BUILD_DIR/gen" && zip -q base.apk classes.dex )
( cd "$BUILD_DIR/gen" && zip -qr base.apk lib )

echo "==> zipalign"
"$BT/zipalign" -f 4 "$BUILD_DIR/gen/base.apk" "$BUILD_DIR/app-aligned.apk"

echo "==> Sign (local debug keystore, outside the repo)"
mkdir -p "$(dirname "$KEYSTORE")"
if [ ! -f "$KEYSTORE" ]; then
  "$JAVA_HOME/bin/keytool" -genkeypair -v -keystore "$KEYSTORE" \
    -storepass "$KS_PASS" -key-pass "$KS_PASS" -alias "$KS_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=HomeGuardian Harness Debug, O=HomeGuardian, C=US"
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" \
  --key-pass "pass:$KS_PASS" --out "$BUILD_DIR/homeguardian-edge-service.apk" \
  "$BUILD_DIR/app-aligned.apk"

echo "==> Verify"
"$BT/apksigner" verify --print-certs "$BUILD_DIR/homeguardian-edge-service.apk" | head -4
"$BT/aapt" dump badging "$BUILD_DIR/homeguardian-edge-service.apk" \
  | grep -E "package:|sdkVersion|targetSdkVersion|native-code|uses-permission" || true
echo "--- packaged ABIs ---"
unzip -l "$BUILD_DIR/homeguardian-edge-service.apk" | grep -E 'lib/' || true

echo
echo "==> DONE (built and signed only — NOT installed, no permission granted, nothing started)"
echo "APK: $BUILD_DIR/homeguardian-edge-service.apk"
ls -l "$BUILD_DIR/homeguardian-edge-service.apk" | awk '{printf "     %.2f MB\n", $5/1048576}'
