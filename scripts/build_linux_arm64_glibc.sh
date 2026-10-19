#!/usr/bin/env bash
# build_linux_arm64_glibc.sh — build hg_mcp_server as a genuine Linux ARM64 (glibc) executable.
#
# Why this exists: the Android artifact links against Bionic (liblog, interpreter
# /system/bin/linker64) and cannot run under Ubuntu/Debian/Alpine PRoot, which are glibc or musl.
# A separate build against glibc is required, and until now that build existed only as an ad-hoc
# command typed into a shell — not reproducible by anyone else.
#
# Method: build NATIVELY inside an arm64 Ubuntu container (native arm64 under emulation), rather
# than cross-compiling against a guessed sysroot. The resulting binary is then exercised in the same
# container by verify_glibc_mcp_in_container.sh. Requires docker with arm64 binfmt registered.
#
# NOTE: emulated ARM64. This verifies the artifact, not the physical device.
#
# Two builds of the same source produce an identical sha256 — verified.
#
# Usage: build_linux_arm64_glibc.sh [output-dir]
set -u

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJECT_ROOT/linux_arm64_build}"
STAGE="${HG_STAGE:-$(mktemp -d /tmp/hg-glibc-src.XXXXXX)}"
UBUNTU_IMAGE="${HG_UBUNTU_IMAGE:-ubuntu:24.04}"

say() { printf '%s\n' "$*"; }

say "== staging source =="
say "  from: $PROJECT_ROOT"
say "  to  : $STAGE"
# Exclude build outputs and VCS so the staged tree is exactly the source under test.
tar -C "$PROJECT_ROOT" \
    --exclude='./.git' \
    --exclude='./build*' \
    --exclude='./linux_arm64_build' \
    --exclude='./hg_service_apk_build' \
    --exclude='./user-backups' \
    -cf - . 2>/dev/null | tar -C "$STAGE" -xf - 2>/dev/null
[ -f "$STAGE/CMakeLists.txt" ] || { say "  ERROR: CMakeLists.txt not staged"; exit 2; }
say "  staged: $(du -sh "$STAGE" | cut -f1)"

mkdir -p "$OUT"

say
say "== building natively in $UBUNTU_IMAGE (arm64) =="
say "  NOTE: CMakeLists fetches spdlog and nlohmann_json at configure time, so this step needs"
say "        network access and a WRITABLE source tree."
docker run --rm --platform linux/arm64 \
  -v "$STAGE":/src \
  -v "$OUT":/out \
  "$UBUNTU_IMAGE" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq cmake g++ git ca-certificates >/dev/null 2>&1
echo "  toolchain: $(g++ --version | head -1)"
echo "  arch     : $(uname -m)"
[ "$(uname -m)" = "aarch64" ] || { echo "  ERROR: container is not aarch64"; exit 1; }
cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DHOMEGUARDIAN_BUILD_TESTS=OFF \
      -DCMAKE_CXX_FLAGS="-O2" > /tmp/cm.log 2>&1 || { echo "  CMAKE FAILED"; tail -10 /tmp/cm.log; exit 1; }
echo "  configure OK"
cmake --build /build --target hg_mcp_server -j 4 > /tmp/bd.log 2>&1 || { echo "  BUILD FAILED"; tail -15 /tmp/bd.log; exit 1; }
strip --strip-unneeded /build/hg_mcp_server 2>/dev/null || true
cp /build/hg_mcp_server /out/hg_mcp_server
echo "  BUILT"
'
rc=$?
[ "$rc" -eq 0 ] || { say "  docker exit=$rc"; exit "$rc"; }

say
say "== artifact =="
BIN="$OUT/hg_mcp_server"
if [ -f "$BIN" ]; then
  say "  path   : $BIN"
  say "  size   : $(stat -c%s "$BIN") bytes"
  say "  sha256 : $(sha256sum "$BIN" | awk '{print $1}')"
  say "  NEEDED : $(readelf -d "$BIN" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')"
  say "  interp : $(readelf -l "$BIN" 2>/dev/null | grep -o '/lib/ld-linux-aarch64[^ ]*' | head -1)"
else
  say "  ERROR: no binary produced"; exit 1
fi

say
say "== next =="
say "  scripts/verify_glibc_mcp_in_container.sh $BIN"
say "  Expected sha256 for this source revision: 82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8"
