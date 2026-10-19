#!/usr/bin/env bash
# build_kernel_native.sh — cross-build the Mi A3 kernel (laurel_sprout) from the pinned source.
#
# Produces Image + dtbs + modules without root and without pmbootstrap (which needs sudo for
# its chroot). Cross-compiling aarch64 from x86-64 runs at native speed — no emulation.
#
# FOUR HARD-WON RULES, each from a failed build:
#
#  1. PIN THE CONTAINER PLATFORM (--platform linux/amd64). Without it, a previously cached
#     arm64 image is silently reused and the whole build runs under qemu at ~10-20x slower.
#
#  2. NEVER USE --rm ON A BUILD YOU MIGHT NEED TO DEBUG. The first failure lost its logs
#     because the container was deleted on exit, costing a full rebuild cycle.
#
#  3. ALWAYS mrproper A TREE THAT WAS PREVIOUSLY BUILT FOR ANOTHER ARCH. An arm64 build
#     compiles the kernel's HOST tools (scripts/kconfig/*) as aarch64 ELF; on x86-64 they
#     then cannot execute and even `make defconfig` dies with:
#       aarch64-binfmt-P: Could not open '/lib/ld-linux-aarch64.so.1'
#     `make mrproper` + `git clean -fdx` is the fix. The kernel tree is NOT arch-agnostic
#     once configured.
#
#  4. BUILD MODULES, NOT JUST Image. Wi-Fi, the PMIC ADC and the temperature alarm are all
#     =m in this config. An Image without modules has NO WI-FI and no thermal sensing.
#
# Usage: build_kernel_native.sh [workdir]
set -u

W="${1:-$HOME/homeguardian-native-build}"
KREPO="https://codeberg.org/sm61x5-mainline/linux"
KTAG="v7.2.3-r0"
# The tag's dereferenced commit, verified with `git ls-remote --tags`.
KCOMMIT="5a88d98fa3d282ec4155a91577da86229d6b33d1"
IMAGE="ubuntu:24.04"

mkdir -p "$W"
LOG="$W/build_native.log"
: > "$LOG"

echo "workdir : $W"
echo "source  : $KREPO"
echo "tag     : $KTAG  ($KCOMMIT)"
echo "log     : $LOG"
echo

docker rm -f hg-native 2>/dev/null || true

# --- rule 1: pin the platform. ---
# --- rule 2: no --rm, so a failure can be inspected. ---
docker run --name hg-native --platform linux/amd64 -v "$W":/work -w /work ubuntu:24.04 \
  bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq git make bc bison flex libssl-dev libelf-dev \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu ca-certificates \
  xz-utils cpio python3 kmod >/dev/null 2>&1

echo "=== environment ==="
echo "  container arch : $(uname -m)     <- MUST be x86_64"
[ "$(uname -m)" = "x86_64" ] || { echo "  FATAL: not native; aborting rather than building under emulation"; exit 1; }
echo "  cross gcc      : $(aarch64-linux-gnu-gcc --version | head -1)"

cd /work
if [ ! -d linux/.git ]; then
  echo "=== cloning $KTAG ==="
  rm -rf linux
  git clone --depth 1 --branch '"$KTAG"' '"$KREPO"' linux 2>&1 | tail -2
fi
cd linux

echo "=== source revision ==="
echo "  HEAD     : $(git rev-parse HEAD 2>/dev/null || echo "unknown (tree cleaned)")"
echo "  describe : $(git describe --tags 2>/dev/null || echo n/a)"

# --- rule 3: purge any artifacts from a build of a different architecture. ---
echo "=== clearing stale artifacts (rule 3) ==="
echo "  kconfig host objects before : $(ls scripts/kconfig/*.o 2>/dev/null | wc -l)"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- mrproper >/dev/null 2>&1 || true
git clean -fdx >/dev/null 2>&1 || true
echo "  kconfig host objects after  : $(ls scripts/kconfig/*.o 2>/dev/null | wc -l)  (want 0)"

