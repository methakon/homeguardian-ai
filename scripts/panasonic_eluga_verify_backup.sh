#!/usr/bin/env bash
# HG-CAM-03: prove the ENCRYPTED ARCHIVE is actually restorable.
#
# Why this exists: hashing the files sitting in the backup dir against a manifest we
# generated moments earlier proves almost nothing - it cannot detect a corrupt, truncated
# or unreadable archive. The only meaningful test is to decrypt the archive, extract it,
# and compare the RESULT against the recorded manifest.
#
# This test never touches the phone and never writes to it.
# Usage: panasonic_eluga_verify_backup.sh <archive.tar.zst.gpg>
# Exit: 0 PASS, 2 missing inputs, 3 extraction failed, 4 verification failed.

set -uo pipefail

ARCHIVE="${1:-}"
BK="${HG_BACKUP_DIR:-/home/swarna-sekhar-dhar/projects/homeguardian-ai/devices/backup/deviceprofile/panasonic-eluga-i7}"
KEY="${HG_BACKUP_KEY:-/home/swarna-sekhar-dhar/.config/hg-backup/eluga-i7.key}"
MANIFEST="$BK/manifest/checksums.sha256"
WORK="${HG_VERIFY_DIR:-/home/swarna-sekhar-dhar/backups/homeguardian/_restore-test}"

log() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
die() { log "FAIL: $*"; exit "${2:-4}"; }

[[ -n "$ARCHIVE" && -f "$ARCHIVE" ]] || die "archive not given or missing: '$ARCHIVE'" 2
[[ -f "$KEY" ]]      || die "key missing: $KEY" 2
[[ -f "$MANIFEST" ]] || die "manifest missing: $MANIFEST" 2

echo "restoration test"
echo "  archive   : $ARCHIVE ($(stat -c '%s' "$ARCHIVE") bytes)"
echo "  manifest  : $MANIFEST ($(wc -l < "$MANIFEST") entries)"
echo "  scratch   : $WORK"

# ---- 1. does the sidecar hash still match the archive (detects later corruption)? ----
if [[ -f "$ARCHIVE.sha256" ]]; then
  want=$(awk '{print $1}' "$ARCHIVE.sha256"); got=$(sha256sum "$ARCHIVE" | awk '{print $1}')
  echo "  archive sha256 sidecar : $([[ "$want" == "$got" ]] && echo 'MATCH' || echo 'MISMATCH')"
  [[ "$want" == "$got" ]] || die "archive hash does not match its sidecar - the file has changed" 3
fi

# ---- 2. decrypt + decompress + extract ----
rm -rf "$WORK"; mkdir -p "$WORK"; chmod 700 "$WORK"
log "decrypting and extracting (this is the real test)"
if ! gpg --batch --quiet --pinentry-mode loopback --passphrase-file "$KEY" -d "$ARCHIVE" 2>/dev/null \
     | zstd -d -q -c 2>/dev/null \
     | tar -x -C "$WORK" 2>/dev/null; then
  die "extraction pipeline failed - the archive is NOT restorable" 3
fi
EXTRACTED="$WORK/shared"
[[ -d "$EXTRACTED" ]] || die "extraction produced no 'shared/' tree" 3

# ---- 3. compare the RESTORED tree against the manifest, every entry ----
log "verifying every manifest entry against the restored tree"
TOTAL=0; OK=0; MISSING=0; MISMATCH=0
: > "$WORK/verify-detail.txt"
while read -r sum path; do
  [[ -n "${path:-}" ]] || continue
  TOTAL=$((TOTAL+1))
  f="$EXTRACTED/${path#./}"
  if [[ ! -f "$f" ]]; then
    MISSING=$((MISSING+1)); printf 'MISSING %s\n' "$path" >> "$WORK/verify-detail.txt"; continue
  fi
  if [[ "$(sha256sum "$f" | awk '{print $1}')" == "$sum" ]]; then
    OK=$((OK+1))
  else
    MISMATCH=$((MISMATCH+1)); printf 'MISMATCH %s\n' "$path" >> "$WORK/verify-detail.txt"
  fi
done < "$MANIFEST"

RESTORED_FILES=$(find "$EXTRACTED" -type f 2>/dev/null | wc -l)
RESTORED_BYTES=$(du -sb "$EXTRACTED" 2>/dev/null | awk '{print $1}')
VERDICT=$([[ "$TOTAL" -gt 0 && "$OK" -eq "$TOTAL" && "$MISSING" -eq 0 && "$MISMATCH" -eq 0 ]] && echo PASS || echo FAIL)

echo
echo "  result            : $VERDICT"
echo "  manifest entries  : $TOTAL"
echo "  hashes verified   : $OK"
echo "  missing           : $MISSING"
echo "  mismatched        : $MISMATCH"
echo "  files restored    : $RESTORED_FILES"
echo "  bytes restored    : $RESTORED_BYTES"
echo "  (details, if any  : $WORK/verify-detail.txt)"

# ---- 4. the failure detail is worth keeping; the 4 GB extraction is not ----
{
  echo "restoration test - $(date -Is)"
  echo "  archive           : $ARCHIVE"
  echo "  result            : $VERDICT"
  echo "  manifest entries  : $TOTAL"
  echo "  hashes verified   : $OK"
  echo "  missing           : $MISSING"
  echo "  mismatched        : $MISMATCH"
  echo "  files restored    : $RESTORED_FILES"
  echo "  bytes restored    : $RESTORED_BYTES"
} > "$BK/restore-test/archive_roundtrip.txt"
if [[ -s "$WORK/verify-detail.txt" ]]; then cp "$WORK/verify-detail.txt" "$BK/restore-test/archive_roundtrip_detail.txt"; fi

log "removing the 4 GB extraction (the encrypted archive is the artefact of record)"
rm -rf "$WORK"

[[ "$VERDICT" == "PASS" ]] || die "restored tree does not match the manifest" 4
log "PASS - the encrypted archive decrypts and restores byte-for-byte"
exit 0
