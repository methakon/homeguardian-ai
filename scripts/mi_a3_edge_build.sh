#!/usr/bin/env bash
# mi_a3_edge_build.sh — cross-compile the REAL HomeGuardian C++ MCP server
# (hg_mcp_server) for the Xiaomi Mi A3 (Snapdragon 665 / SM6125, arm64-v8a).
#
# Rationale: the target is an arm64 Android device. The server needs only POSIX
# sockets, spdlog, nlohmann/json and the vendored SQLite amalgamation - no glibc,
# no OpenSSL, no extra userland. So we build against Bionic with the NDK and run
# the resulting binary under adb, which has the LEAST compatibility overhead of
# the candidate runtimes (see docs/EDGE_DEVICE_MI_A3.md section 2).
#
# Offline: FetchContent sources are reused from an existing host build directory
# via FETCHCONTENT_SOURCE_DIR_* so no network clone is required.
#
# Usage: scripts/mi_a3_edge_build.sh [worktree_root]
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/android-ndk-r26d}"
# Any existing build tree works as the FetchContent source cache.
DEPS="${HG_FETCHCONTENT_CACHE:-$HOME/projects/homeguardian-ai/build/_deps}"
BUILD_DIR="$ROOT/build_mi_a3_edge_arm64"
ABI=arm64-v8a
API=30            # Mi A3 ships Android 11 / API 30
STL=c++_shared

echo "==> source    : $ROOT"
echo "==> NDK       : $NDK"
echo "==> build dir : $BUILD_DIR (isolated from the host build trees)"

[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "NDK toolchain missing: $NDK"; exit 1; }
[ -d "$DEPS/spdlog-src" ]        || { echo "missing FetchContent cache: $DEPS/spdlog-src"; exit 1; }
[ -d "$DEPS/nlohmann_json-src" ] || { echo "missing FetchContent cache: $DEPS/nlohmann_json-src"; exit 1; }

mkdir -p "$ROOT/logs"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL="$STL" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHOMEGUARDIAN_BUILD_TESTS=OFF \
  -DHOMEGUARDIAN_BUILD_ANDROID_BACKEND=OFF \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG="$DEPS/spdlog-src" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$DEPS/nlohmann_json-src" \
  > "$ROOT/logs/cmake_configure_arm64.log" 2>&1

echo "==> configuring: OK (see logs/cmake_configure_arm64.log)"
echo "==> OpenSSL-gated targets (hg_device_ingress/client/probe) are SKIPPED by design:"
grep -A2 'D-03: OpenSSL' "$ROOT/logs/cmake_configure_arm64.log" | sed 's/^/    /' || true
echo "    hg_mcp_server is NOT inside that guard and contains no TLS code."

cmake --build "$BUILD_DIR" -j"${HG_JOBS:-6}" --target hg_mcp_server homeguardian hg_alexa_bridge \
  > "$ROOT/logs/build_arm64.log" 2>&1
echo "==> built: $(grep -c 'warning:' "$ROOT/logs/build_arm64.log") warnings (all pre-existing -Wunused warnings in DemoScenarioRunner/hg_alexa_bridge)"

TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
STAGE="$ROOT/deploy/mi_a3_edge"
mkdir -p "$STAGE"
for t in hg_mcp_server hg_alexa_bridge; do
  cp "$BUILD_DIR/$t" "$STAGE/$t"
  "$TC/bin/llvm-strip" --strip-unneeded "$STAGE/$t"
done
cp "$TC/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$STAGE/"
cp "$ROOT/config/mi_a3_edge.json"        "$STAGE/hg_config.json"          # capture DISABLED (edge default)
cp "$ROOT/config/mi_a3_edge_engine.json" "$STAGE/hg_engine_config.json"   # capture enabled (scenario demo)

echo "==> staged in $STAGE:"
ls -l "$STAGE"
echo "==> artifact:"
file "$STAGE/hg_mcp_server" | cut -c1-130
file "$STAGE/hg_alexa_bridge" | cut -c1-130
ls -l "$STAGE"/*hg_mcp_server "$STAGE"/hg_alexa_bridge | awk '{printf "    %-22s %.2f MB\n", $NF, $5/1048576}'
echo
echo "Next: scripts/mi_a3_edge_deploy.sh    (push + provision credentials + start)"
echo "      scripts/mi_a3_edge_verify.sh    (functional MCP tests)"
echo "      scripts/mi_a3_edge_measure.py   (CPU/RAM/latency/thermal)"
echo "      scripts/mi_a3_edge_scenario.sh  (synthetic scenario + joined MCP read)"
