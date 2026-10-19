#!/usr/bin/env bash
# build_native_image.sh — build the Mi A3 native Linux image. NOT run as part of this work.
#
# This script exists so the build is reviewable and repeatable rather than an ad-hoc sequence
# of commands. It produces a flashable image; the standing instruction is to PREPARE only and
# not to install, and flashing additionally requires a demonstrated reliable fastboot
# transport and explicit owner approval of the exact procedure.
#
# Read reports/07-native-linux-readiness.md before running this.
set -eu

# ---- target ------------------------------------------------------------------
PMB_VENDOR="xiaomi"
PMB_DEVICE="laurel"                 # pmaports codename for the Mi A3 (device-xiaomi-laurel)
PMB_UI="none"                       # headless: this is a hub, not a phone UI
PMB_SYSTEMD="systemd"               # systemd supervision for the MCP service
PMB_HOSTNAME="hg-hub"
PMB_USER="hub"

# ---- reproducibility ---------------------------------------------------------
# The kernel and device packages are checksum-pinned inside pmaports:
#   linux-nura-qcom-sm61x5  pkgver=7.2.3  pkgrel=0
#   source: https://codeberg.org/sm61x5-mainline/linux  tag v7.2.3-r0
#   tarball sha512 2a6393212c413cf3812c2293d074fc9b7f82dec01c740251fe390b0633484b247
#                  e95e886493cbec77e6ef9741aebe0a4134c2218672424b332ae119c22566f23
#   config  sha512 c84672dd702acf5cdad7ed57ffa6aaeb4d0cc3c71d1ef4c9f5a1a2450e0f3d0
#                  85f331c4489ec345c63a89f753c882ce42ba0d661928cd2d4c393f1144286d2b1
#
# Record the exact pmaports commit used, so an image can be tied to a revision:
#   git -C ~/.local/var/pmbootstrap/pmaports rev-parse HEAD
#
# Prerequisite: the kernel revision MUST include the DRAM carveout fix
# (sm6125-xiaomi-laurel-sprout: enlarge reserved_mem1, patch v2). Without it the device
# hard-locks under sustained buffered reads — which is exactly our workload.
# Verify before building:
#   pmbootstrap chroot -b -- sh -c 'grep -A3 reserved_mem1 /usr/src/linux/arch/arm64/boot/dts/qcom/sm6125-xiaomi-laurel-sprout.dts'

say() { printf '\n== %s\n' "$*"; }

say "0. prerequisites"
command -v pmbootstrap >/dev/null 2>&1 || {
  echo "pmbootstrap not installed. Install it from the postmarketOS repository first."
  echo "This script never installs host tooling implicitly."
  exit 1
}

say "1. init (answers pinned non-interactively)"
pmbootstrap -q init \
  --aports ~/.local/var/pmbootstrap/pmaports \
  --vendor "$PMB_VENDOR" \
  --device "$PMB_DEVICE" \
  --ui "$PMB_UI" \
  --systemd "$PMB_SYSTEMD" \
  --hostname "$PMB_HOSTNAME" \
  --user "$PMB_USER" \
  --no-ssh-keys

say "2. build device + kernel packages"
pmbootstrap -q build device-xiaomi-laurel
pmbootstrap -q build linux-nura-qcom-sm61x5

say "3. build the HomeGuardian MCP package"
# Local package: copy the APKBUILD and the verified binary into the pmaports tree, then build.
# The APKBUILD asserts the binary sha256 during prepare(), so a wrong artifact fails here.
HERE="$(cd "$(dirname "$0")" && pwd)"
DST=~/.local/var/pmbootstrap/pmaports/device/testing/homeguardian-mcp
mkdir -p "$DST"
cp "$HERE/packaging/homeguardian-mcp/APKBUILD"        "$DST/"
cp "$HERE/packaging/homeguardian-mcp/hg-mcp.service"  "$DST/"
cp "$HERE/packaging/homeguardian-mcp/hg-mcp.conf"     "$DST/" 2>/dev/null || true
# Reuse the single verified artifact already in the repository rather than keeping a second
# copy of a 2.7 MB binary. Its sha256 is asserted by the APKBUILD's prepare().
# $HERE is native-linux/, so the repository root is one level up.
cp "$HERE/../deploy/linux_arm64/hg_mcp_server"        "$DST/"
( cd "$DST" && pmbootstrap -q checksum homeguardian-mcp 2>/dev/null || true )
pmbootstrap -q build homeguardian-mcp

say "4. install (writes the image; does NOT flash)"
pmbootstrap -q install

say "done"
cat <<'EOF'
Image written under ~/.local/var/pmbootstrap/.

NEXT: do not flash yet. Required first:
  - a demonstrated reliable fastboot transport (current USB drops with error -71)
  - a verified stock Android 11 fastboot image held locally for rollback
  - owner approval of the exact flash procedure

Flashing targets, for reference only (from deviceinfo):
  boot / vbmeta / dtbo partitions, 4096-byte pages, sparse images
EOF
