#!/usr/bin/env bash
# Build the HomeGuardian acceptance-test APK using the installed JDK, SDK, and
# NDK directly (no Gradle). Produces a debug-signed APK.
#
# The signing keystore is generated OUTSIDE the repository (in
# $HOME/.homeguardian-apk-keystore) and is never committed.
#
# Safety: this script only BUILDS and PACKAGES the APK. It does not install it,
# grant permissions, or start any capture. Capture is activated only by an
# explicit operator action inside the app after a runtime permission grant.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK_DIR="$ROOT/apk"
BUILD_DIR="$ROOT/hg_apk_build"

export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/android-ndk-r26d}"
NDK="$ANDROID_NDK_HOME"
SDK="$ANDROID_HOME"
BT="$SDK/build-tools/27.0.0"
[ -d "$BT" ] || BT="$SDK/build-tools/27.0.3"
PLATFORM="$SDK/platforms/android-27"
ABI="armeabi-v7a"
API=27

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CXX="$TOOLCHAIN/armv7a-linux-androideabi${API}-clang++"

KEYSTORE="$HOME/.homeguardian-apk-keystore/harness-debug.keystore"
KS_ALIAS="harnessdebug"
KS_PASS="homeguardian-harness"   # local debug key only; never committed

echo "==> Toolchain check"
[ -x "$CXX" ] || { echo "missing NDK compiler $CXX"; exit 1; }
[ -f "$PLATFORM/android.jar" ] || { echo "missing android.jar"; exit 1; }
[ -x "$BT/apksigner" ] || { echo "missing apksigner"; exit 1; }
"$JAVA_HOME/bin/java" -version 2>&1 | head -1

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"/{obj,lib,classes,dex,gen}
mkdir -p "$BUILD_DIR/gen/lib/$ABI"

echo "==> Compile native library (libhg_harness.so)"
NATIVE_SRC="$APK_DIR/jni/hg_harness_jni.cpp"
CORE_SRC=(
  "$ROOT/src/backend/android/NdkCameraDevice.cpp"
  "$ROOT/src/backend/android/AAudioCaptureDevice.cpp"
  "$ROOT/src/core/ConsentGate.cpp"
  "$ROOT/src/core/ConsentGuardedDevice.cpp"
)
INC=( -I"$ROOT/src" -I"$ROOT/src/core" -I"$ROOT/src/backend/android"
      -I"$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include" )
JSON_DIR="$(find "$ROOT"/hg_build/_deps -type d -path '*nlohmann_json*/include' 2>/dev/null | head -1 || true)"
[ -n "$JSON_DIR" ] || { echo "nlohmann_json include dir not found"; exit 1; }

"$CXX" -std=c++20 -fPIC -DHOMEGUARDIAN_ANDROID=1 -O1 -g \
  "${INC[@]}" -I"$JSON_DIR" \
  "$NATIVE_SRC" "${CORE_SRC[@]}" \
  -shared -o "$BUILD_DIR/lib/libhg_harness.so" \
  -lcamera2ndk -laaudio -lmediandk -llog -landroid

echo "==> Compile Java (target Java 8 bytecode for dx compatibility)"
JAVAC_SRC=$(find "$APK_DIR/src" -name '*.java')
"$JAVA_HOME/bin/javac" --release 8 -classpath "$PLATFORM/android.jar" -d "$BUILD_DIR/classes" $JAVAC_SRC

echo "==> Dex"
# Do NOT pass --min-sdk-version to dx: that enables invokedynamic desugaring,
# whose bootstrap descriptor the Android 8.1 (API 27) dex verifier on this
# device rejects (BootstrapMethodError / NoClassDefFoundError). Without it, dx
# emits legacy string-concat and interface-dispatch bytecode. The Java source
# also avoids lambdas for the same reason.
( cd "$BUILD_DIR/classes" && "$JAVA_HOME/bin/jar" cf "$BUILD_DIR/dex/in.jar" . )
"$BT/dx" --dex --output="$BUILD_DIR/dex/classes.dex" "$BUILD_DIR/dex/in.jar"

echo "==> Package base APK with aapt (compile resources + link + manifest)"
"$BT/aapt" package -f \
  -M "$APK_DIR/AndroidManifest.xml" \
  -S "$APK_DIR/res" \
  -I "$PLATFORM/android.jar" \
  -F "$BUILD_DIR/gen/base.apk"

echo "==> Add classes.dex and native lib"
cp "$BUILD_DIR/dex/classes.dex" "$BUILD_DIR/gen/classes.dex"
cp "$BUILD_DIR/lib/libhg_harness.so" "$BUILD_DIR/gen/lib/$ABI/libhg_harness.so"
# Bundle the NDK C++ runtime so the app does not depend on a system libc++_shared.
LIBCXX="$(ls "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so" 2>/dev/null || true)"
if [ -n "$LIBCXX" ]; then
  cp "$LIBCXX" "$BUILD_DIR/gen/lib/$ABI/libc++_shared.so"
  echo "bundled libc++_shared.so"
else
  echo "WARNING: libc++_shared.so not found; app may fail to dlopen"
fi
( cd "$BUILD_DIR/gen" && \
  "$JAVA_HOME/bin/jar" uf base.apk classes.dex 2>/dev/null || zip -j base.apk classes.dex >/dev/null )
( cd "$BUILD_DIR/gen" && zip -r base.apk lib >/dev/null )

echo "==> zipalign"
"$BT/zipalign" -f 4 "$BUILD_DIR/gen/base.apk" "$BUILD_DIR/app-aligned.apk"

echo "==> Sign (local debug keystore, outside repo)"
mkdir -p "$(dirname "$KEYSTORE")"
if [ ! -f "$KEYSTORE" ]; then
  "$JAVA_HOME/bin/keytool" -genkeypair -v -keystore "$KEYSTORE" \
    -storepass "$KS_PASS" -key-pass "$KS_PASS" -alias "$KS_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=HomeGuardian Harness Debug, O=HomeGuardian, C=US"
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:"$KS_PASS" \
  --key-pass pass:"$KS_PASS" --out "$BUILD_DIR/homeguardian-harness.apk" \
  "$BUILD_DIR/app-aligned.apk"

echo "==> Verify signature"
"$BT/apksigner" verify --print-certs "$BUILD_DIR/homeguardian-harness.apk" | head -5

echo "==> DONE"
echo "APK: $BUILD_DIR/homeguardian-harness.apk"
echo "NOTE: not installed, no permissions granted, no capture started."
