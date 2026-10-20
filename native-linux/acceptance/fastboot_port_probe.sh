#!/bin/bash
# fastboot_port_probe.sh - read-only fastboot connectivity diagnostic for the Mi A3.
#
# PURPOSE
#   The first authorized fastboot session failed with error -71 (EPROTO) on port 2-1,
#   which sits on root hub usb2 = PCI 0000:00:14.0 = xhci_hcd. ADB worked on that same
#   port minutes earlier, so the failure is specific to the bootloader's USB interface.
#
#   Working hypothesis: the bootloader's USB stack cannot complete enumeration through
#   this host's xHCI controller, but may work on a native EHCI port.
#
#   This script does NOT assume that. It measures where the device actually lands.
#
# SAFETY
#   Read-only. It never issues unlock, flash, erase, slot, or vbmeta commands.
#   The only write-adjacent call is `reboot bootloader`, which requires --reboot
#   and is used solely to move the phone into bootloader mode for observation.
#
# USAGE
#   ./fastboot_port_probe.sh --observe          # watch for the device to appear
#   ./fastboot_port_probe.sh --reboot           # adb reboot bootloader, then observe
#   ./fastboot_port_probe.sh --query            # read-only getvars (only if enumerated)
#   ./fastboot_port_probe.sh --stability N      # N-second enumeration stability window

set -u

SDK="${ANDROID_SDK:-$HOME/Android/Sdk/platform-tools}"
ADB="$SDK/adb"
FB="$SDK/fastboot"
SERIAL="${MI_A3_SERIAL:-edd53dbd04de}"
LOG="${PROBE_LOG:-$(pwd)/fastboot_probe_$(date +%Y%m%d-%H%M%S).log}"

# Controller map, derived from this host (see the report).
# xhci = the port family that failed. ehci = untested candidate.
#
# Deliberately a `case` rather than an associative array: `declare -A` is a bashism,
# and this script previously died with a bare `Syntax error: "(" unexpected` when
# invoked as `sh script.sh` (dash cannot parse it), because the failure happens at
# parse time before any runtime guard could explain itself.
controller_of() {   # $1 = device path like 2-1  ->  prints "<pci> <type>"
    local root
    root="${1%%-*}"
    case "$root" in
        1) echo "0000:00:1a.0 ehci" ;;
        2) echo "0000:00:14.0 xhci" ;;
        3) echo "0000:00:14.0 xhci" ;;
        4) echo "0000:00:1d.0 ehci" ;;
        *) echo "unknown unknown" ;;
    esac
}

log() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }

