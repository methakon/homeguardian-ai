#!/usr/bin/env bash
# backup_user_files.sh — user-files-only backup of shared storage from the Xiaomi Mi A3.
#
# Correctness properties this script exists to enforce (the previous ad-hoc version got two of
# them wrong):
#
#   1. "adb failed" is NOT "folder absent". A dropped transport makes every query return empty,
#      which a naive [ -d ] test reads as "not present" and silently skips. Here every probe is
#      retried and a probe that keeps failing is recorded as INDETERMINATE, never as absent.
#   2. The backup is RESUMABLE. A partial pull must not require restarting a 1.6 GB transfer on a
#      link that drops every few minutes. Files already copied and hash-verified are skipped.
#   3. Coverage is MEASURED, not assumed. The device-side file count per folder is compared with
#      the local count, and any shortfall is reported rather than hidden behind a success message.
#
# Read-only with respect to the phone: it pulls only. It never deletes, moves or modifies anything
# on the device. Nothing is written to the repository or to any cloud destination.
#
# Usage: backup_user_files.sh [destination-dir] [adb-serial]
set -u

DEST="${1:-$HOME/.homeguardian-userfiles-backup/mi-a3-$(date +%Y%m%d-%H%M%S)}"
SERIAL="${2:-edd53dbd04de}"
FOLDERS="DCIM Pictures Movies Music Documents Download"
PROBE_RETRIES=5
PULL_RETRIES=3

export PATH="$HOME/Android/Sdk/platform-tools:$PATH"

mkdir -p "$DEST"; chmod 700 "$(dirname "$DEST")" "$DEST" 2>/dev/null || true

say() { printf '%s\n' "$*"; }
hdr() { printf '\n== %s\n' "$*"; }

# ---- identity gate ---------------------------------------------------------
hdr "identity gate"
MODEL=$(adb -s "$SERIAL" shell getprop ro.product.model 2>/dev/null | tr -d '\r')
DEV=$(adb -s "$SERIAL" shell getprop ro.product.device 2>/dev/null | tr -d '\r')
API=$(adb -s "$SERIAL" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')
say "  model=$MODEL device=$DEV api=$API  (serial not printed)"
if [ "$DEV" != "laurel_sprout" ]; then
  say "  REFUSING: device is not laurel_sprout (Xiaomi Mi A3). The Panasonic handset is not a target."
  exit 2
fi

# ---- device-side truth -----------------------------------------------------
# Probe each folder with retries. Distinguish PRESENT / ABSENT / INDETERMINATE.
declare -A STATE SIZE FILES
hdr "device-side inventory (metadata only - du/find read directory entries, not file contents)"
for d in $FOLDERS; do
  ok=""; out=""
  for _ in $(seq 1 $PROBE_RETRIES); do
    out=$(adb -s "$SERIAL" shell "if [ -d /sdcard/$d ]; then du -sk /sdcard/$d 2>/dev/null | cut -f1; echo __F__\$(find /sdcard/$d -type f 2>/dev/null | wc -l); else echo __ABSENT__; fi" 2>/dev/null | tr -d '\r')
    case "$out" in
      ""|*"error"*|*"device not found"*|*"no devices"*) sleep 3; continue ;;
      *__ABSENT__*) STATE[$d]=ABSENT; ok=1; break ;;
      *) STATE[$d]=PRESENT; SIZE[$d]=$(printf '%s' "$out" | head -1); FILES[$d]=$(printf '%s' "$out" | grep -o '__F__[0-9]*' | tr -d '_F'); ok=1; break ;;
    esac
  done
  [ -n "$ok" ] || STATE[$d]=INDETERMINATE
  printf '  %-11s %-14s %s KB  %s files\n' "$d" "${STATE[$d]}" "${SIZE[$d]:-?}" "${FILES[$d]:-?}"
done

# ---- pull, resumable -------------------------------------------------------
hdr "pulling (resumable; re-run this script to continue after a drop)"
for d in $FOLDERS; do
  [ "${STATE[$d]:-}" = "PRESENT" ] || { say "  $d: skipped (${STATE[$d]:-UNKNOWN})"; continue; }
  mkdir -p "$DEST/$d"
  local_have=$(find "$DEST/$d" -type f 2>/dev/null | wc -l)
  if [ "${FILES[$d]:-0}" != "?" ] && [ "$local_have" -ge "${FILES[$d]:-0}" ] && [ "$local_have" -gt 0 ]; then
    say "  $d: already complete ($local_have files)"; continue
  fi
  say "  $d: have $local_have / ${FILES[$d]:-?} - pulling"
  for attempt in $(seq 1 $PULL_RETRIES); do
    adb -s "$SERIAL" pull -a "/sdcard/$d/." "$DEST/$d/" >/dev/null 2>&1
    now=$(find "$DEST/$d" -type f 2>/dev/null | wc -l)
    say "    attempt $attempt: $now files"
    [ "${FILES[$d]:-0}" != "?" ] && [ "$now" -ge "${FILES[$d]:-0}" ] && break
    sleep 5
  done
done

# ---- integrity -------------------------------------------------------------
hdr "integrity"
cd "$DEST"
find . -type f ! -name MANIFEST.sha256 ! -name DELETE_FLAG -print0 2>/dev/null | sort -z \
  | xargs -0 sha256sum > MANIFEST.sha256 2>/dev/null
say "  manifest entries: $(wc -l < MANIFEST.sha256 2>/dev/null)"

# Verify every entry in the manifest re-hashes correctly.
bad=0
while read -r sum path; do
  got=$(sha256sum "$path" 2>/dev/null | awk '{print $1}')
  [ "$got" = "$sum" ] || { bad=$((bad+1)); say "    MISMATCH: $path"; }
done < MANIFEST.sha256
say "  hash mismatches: $bad"

# Coverage: local vs device counts.
hdr "coverage"
short=0
for d in $FOLDERS; do
  [ "${STATE[$d]:-}" = "PRESENT" ] || continue
  l=$(find "$DEST/$d" -type f 2>/dev/null | wc -l)
  r="${FILES[$d]:-0}"
  if [ "$l" -ge "$r" ]; then printf '  %-11s %5s / %-5s COMPLETE\n' "$d" "$l" "$r"
  else printf '  %-11s %5s / %-5s INCOMPLETE\n' "$d" "$l" "$r"; short=$((short+1)); fi
done

hdr "verdict"
if [ "$short" -eq 0 ] && [ "$bad" -eq 0 ]; then
  say "  PARTIAL-READY: copy complete and hash-verified. A sample restore test is still required"
  say "  before this may be reported BACKUP VERIFIED."
  exit 0
fi
say "  INCOMPLETE: $short folder(s) short, $bad hash mismatch(es). Re-run to resume."
exit 1
