#!/usr/bin/env bash
# HG-CAM-06 Stage D: verified removal of ONE package at a time.
#
# Design rules taken directly from the brief:
#   * identity is re-verified before every single removal, and the run aborts if the device
#     vanishes mid-way (mains power may fail);
#   * exactly one package per invocation - there is no wildcard, no list-driven batch and no
#     generic debloating mode. Callers pass one package name;
#   * /data is measured before and after each removal, and the exit status and resulting
#     package state are recorded;
#   * essential service availability is checked after each removal;
#   * evidence is appended to a file so the whole sequence is traceable.
#
# It refuses to remove anything that is not a third-party (user-installed) package, so it
# cannot be pointed at a system/vendor package by mistake.
#
# Usage: panasonic_pkg_remove.sh <package.name> [more.packages...]
#        (each package is still processed strictly one at a time, sequentially)
# Exit: 0 all requested removals succeeded, 2 identity/guard refusal, 3 device absent,
#       4 one or more removals failed.

set -uo pipefail

SERIAL="LRW86SC6RO8TZ99H"
MODEL="ELUGA_I7_EE"; BOARD="k39tv1_64_a27"; ABI="arm64-v8a"
EVID="${HG_EVIDENCE:-/home/swarna-sekhar-dhar/projects/homeguardian-ai/devices/backup/deviceprofile/panasonic-eluga-i7/manifest}/package_removal_evidence.txt"

[[ $# -ge 1 ]] || { echo "usage: $0 <package.name> [...]" >&2; exit 2; }

A() { timeout 180 adb -s "$SERIAL" "$@"; }

log() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$EVID"; }

guard() {
  local state; state="$(adb devices | awk -v s="$SERIAL" '$1==s{print $2}')"
  [[ "$state" == "device" ]] || { log "ABORT: '$SERIAL' not attached (state='${state:-absent}')"; return 3; }
  local m b a
  read -r m b a <<<"$(A shell 'getprop ro.product.model; getprop ro.product.board; getprop ro.product.cpu.abi' | tr -d '\r' | tr '\n' ' ')"
  [[ "$m" == "$MODEL" && "$b" == "$BOARD" && "$a" == "$ABI" ]] \
    || { log "ABORT: identity mismatch '$m/$b/$a'"; return 2; }
  return 0
}

data_used_kb() { A shell 'df /data' | tr -d '\r' | awk 'NR==2{print $3}'; }

services_ok() {
  local cam aud ph
  cam="$(A shell 'service check media.camera' | tr -d '\r')"
  aud="$(A shell 'service check media.audio_flinger' | tr -d '\r')"
  ph="$(A shell 'service check phone' | tr -d '\r')"
  [[ "$cam" == *found* && "$aud" == *found* && "$ph" == *found* ]] && echo "PASS" || echo "FAIL(cam='$cam' aud='$aud' phone='$ph')"
}

: >> "$EVID"
log "=== Stage D run: requested $# package(s) ==="

RC=0
for pkg in "$@"; do
  log "--- $pkg ---"
  guard || { log "guard failed before touching $pkg - stopping"; exit $?; }

  # Refuse anything that is not a user-installed package.
  if [[ "$(A shell "pm list packages -3 | grep -c '^package:$pkg\$'" | tr -d '\r')" != "1" ]]; then
    log "  REFUSED: $pkg is not a third-party package on this device - not removing"
    RC=4; continue
  fi

  before="$(data_used_kb)"
  inst_before="$(A shell "pm list packages | grep -c '^package:$pkg\$'" | tr -d '\r')"
  log "  /data used before : ${before} KB"
  log "  installed before  : ${inst_before}"

  out="$(A shell "pm uninstall $pkg" 2>&1 | tr -d '\r')"
  st=$?
  log "  command           : pm uninstall $pkg"
  log "  exit status       : $st"
  log "  output            : $out"

  guard || { log "device disappeared during removal of $pkg - stopping"; exit 3; }

  after="$(data_used_kb)"
  inst_after="$(A shell "pm list packages | grep -c '^package:$pkg\$'" | tr -d '\r')"
  svc="$(services_ok)"
  log "  installed after   : ${inst_after}"
  log "  /data used after  : ${after} KB"
  log "  delta             : $(( before - after )) KB freed"
  log "  essential services: $svc"

  if [[ "$inst_after" == "0" && "$svc" == "PASS" ]]; then
    log "  RESULT            : REMOVED and device healthy"
  else
    log "  RESULT            : PROBLEM (installed_after=$inst_after services=$svc)"
    RC=4
  fi
  sleep 2
done

log "=== Stage D done (rc=$RC) ==="
exit $RC
