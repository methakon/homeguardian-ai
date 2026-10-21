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
# dx/d8/apksigner are shell wrappers that invoke `java`, so the JDK's bin must be
# on PATH as well as JAVA_HOME being set.
export PATH="$JAVA_HOME/bin:$PATH"
NDK="$ANDROID_NDK_HOME"
SDK="$ANDROID_HOME"
BT="$SDK/build-tools/27.0.0"
[ -d "$BT" ] || BT="$SDK/build-tools/27.0.3"
PLATFORM="${HG_PLATFORM:-$SDK/platforms/android-27}"
ABI="${HG_ABI:-armeabi-v7a}"
API="${HG_API:-27}"

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
# ABI/API are overridable (HG_ABI / HG_API / HG_PLATFORM) so the same script can build the
# arm64-v8a / API 28 Panasonic variant without forking it. Defaults are unchanged, so the
# H-03 harness build behaves exactly as before. The NDK ships both compilers:
# armv7a-linux-androideabi27-clang++ and aarch64-linux-android28-clang++.
case "$ABI" in
  armeabi-v7a) CXX_TRIPLE="armv7a-linux-androideabi"; LIBCXX_DIR="arm-linux-androideabi" ;;
  arm64-v8a)   CXX_TRIPLE="aarch64-linux-android";     LIBCXX_DIR="aarch64-linux-android" ;;
  *) echo "unsupported HG_ABI='$ABI' (expected armeabi-v7a or arm64-v8a)"; exit 1 ;;
esac
CXX="$TOOLCHAIN/${CXX_TRIPLE}${API}-clang++"

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
  "$ROOT/src/backend/android/Camera1Device.cpp"
  "$ROOT/src/backend/android/NdkCameraDevice.cpp"
  "$ROOT/src/backend/android/AAudioCaptureDevice.cpp"
  "$ROOT/src/core/ConsentGate.cpp"
  "$ROOT/src/core/ConsentGuardedDevice.cpp"
  "$ROOT/src/core/CaptureAccounting.cpp"
  "$ROOT/src/core/BoundedFeatures.cpp"
  "$ROOT/src/core/PreviewSizePolicy.cpp"
  "$ROOT/src/core/NodeIdentity.cpp"
)
INC=( -I"$ROOT/src" -I"$ROOT/src/core" -I"$ROOT/src/backend/android"
      -I"$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include" )
# The FetchContent tree is not in the repo (it is gitignored), so a FRESH WORKTREE has no
# hg_build/_deps and this script could not run at all. HG_DEPS_DIR lets a worktree point at any
# already-populated tree, and it defaults to the historical location so existing checkouts are
# unaffected. Both headers are checked, so a wrong path fails here with a clear message instead of
# failing later inside the compiler.
#
# CAVEAT - symlinked CHILDREN do not work. The lookups below use `find`, which does not descend a
# symlink. A symlinked HG_DEPS_DIR root IS followed (find starts there), but a directory whose
# children are symlinks - e.g. `_deps/nlohmann_json-src -> /elsewhere/...` - yields ZERO matches and
# this reports "not found" even though the headers are reachable. Materialise real directories, or
# point at a tree that already has them. Cost an hour to diagnose once; written down so it is once.
DEPS_DIR="${HG_DEPS_DIR:-$ROOT/hg_build/_deps}"
JSON_DIR="$(find "$DEPS_DIR" -type d -path '*nlohmann_json*/include' 2>/dev/null | head -1 || true)"
SPDLOG_INC_DIR="$(find "$DEPS_DIR" -type d -path '*spdlog*/include' 2>/dev/null | head -1 || true)"
[ -n "$JSON_DIR" ] || {
    echo "nlohmann_json include dir not found under: $DEPS_DIR"
    echo "  set HG_DEPS_DIR to a FetchContent tree, or populate hg_build/_deps first."
    exit 1
}
[ -n "$SPDLOG_INC_DIR" ] || {
    echo "spdlog include dir not found under: $DEPS_DIR"
    echo "  set HG_DEPS_DIR to a FetchContent tree, or populate hg_build/_deps first."
    exit 1
}

"$CXX" -std=c++20 -fPIC -DHOMEGUARDIAN_ANDROID=1 -O1 -g \
  "${INC[@]}" -I"$JSON_DIR" \
  "$NATIVE_SRC" "${CORE_SRC[@]}" \
  -shared -o "$BUILD_DIR/lib/libhg_harness.so" \
  -lcamera2ndk -laaudio -lmediandk -llog -landroid

echo "==> Verify JNI exports against the Java native declarations (from the compiled ELF)"
# The check itself lives in scripts/verify_jni_exports.sh so it can also be run on its own
# against any built .so. It reads the ELF symbol table, never the source text.
bash "$ROOT/scripts/verify_jni_exports.sh" "$BUILD_DIR/lib/libhg_harness.so" \
  "$APK_DIR/src/org/homeguardian/harness" "$TOOLCHAIN/llvm-nm" || exit 1


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
LIBCXX="$(ls "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/${LIBCXX_DIR}/libc++_shared.so" 2>/dev/null || true)"
if [ -n "$LIBCXX" ]; then
  cp "$LIBCXX" "$BUILD_DIR/gen/lib/$ABI/libc++_shared.so"
  echo "bundled libc++_shared.so"
else
  echo "WARNING: libc++_shared.so not found; app may fail to dlopen"
fi
( cd "$BUILD_DIR/gen" && \
  "$JAVA_HOME/bin/jar" uf base.apk classes.dex 2>/dev/null || zip -j base.apk classes.dex >/dev/null )
( cd "$BUILD_DIR/gen" && zip -r base.apk lib >/dev/null )

echo "==> Normalise ZIP metadata (reproducible builds)"
# The zip calls above stamp classes.dex, lib/ and META-INF with the build wall-clock, while aapt
# already normalises the entries it writes. Without this step two builds of IDENTICAL source
# produce different whole-file SHA-256 values, which makes "verify the artifact hash" meaningless.
# MUST run before zipalign and signing: the signature covers the archive, so normalising after
# signing would invalidate it (zipalign preserves entry timestamps).
[ -f "$ROOT/scripts/normalize_apk_zip.py" ] || {
    echo "missing $ROOT/scripts/normalize_apk_zip.py - reproducibility step cannot run"; exit 1
}
python3 "$ROOT/scripts/normalize_apk_zip.py" "$BUILD_DIR/gen/base.apk"

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
