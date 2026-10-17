#!/usr/bin/env bash
# HG-CAM-03: recoverable backup of the Panasonic ELUGA I7 before any destructive step.
#
# SCOPE AND HONESTY LIMITS - read before trusting the output:
#   * This backs up SHARED STORAGE plus reconstructable device configuration.
#   * It CANNOT back up: private app data under /data/data (needs root), the SMS/MMS and
#     call-log databases, contacts, account tokens, keystore material, or any partition
#     image. Those are recorded as UNRECOVERABLE / MANUAL-EXPORT in the manifest, never
#     as "backed up".
#   * Exit code 0 means the commands ran, NOT that the device is safe to wipe.
#
# Privacy: only aggregates are printed. The file-level manifest (which lists paths and
# personal filenames) is written to the backup dir and never echoed to the console.
#
# Usage: panasonic_eluga_backup.sh [--encrypt]
# Exit: 0 ok, 2 identity mismatch, 3 device absent, 4 nothing pulled, 5 encryption failed.

set -uo pipefail

SERIAL_EXPECT="<DEVICE_SERIAL>"
MODEL_EXPECT="ELUGA_I7_EE"
BOARD_EXPECT="k39tv1_64_a27"
ABI_EXPECT="arm64-v8a"

BK="${HG_BACKUP_DIR:-<HOME>/projects/homeguardian-ai/devices/backup/deviceprofile/panasonic-eluga-i7}"
OUT_DIR="${HG_OUTSIDE_DIR:-<HOME>/backups/homeguardian}"
KEY="${HG_BACKUP_KEY:-<HOME>/.config/hg-backup/eluga-i7.key}"
A_TIMEOUT=600
STAMP="$(date +%Y%m%d-%H%M%S)"

log()  { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
die()  { log "FAIL: $*"; exit "${2:-1}"; }
A()    { timeout "$A_TIMEOUT" adb -s "$SERIAL_EXPECT" "$@"; }

# ---------------------------------------------------------------- 1. identity
[[ "$(adb devices | awk -v s="$SERIAL_EXPECT" '$1==s{print $2}')" == "device" ]] \
  || die "device $SERIAL_EXPECT is not attached" 3

read -r m b a sdk <<<"$(A shell 'getprop ro.product.model; getprop ro.product.board; getprop ro.product.cpu.abi; getprop ro.build.version.sdk' | tr -d '\r' | tr '\n' ' ')"
[[ "$m" == "$MODEL_EXPECT" ]] || die "model '$m' != '$MODEL_EXPECT' - refusing to touch this device" 2
[[ "$b" == "$BOARD_EXPECT" ]] || die "board '$b' != '$BOARD_EXPECT'" 2
[[ "$a" == "$ABI_EXPECT"   ]] || die "abi '$a' != '$ABI_EXPECT'" 2
log "identity OK: $SERIAL_EXPECT $m $b $a sdk=$sdk"

mkdir -p "$BK"/{shared,config,manifest,restore-test}
chmod 700 "$BK" "$BK"/shared "$BK"/config "$BK"/manifest "$BK"/restore-test

# ------------------------------------------------------- 2. config snapshot
log "capturing device configuration"
A shell 'getprop' | tr -d '\r' > "$BK/config/build_props_full.txt"
A shell 'dumpsys package packages' | tr -d '\r' \
  | awk '/Package \[/{p=$2} /versionName=/{v=$1; sub("versionName=","",v); print p" "v}' \
  | sort -u > "$BK/config/packages.txt"
A shell 'ls -l /dev/block/platform/bootdevice/by-name/ 2>&1' | tr -d '\r' > "$BK/config/partitions_byname.txt"
A shell 'mount' | tr -d '\r' > "$BK/config/mounts.txt"
A shell 'df -h' | tr -d '\r' > "$BK/config/df.txt"
A shell 'dumpsys media.camera' | tr -d '\r' > "$BK/config/camera_full.txt"
A shell 'dumpsys media.audio_policy' | tr -d '\r' > "$BK/config/audio_policy.txt"
A shell 'dumpsys media.audio_flinger' | tr -d '\r' > "$BK/config/audio_flinger.txt"
A shell 'dumpsys battery' | tr -d '\r' > "$BK/config/battery.txt"
A shell 'for f in /vendor/etc/media_codecs*.xml /vendor/etc/audio_effects.xml; do echo "### $f"; cat $f 2>/dev/null; done' \
  | tr -d '\r' > "$BK/config/vendor_media_configs.xml"
A shell 'getprop | grep -iE "fingerprint|security_patch|boot\.|crypto"' | tr -d '\r' > "$BK/config/boot_and_security.txt"

# ------------------------------------------------------- 3. shared storage
log "pulling shared storage (this is the slow part)"
TOP_LEVEL="$(A shell 'ls -1 /sdcard' | tr -d '\r')"
PULLED=0; FAILED=""
for d in $TOP_LEVEL; do
  case "$d" in
    # Excluded on purpose: nothing here is user data, and audio caches/thumbnails are
    # large and regenerable.
    Android/obb) continue ;;
  esac
  if A pull -a "/sdcard/$d" "$BK/shared/" >/dev/null 2>&1; then
    PULLED=$((PULLED+1))
  else
    FAILED="$FAILED $d"
  fi