# ---------------------------------------------------------------------------
# Locate a Xiaomi device (vendor 2717) anywhere in the USB tree and report the
# controller it landed on. This is the decisive measurement.
# ---------------------------------------------------------------------------
find_xiaomi() {
    for d in /sys/bus/usb/devices/*/; do
        [ -f "$d/idVendor" ] || continue
        v=$(cat "$d/idVendor" 2>/dev/null)
        [ "$v" = "2717" ] || continue
        p=$(cat "$d/idProduct" 2>/dev/null)
        sp=$(cat "$d/speed" 2>/dev/null)
        dn=$(cat "$d/devnum" 2>/dev/null)
        pr=$(cat "$d/product" 2>/dev/null)
        echo "$(basename "$d")|$v:$p|$sp|$dn|$pr"
        return 0
    done
    return 1
}

controller_of() {   # $1 = device path like 2-1
    local dev="$1" root
    root="${dev%%-*}"            # 2-1 -> 2 ; 1-1.3 -> 1
    echo "${CTRL_PCI[usb$root]:-unknown}"
}

# ---------------------------------------------------------------------------
cmd_observe() {
    log "=== OBSERVE: waiting for a Xiaomi device (2717:*) to appear ==="
    log "    plug the phone in; this does not touch the phone"
    local i=0 found=""
    while [ $i -lt 40 ]; do
        if found=$(find_xiaomi); then
            # portable split: a herestring (<<<) is a bashism and dash rejects it
            # outright, so use a heredoc instead.
            IFS='|' read -r path ids speed devnum prod <<EOF
$found
EOF
            # portable split (no mapfile: bash-only, would break under sh)
            ctrl_info=$(controller_of "$path")
            c_pci=${ctrl_info%% *}
            c_type=${ctrl_info##* }
            log "  FOUND on $path  ids=$ids speed=$speed devnum=$devnum product='$prod'"
            log "  >>> controller: $c_pci ($c_type)"
            case "$c_type" in
                xhci) log "  >>> this is the xHCI controller that FAILED before" ;;
                ehci) log "  >>> NATIVE EHCI - the untested candidate. Note this port." ;;
                *)    log "  >>> unrecognised controller" ;;
            esac
            return 0
        fi
        sleep 3; i=$((i+1))
    done
    log "  NOT FOUND after 120s. Nothing on vendor 2717."
    log "  kernel events:"; journalctl -k --since '3 min ago' --no-pager 2>/dev/null \
        | grep -iE 'usb .*error -71|new .* USB device|usb [0-9]+-[0-9]+:' | tail -12 | sed 's/^/    /'
    return 1
}

cmd_reboot() {
    log "=== REBOOT into bootloader (requires a working ADB link first) ==="
    if ! "$ADB" devices 2>/dev/null | grep -q "$SERIAL"; then
        log "  ABORT: $SERIAL not in ADB. Cannot safely issue reboot."
        log "  If the phone is stuck in bootloader, hold POWER ~15s instead."
        return 1
    fi
    # identity gate before touching anything
    local dev bld
    dev=$("$ADB" -s "$SERIAL" shell getprop ro.product.device 2>/dev/null | tr -d '\r')
    bld=$("$ADB" -s "$SERIAL" shell getprop ro.build.display.id 2>/dev/null | tr -d '\r')
    log "  identity: device=$dev build=$bld"
    [ "$dev" = "laurel_sprout" ] || { log "  ABORT: wrong device"; return 1; }
    case "$bld" in *V12.0.26.0.RFQMIXM*) ;; *) log "  ABORT: unexpected build"; return 1;; esac
    log "  identity gate PASS"
    "$ADB" -s "$SERIAL" reboot bootloader 2>&1 | sed 's/^/  /' | tee -a "$LOG"
    sleep 5
    cmd_observe
}

cmd_query() {
    log "=== READ-ONLY QUERY (only runs if fastboot enumerates) ==="
    if [ -z "$("$FB" devices 2>/dev/null)" ]; then
        log "  no fastboot device -> nothing to query. Stopping, as instructed."
        return 1
    fi
    log "  fastboot device present: $("$FB" devices 2>&1 | tr '\n' ' ')"
    # Each query individually bounded, and each result labelled so an empty or
    # error response is recorded as UNKNOWN rather than silently becoming a value.
    for v in product serial current-slot slot-successful slot-unbootable \
             slot-retry-count anti unlock-ability max-download-size; do
        out=$(timeout 12 "$FB" -s "$SERIAL" getvar "$v" 2>&1 | head -2 | tr '\n' ' ')
        if [ -z "$out" ] || echo "$out" | grep -qiE 'FAILED|unknown|not found|error'; then
            log "  getvar $v -> UNKNOWN   (raw: ${out:-<empty>})"
        else
            log "  getvar $v -> $out"
        fi
    done
    log "  -- flashing get_unlock_ability (read-only) --"
    out=$(timeout 12 "$FB" -s "$SERIAL" flashing get_unlock_ability 2>&1 | head -3 | tr '\n' ' ')
    log "  -> ${out:-UNKNOWN}"
    log "  NOTE: any UNKNOWN above is NOT a verified value."
}

cmd_stability() {
    local dur="${1:-120}" i=0 dn_first="" dn_changes=0 devs_lost=0
    log "=== STABILITY WINDOW: ${dur}s, watching USB devnum (bus truth, not transport_id) ==="
    while [ $i -lt "$dur" ]; do
        if found=$(find_xiaomi); then
            dn=$(echo "$found" | cut -d'|' -f4)
            [ -z "$dn_first" ] && { dn_first="$dn"; log "  baseline devnum=$dn"; }
            [ "$dn" != "$dn_first" ] && { dn_changes=$((dn_changes+1)); log "  devnum CHANGE -> $dn"; dn_first="$dn"; }
        else
            devs_lost=$((devs_lost+1))
        fi
        i=$((i+3)); sleep 3
    done
    log "  samples=$((dur/3))  devnum changes=$dn_changes  absent_samples=$devs_lost"
    if [ "$dn_changes" -eq 0 ] && [ "$devs_lost" -eq 0 ]; then
        log "  RESULT: STABLE - zero re-enumeration. Fastboot may be usable."
    else
        log "  RESULT: UNSTABLE - $dn_changes re-enumerations, $devs_lost absent samples."
        log "          A flash over this link would be unsafe."
    fi
}

# ---------------------------------------------------------------------------
case "${1:---help}" in
    --observe)   cmd_observe ;;
    --reboot)    cmd_reboot ;;
    --query)     cmd_query ;;
    --stability) shift; cmd_stability "${1:-120}" ;;
    *) cat <<EOF
Usage: $0 [--observe|--reboot|--query|--stability N]
Read-only. Never unlocks, flashes, erases, or changes slots.
Log: each run writes $LOG
EOF
    ;;
esac
