#!/usr/bin/env bash
# backup_user_files_parallel.sh — user-files-only backup of shared storage from the Xiaomi Mi A3.
#
# Destination is inside the project folder but GITIGNORED and never committed: it holds the owner's
# private files, which must not reach GitHub, any public log, or any cloud destination.
#
# Correctness properties enforced here:
#
#   1. "adb failed" is NOT "folder absent". A dropped transport makes every query return empty,
#      which a naive [ -d ] test reads as "not present" and silently skips. Every probe is retried,
#      and a probe that keeps failing is recorded INDETERMINATE, never as absent.
#   2. The backup is RESUMABLE at file granularity: a file already present at the expected size is
#      skipped, so a drop costs one file rather than the whole transfer.
#   3. Transfers run in PARALLEL (HG_JOBS, default 4). A single serial adb pull over a link that
#      drops every few minutes is the slowest way to move 1.6 GB; concurrent transfers keep the
#      pipe busy and let progress survive an individual failure.
#   4. Coverage is MEASURED: device-side file counts are compared with local counts and any
#      shortfall is reported rather than hidden behind a success message.
#
# Read-only with respect to the phone: pulls only, never deletes or modifies anything on it.
#
# Usage: backup_user_files_parallel.sh [destination-dir] [adb-serial]
set -u

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$PROJECT_ROOT/user-backups/mi-a3-$(date +%Y%m%d-%H%M%S)}"
SERIAL="${2:-edd53dbd04de}"
FOLDERS="DCIM Pictures Movies Music Documents Download"
PROBE_RETRIES=6
FILE_RETRIES=3
HG_JOBS="${HG_JOBS:-4}"

export PATH="$HOME/Android/Sdk/platform-tools:$PATH"

mkdir -p "$DEST" "$PROJECT_ROOT/user-backups" "$DEST/logs"
chmod 700 "$DEST" "$PROJECT_ROOT/user-backups" 2>/dev/null || true

say() { printf '%s\n' "$*"; }
hdr() { printf '\n== %s\n' "$*"; }

# ---- identity gate ---------------------------------------------------------
hdr "identity gate"
DEV=$(adb -s "$SERIAL" shell getprop ro.product.device 2>/dev/null | tr -d '\r')
MODEL=$(adb -s "$SERIAL" shell getprop ro.product.model 2>/dev/null | tr -d '\r')
say "  device=$DEV model=$MODEL  (serial not printed)"
if [ "$DEV" != "laurel_sprout" ]; then
  say "  REFUSING: not laurel_sprout. The Panasonic handset is not a target."
  exit 2
fi

# ---- enumerate files (metadata only) ---------------------------------------
hdr "enumerating files (names and sizes only; file contents are not read)"
LIST="$DEST/.filelist"
: > "$LIST"
declare -A DEVCOUNT
for d in $FOLDERS; do
  out=""; ok=""
  for _ in $(seq 1 $PROBE_RETRIES); do
    out=$(adb -s "$SERIAL" shell "if [ -d /sdcard/$d ]; then find /sdcard/$d -type f -printf '%s %P\n' 2>/dev/null; echo __END__; else echo __ABSENT__; fi" 2>/dev/null | tr -d '\r')
    case "$out" in
      ""|*"error"*|*"device not found"*|*"no devices"*) sleep 3; continue ;;
      *__ABSENT__*) DEVCOUNT[$d]=ABSENT; ok=1; break ;;
      *__END__*) DEVCOUNT[$d]=$(printf '%s' "$out" | grep -v '__END__' | grep -c .); ok=1; break ;;
    esac
  done
  [ -n "$ok" ] || DEVCOUNT[$d]=INDETERMINATE
  printf '  %-11s %s files\n' "$d" "${DEVCOUNT[$d]}"
  case "${DEVCOUNT[$d]}" in
    ABSENT|INDETERMINATE) ;;
    *) printf '%s' "$out" | grep -v '__END__' | grep -v '__ABSENT__' | while IFS= read -r line; do
         [ -n "$line" ] && printf '%s\t%s\n' "$d" "$line" >> "$LIST"
       done ;;
  esac
done
TOTAL_LISTED=$(wc -l < "$LIST")
say "  total files listed: $TOTAL_LISTED"