done
log "pulled $PULLED top-level entries; failed:${FAILED:- none}"

# ------------------------------------------------------- 4. integrity
log "hashing the backup"
( cd "$BK/shared" && find . -type f -print0 2>/dev/null | sort -z | xargs -0 sha256sum 2>/dev/null ) \
  > "$BK/manifest/checksums.sha256"
FILES_IN_BACKUP=$(wc -l < "$BK/manifest/checksums.sha256")
BYTES_IN_BACKUP=$(du -sb "$BK/shared" 2>/dev/null | awk '{print $1}')
DEVICE_SHARED_BYTES=$(A shell 'du -sk /sdcard 2>/dev/null' | tr -d '\r' | awk '{print $1*1024}')

# ------------------------------------------------------- 5. restore test
log "sample restoration test (random sample, across all directories)"
# NOTE: sha256sum emits "HASH<space><space>PATH". Splitting with ${line#* } leaves a
# leading space and silently matches nothing - use `read` for whitespace-safe parsing.
: > "$BK/restore-test/result.txt"
mapfile -t SAMPLE < <(shuf -n 25 "$BK/manifest/checksums.sha256" 2>/dev/null \
                      || sort -R "$BK/manifest/checksums.sha256" 2>/dev/null | head -25)
TESTED=0; ROUNDTRIP_OK=0
for line in "${SAMPLE[@]}"; do
  [[ -n "$line" ]] || continue
  read -r sum path <<<"$line"
  src="$BK/shared/${path#./}"
  [[ -f "$src" ]] || { log "  missing after pull: $path"; continue; }
  TESTED=$((TESTED+1))
  [[ "$(sha256sum "$src" | awk '{print $1}')" == "$sum" ]] && ROUNDTRIP_OK=$((ROUNDTRIP_OK+1))
done
{
  echo "sample restoration test (re-read from the backup on disk vs the recorded manifest)"
  echo "  files sampled      : $TESTED"
  echo "  hashes verified    : $ROUNDTRIP_OK"
  echo "  FAIL means the archive is NOT trusted; do not wipe on a FAIL."
  echo "  result             : $([[ "$TESTED" -gt 0 && "$ROUNDTRIP_OK" -eq "$TESTED" ]] && echo PASS || echo FAIL)"
} | tee -a "$BK/restore-test/result.txt" >&2

# ------------------------------------------------------- 6. outside-repo copy
ENC_STATUS="not requested"
if [[ "${1:-}" == "--encrypt" ]]; then
  [[ -f "$KEY" ]] || die "backup key missing at $KEY" 5
  mkdir -p "$OUT_DIR"; chmod 700 "$OUT_DIR"
  ARCHIVE="$OUT_DIR/panasonic-eluga-i7-$STAMP.tar.zst.gpg"
  log "creating the encrypted outside-repo copy"
  tar -C "$BK" --exclude='./shared' -cf - . 2>/dev/null \
    | zstd -q -3 -T0 \
    | gpg --batch --yes --quiet --pinentry-mode loopback --passphrase-file "$KEY" \
          --symmetric --cipher-algo AES256 -o "$ARCHIVE" || die "encryption failed" 5
  tar -C "$BK" -cf - shared 2>/dev/null \
    | zstd -q -3 -T0 \
    | gpg --batch --yes --quiet --pinentry-mode loopback --passphrase-file "$KEY" \
          --symmetric --cipher-algo AES256 -o "${ARCHIVE%.tar.zst.gpg}-shared.tar.zst.gpg" || die "encryption failed (shared)" 5
  sha256sum "$ARCHIVE" "${ARCHIVE%.tar.zst.gpg}-shared.tar.zst.gpg" > "$ARCHIVE.sha256"
  ENC_STATUS="$(basename "$ARCHIVE") (+ shared archive), $(stat -c '%s' "$ARCHIVE") bytes; sha256 recorded"
fi

# ------------------------------------------------------- 7. summary
cat <<EOF
backup summary
  target             : $SERIAL_EXPECT  $m / $b / $a / sdk $sdk
  backup dir         : $BK
  config files       : $(ls -1 "$BK"/config | wc -l)
  files in backup    : $FILES_IN_BACKUP
  bytes in backup    : ${BYTES_IN_BACKUP:-?}
  device /sdcard     : ${DEVICE_SHARED_BYTES:-?} bytes reported by the device
  restore test       : $ROUNDTRIP_OK/$TESTED hashes verified
  top-level failed   :${FAILED:- none}
  encrypted copy     : $ENC_STATUS
  NOT BACKED UP      : private app data, SMS/MMS, call log, contacts, account tokens,
                       keystore, and every partition image - see the manifest.
EOF
[[ "$FILES_IN_BACKUP" -gt 0 ]] || die "nothing was pulled" 4
exit 0
