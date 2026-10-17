#!/usr/bin/env bash
# Shared helpers for Mi A3 edge development over Wi-Fi (HG-EDGE-04).
#
# DESIGN RULES (see docs/EDGE_WIRELESS_DEVELOPMENT_MI_A3.md):
#   * every adb call has a timeout;
#   * every operation targets an EXPLICITLY verified device - there is no
#     "pick the only device" fallback anywhere in this toolkit, because a second
#     phone (the MT6580 clone) can appear on the same cable/network and a wrong
#     target would silently install onto the wrong hardware;
#   * identity is proven by MULTIPLE independent identifiers, not by the model
#     string, because the Mi A3's USB descriptor misreports itself as
#     "Redmi Note 3 (ADB Interface)" (Xiaomi's generic ADB descriptor);
#   * non-zero exit codes on every failure path.
#
# Exit codes
#   0 ok   2 identity mismatch   3 device not found/offline   4 not paired
#   5 deploy failed   6 service action failed   7 test failed   8 no network

set -uo pipefail

: "${HG_EDGE_SERIAL:=edd53dbd04de}"     # USB serial / expected device
: "${HG_EDGE_HOST:=192.168.1.14}"       # LAN address of the phone
: "${HG_EDGE_ADB_TIMEOUT:=20}"
: "${HG_EDGE_PKG:=org.homeguardian.edgeservice}"
: "${HG_EDGE_MCP_PORT:=8790}"
: "${HG_EDGE_FWD_PORT:=18790}"

# Identity we accept. All of these must hold or the target is refused.
HG_EXPECT_DEVICE="laurel_sprout"
HG_EXPECT_PLATFORM="trinket"
HG_EXPECT_MANUFACTURER="Xiaomi"
HG_EXPECT_ABI="arm64-v8a"

# Known-bad: the MT6580 clone that shares the same USB port. Named explicitly so a
# mismatch reports WHICH wrong phone it found rather than a generic failure.
HG_CLONE_PLATFORM="mt6580"
HG_CLONE_ABI="armeabi-v7a"

hg_log()  { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
hg_die()  { hg_log "FAIL: $*"; exit "${2:-1}"; }

# adb with a timeout, targeting one explicit serial. No fallback, ever.
hg_adb() {
  timeout "$HG_EDGE_ADB_TIMEOUT" adb -s "$1" "${@:2}"
}

# Is the given serial present and in `device` state (not offline/unauthorized)?
hg_state() {
  adb devices | awk -v s="$1" '$1==s {print $2}' | head -1
}

# --- identity ---------------------------------------------------------------
# Print key=value for every identifier we care about, for one explicit target.
hg_identifiers() {
  local serial="$1"
  hg_adb "$serial" shell 'getprop ro.product.device; getprop ro.board.platform; \
      getprop ro.product.manufacturer; getprop ro.product.cpu.abi; \
      getprop ro.build.version.sdk; grep MemTotal /proc/meminfo' 2>/dev/null | tr -d '\r'
}

# Verify a target is the genuine Mi A3. Usage: hg_assert_mi_a3 <serial> [quiet]
# Exits 2 on mismatch, 3 if the target is not usable at all.
hg_assert_mi_a3() {
  local serial="$1" quiet="${2:-}"
  local st; st="$(hg_state "$serial")"
  case "$st" in
    "")          hg_die "device '$serial' is not attached (no transport) - refusing to fall back to another device" 3 ;;
    offline)     hg_die "device '$serial' is offline - run: adb reconnect offline" 3 ;;
    unauthorized) hg_die "device '$serial' is unauthorized - approve the USB debugging prompt on the phone" 3 ;;
    device)      : ;;
    *)           hg_die "device '$serial' has unexpected state '$st'" 3 ;;
  esac

  local got_device got_platform got_manuf got_abi got_sdk got_mem
  { read -r got_device; read -r got_platform; read -r got_manuf; \
    read -r got_abi; read -r got_sdk; read -r got_mem; } < <(hg_identifiers "$serial")

  local mem_kb; mem_kb="$(awk '{print $2}' <<<"$got_mem")"

  if [[ "$got_platform" == "$HG_CLONE_PLATFORM" || "$got_abi" == "$HG_CLONE_ABI" ]]; then
    hg_log "IDENTITY MISMATCH - this is the MT6580 clone, not the Mi A3:"
    hg_log "  platform=$got_platform abi=$got_abi manufacturer=$got_manuf sdk=$got_sdk mem=${mem_kb}kB"
    hg_die "refusing to operate on the clone" 2
  fi

  local bad=0
  [[ "$got_device"       == "$HG_EXPECT_DEVICE"       ]] || { hg_log "  ro.product.device='$got_device' (want $HG_EXPECT_DEVICE)"; bad=1; }
  [[ "$got_platform"     == "$HG_EXPECT_PLATFORM"     ]] || { hg_log "  ro.board.platform='$got_platform' (want $HG_EXPECT_PLATFORM)"; bad=1; }
  [[ "$got_manuf"        == "$HG_EXPECT_MANUFACTURER" ]] || { hg_log "  ro.product.manufacturer='$got_manuf' (want $HG_EXPECT_MANUFACTURER)"; bad=1; }
  [[ "$got_abi"          == "$HG_EXPECT_ABI"          ]] || { hg_log "  abi='$got_abi' (want $HG_EXPECT_ABI)"; bad=1; }
  (( mem_kb > 4000000 )) || { hg_log "  MemTotal=${mem_kb}kB is too small for a 6GB Mi A3"; bad=1; }

  (( bad == 0 )) || hg_die "identity check failed - this is NOT a verified Mi A3" 2

  [[ -n "$quiet" ]] || hg_log "identity OK: serial=$serial device=$got_device platform=$got_platform abi=$got_abi sdk=$got_sdk mem=$((mem_kb/1024))MB"
  return 0
}

# Wait for a serial to appear as a usable transport (bounded).
hg_wait_present() {
  local serial="$1" deadline=$(( $(date +%s) + ${2:-20} ))
  while (( $(date +%s) < deadline )); do
    [[ "$(hg_state "$serial")" == "device" ]] && return 0
    sleep 1
  done
  return 1
}

# --- redaction --------------------------------------------------------------
# Never let a pairing code, token or session id reach a saved log. 6-digit
# standalone numbers are the pairing-code shape; bearer tokens are long hex.
hg_redact() {
  sed -E 's/\b[0-9]{6}\b/[PAIRING-CODE-REDACTED]/g; s/\b[0-9a-fA-F]{32,}\b/[TOKEN-REDACTED]/g'
}