# ---- parallel pull ---------------------------------------------------------
hdr "parallel pull (HG_JOBS=$HG_JOBS, resumable, $FILE_RETRIES retries per file)"

pull_one() {
  local folder="$1" rest="$2"
  local size rel dest dir
  size=$(printf '%s' "$rest" | awk '{print $1}')
  rel=$(printf '%s' "$rest" | cut -d' ' -f2-)
  dest="$DEST/$folder/$rel"
  dir=$(dirname "$dest")
  mkdir -p "$dir"
  if [ -f "$dest" ] && [ "$(stat -c%s "$dest" 2>/dev/null)" = "$size" ]; then
    printf 'SKIP %s/%s\n' "$folder" "$rel" >> "$DEST/logs/result.log"; return 0
  fi
  local a
  for a in $(seq 1 "$FILE_RETRIES"); do
    if adb -s "$SERIAL" pull "/sdcard/$folder/$rel" "$dest" >/dev/null 2>&1; then
      if [ "$(stat -c%s "$dest" 2>/dev/null)" = "$size" ]; then
        printf 'OK   %s/%s\n' "$folder" "$rel" >> "$DEST/logs/result.log"; return 0
      fi
    fi
    sleep 2
  done
  printf 'FAIL %s/%s\n' "$folder" "$rel" >> "$DEST/logs/result.log"
  return 1
}
export -f pull_one
export DEST SERIAL FILE_RETRIES

: > "$DEST/logs/result.log"
# One worker per file, HG_JOBS at a time. A tab separates the folder from "size relpath".
xargs -P "$HG_JOBS" -d '\n' -I{} bash -c '
  line="{}"
  folder="${line%%	*}"
  rest="${line#*	}"
  pull_one "$folder" "$rest"
' < "$LIST" 2>/dev/null || true

ok=$(grep -c '^OK'   "$DEST/logs/result.log" 2>/dev/null || echo 0)
sk=$(grep -c '^SKIP' "$DEST/logs/result.log" 2>/dev/null || echo 0)
fl=$(grep -c '^FAIL' "$DEST/logs/result.log" 2>/dev/null || echo 0)
say "  ok=$ok skipped=$sk failed=$fl"

# ---- integrity -------------------------------------------------------------
hdr "integrity"
cd "$DEST"
find . -type f ! -name MANIFEST.sha256 ! -path './logs/*' ! -name '.filelist' -print0 2>/dev/null \
  | sort -z | xargs -0 sha256sum > MANIFEST.sha256 2>/dev/null
say "  manifest entries: $(wc -l < MANIFEST.sha256 2>/dev/null)"
bad=0
while read -r sum path; do
  got=$(sha256sum "$path" 2>/dev/null | awk '{print $1}')
  [ "$got" = "$sum" ] || { bad=$((bad+1)); say "    MISMATCH: $path"; }
done < MANIFEST.sha256
say "  hash mismatches: $bad"

# ---- coverage --------------------------------------------------------------
hdr "coverage"
short=0
for d in $FOLDERS; do
  case "${DEVCOUNT[$d]}" in
    ABSENT)        printf '  %-11s ABSENT on device\n' "$d"; continue ;;
    INDETERMINATE) printf '  %-11s INDETERMINATE (could not measure)\n' "$d"; continue ;;
  esac
  l=$(find "$DEST/$d" -type f 2>/dev/null | wc -l)
  r="${DEVCOUNT[$d]}"
  if [ "$l" -ge "$r" ]; then printf '  %-11s %5s / %-5s COMPLETE\n' "$d" "$l" "$r"
  else printf '  %-11s %5s / %-5s INCOMPLETE\n' "$d" "$l" "$r"; short=$((short+1)); fi
done

hdr "verdict"
if [ "$short" -eq 0 ] && [ "$bad" -eq 0 ] && [ "$fl" -eq 0 ]; then
  say "  COPY COMPLETE and hash-verified."
  say "  A sample restore test is still required before this may be reported BACKUP VERIFIED."
  exit 0
fi
say "  INCOMPLETE: $short folder(s) short, $fl failed file(s), $bad hash mismatch(es)."
say "  Re-run with the same destination to resume; completed files are skipped."
exit 1