echo "=== defconfig + P0 fragments ==="
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
for opt in CONFIG_ARCH_QCOM CONFIG_ATH10K_SNOC CONFIG_ATH10K CONFIG_MAC80211 \
           CONFIG_CFG80211 CONFIG_QCOM_Q6V5_WCSS CONFIG_THERMAL CONFIG_THERMAL_OF \
           CONFIG_THERMAL_GOV_STEP_WISE CONFIG_QCOM_SPMI_ADC5 CONFIG_QCOM_SPMI_TEMP_ALARM \
           CONFIG_CHARGER_QCOM_SMB2 CONFIG_REGULATOR_QCOM_SPMI CONFIG_MMC_SDHCI_MSM \
           CONFIG_EXT4_FS CONFIG_F2FS_FS CONFIG_DM_CRYPT CONFIG_SERIAL_MSM \
           CONFIG_USB_CONFIGFS CONFIG_BT CONFIG_USB_GADGET; do
  scripts/config --enable "$opt" 2>/dev/null || true
done
# empty string, or the build demands a signing key we do not ship
scripts/config --set-str CONFIG_SYSTEM_TRUSTED_KEYS "" 2>/dev/null || true
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig >/dev/null 2>&1
echo "  configured"

echo "=== build: Image + dtbs + modules (rule 4) ==="
T0=$(date +%s)
make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules
RC=$?
T1=$(date +%s)
echo "  build rc=$RC in $(( (T1-T0)/60 )) min"

echo "=== install modules to a staging root ==="
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     INSTALL_MOD_PATH=/work/modules-root modules_install

echo "=== artifacts ==="
ls -l arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image
ls -l arch/arm64/boot/dts/qcom/sm6125-xiaomi-laurel-sprout.dtb
sha256sum arch/arm64/boot/dts/qcom/sm6125-xiaomi-laurel-sprout.dtb
echo "kernel release: $(cat include/config/kernel.release)"
echo "module count  : $(find /work/modules-root -name "*.ko*" | wc -l)"

echo "=== the P0 modules that must exist ==="
# Match BOTH naming conventions deliberately. Driver FILE names use hyphens
# (qcom-spmi-adc5.ko) while the CONFIG symbol uses underscores (CONFIG_QCOM_SPMI_ADC5), and
# grepping only for the config symbol's spelling produced a FALSE "ABSENT" for the entire
# thermal stack. Check the config symbol AND the hyphenated module name.
for m in ath10k_snoc ath10k_core mac80211 cfg80211 qcom_q6v5_wcss \
         qcom-spmi-adc-tm5 qcom-spmi-adc5 qcom-spmi-temp-alarm qcom-vadc-common; do
  f=$(find /work/modules-root \( -name "$m.ko*" -o -name "$(echo "$m" | tr '-' '_').ko*" \) 2>/dev/null | head -1)
  if [ -n "$f" ]; then echo "  PRESENT  $m"; else echo "  ABSENT   $m  <-- investigate"; fi
done
echo
echo "=== cross-check against the live .config so a wrong name cannot hide a gap ==="
for sym in CONFIG_ATH10K_SNOC CONFIG_CFG80211 CONFIG_MAC80211 CONFIG_QCOM_Q6V5_WCSS \
           CONFIG_QCOM_SPMI_ADC_TM5 CONFIG_QCOM_SPMI_ADC5 CONFIG_QCOM_SPMI_TEMP_ALARM; do
  val=$(grep -E "^$sym=" .config 2>/dev/null | cut -d= -f2)
  printf '  %-32s %s\n' "$sym" "${val:-<not set>}"
done
' 2>&1 | tee -a "$LOG"

echo
echo "container kept for inspection: docker logs hg-native"
echo "artifacts under: $W/linux/arch/arm64/boot/ and $W/modules-root"
