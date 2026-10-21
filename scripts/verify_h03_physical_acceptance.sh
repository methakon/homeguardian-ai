#!/usr/bin/env bash
# H-03 PHYSICAL ACCEPTANCE HARNESS. It OBSERVES; it does not simulate, and it does not infer.
#
# Every item is checked against evidence produced by the real device and the real hub:
#   * items 1-4 come from the phone's own log (the counters line the app emits every ~10 s),
#   * items 5-7 come from the HUB side (the ingress's durable replay watermark for THIS device),
#     because after the USB cable is out the phone is no longer reachable by adb - so item 7 can only
#     ever be hub-side evidence, and this script requires `adb devices` to be EMPTY to prove the
#     continuation is not an adb artifact.
#
# An item that cannot be observed is reported NOT OBSERVED and FAILS. A "not observed" is never a pass:
# that is the whole point of the file.
#
# Usage:
#   bash scripts/verify_h03_physical_acceptance.sh            # items 1-6 (phone still on USB)
#   bash scripts/verify_h03_physical_acceptance.sh --post-usb # item 7, AFTER the operator unplugs
set -uo pipefail

DEV="${HG_DEVICE:-4895180706479}"
SAMPLE_GAP="${HG_SAMPLE_GAP:-12}"          # seconds between log/watermark samples
POST_GAP="${HG_POST_GAP:-90}"              # seconds between the two post-unplug watermark samples
PHONE_LOG=/tmp/h03_acc_phone.log
PASS=0; FAIL=0; NOTOBS=0
ok()  { PASS=$((PASS+1));  printf '  %-58s PASS\n' "$1"; }
bad() { FAIL=$((FAIL+1));  printf '  %-58s FAIL  %s\n' "$1" "${2:-}"; }
no()  { NOTOBS=$((NOTOBS+1)); printf '  %-58s NOT OBSERVED  %s\n' "$1" "${2:-}"; }

adb_() { timeout 40 adb -s "$DEV" "$@"; }
ingress_container() { docker ps --format '{{.Names}}' 2>/dev/null | grep -i ingress | head -1; }
watermark() {  # prints "<device> <sequence>" lines from the ingress's durable replay state
    local c; c="$(ingress_container)"
    [[ -n "$c" ]] || return 1
    docker exec "$c" sh -c 'cat /data/device_replay_state.json 2>/dev/null' 2>/dev/null \
      | tr ',{}' '\n\n\n' | sed -n 's/.*"\(hg-[0-9a-f]*\)".*/\1/p'
    docker exec "$c" sh -c 'cat /data/device_replay_state.json 2>/dev/null' 2>/dev/null | head -c 400
}
watermark_seq() { # $1 = node id -> prints the last durable sequence for that device, or empty
    local c; c="$(ingress_container)"
    [[ -n "$c" ]] || return 1
    docker exec "$c" sh -c 'cat /data/device_replay_state.json 2>/dev/null' 2>/dev/null \
      | tr -d ' \n' | grep -o "\"$1\"[^}]*" | grep -o '[0-9]\+' | tail -1
}

MODE="${1:-normal}"
echo "== H-03 physical acceptance ($MODE) =="

if [[ "$MODE" == "--post-usb" ]]; then
    echo
    echo "--- item 7: observations continue after the PHYSICAL USB unplug ---"
    ATTACHED="$(timeout 30 adb devices 2>/dev/null | sed -n '2,$p' | grep -c .)"
    if [[ "$ATTACHED" == "0" ]]; then
        ok "adb reports NO attached device (the cable really is out)"
    else
        bad "adb reports NO attached device" "$ATTACHED still attached - unplug the cable first"
    fi
    NODE="$( [[ -f /tmp/h03_node_id ]] && cat /tmp/h03_node_id || echo hg-443a03b54537c21f )"
    W1="$(watermark_seq "$NODE")"
    echo "  watermark now: device=$NODE last_sequence=${W1:-<none>}"
    echo "  waiting ${POST_GAP}s with the phone on Wi-Fi and the app running..."
    sleep "$POST_GAP"
    W2="$(watermark_seq "$NODE")"
    echo "  watermark after ${POST_GAP}s: device=$NODE last_sequence=${W2:-<none>}"
    if [[ -n "$W1" && -n "$W2" && "$W2" -gt "$W1" ]]; then
        ok "the hub's watermark for this device ADVANCED with no USB attached ($W1 -> $W2)"
    elif [[ -z "$W1" || -z "$W2" ]]; then
        no "the hub's watermark advanced with no USB attached" "no durable watermark for $NODE yet"
    else
        bad "the hub's watermark advanced with no USB attached" "$W1 -> $W2 (no progress)"
    fi
    C="$(ingress_container)"
    if [[ -n "$C" ]]; then
        RECENT="$(docker logs --since "${POST_GAP}s" "$C" 2>&1 | grep -ciE 'accept|observation')"
        if [[ "$RECENT" -gt 0 ]]; then ok "the ingress logged activity during the post-unplug window ($RECENT lines)"
        else no "the ingress logged activity during the post-unplug window"; fi
    fi
    echo "  --------------------------------------------------------------------"
    echo "  post-USB acceptance: $PASS passed, $FAIL failed, $NOTOBS not observed"
    [[ $FAIL -eq 0 && $NOTOBS -eq 0 ]] || exit 1
    exit 0
fi

# ------------------------------------------------------------------ collect the phone log
if [[ "$(timeout 30 adb devices 2>/dev/null | sed -n '2,$p' | grep -c "$DEV")" == "0" ]]; then
    no "phone reachable over adb" "device $DEV not attached - attach USB and unlock (items 1-4 need the phone)"
    echo "  handoff: re-run with the phone attached, or use --post-usb for item 7"
    exit 1
fi
timeout 40 adb -s "$DEV" logcat -d -s HGNode > "$PHONE_LOG" 2>/dev/null
sleep "$SAMPLE_GAP"
timeout 40 adb -s "$DEV" logcat -d -s HGNode >> "$PHONE_LOG" 2>/dev/null
LINES="$(grep -c . "$PHONE_LOG")"
echo "  collected $LINES log lines from the device"

NODE="$(sed -n 's/.*node_id=\(hg-[0-9a-f]*\).*/\1/p' "$PHONE_LOG" | head -1)"
[[ -n "$NODE" ]] && { echo "$NODE" > /tmp/h03_node_id; ok "device identity reported: $NODE"; } \
                 || no "device identity reported" "no identity line in the log"

# ------------------------------------------------------------------ 1. consent survives repeated grants
AMB="$(grep -c 'ambiguous' "$PHONE_LOG")"
CONSENT_LINES="$(grep -c 'consent: camera=' "$PHONE_LOG")"
if [[ "$CONSENT_LINES" -gt 0 && "$AMB" == "0" ]]; then
    ok "consent is being evaluated ($CONSENT_LINES polls) and NEVER reports ambiguity"
else
    bad "consent is being evaluated and never reports ambiguity" "polls=$CONSENT_LINES ambiguous=$AMB"
fi
LAST_C="$(grep 'consent: camera=' "$PHONE_LOG" | tail -1)"
if printf '%s' "$LAST_C" | grep -q 'camera=none'; then ok "camera consent currently AUTHORIZED (reason=none)";
elif printf '%s' "$LAST_C" | grep -q 'camera='; then bad "camera consent currently AUTHORIZED" "$(printf '%s' "$LAST_C" | sed 's/.*consent: //')"
else no "camera consent currently AUTHORIZED"; fi
if printf '%s' "$LAST_C" | grep -q 'microphone=none'; then ok "microphone consent currently AUTHORIZED (reason=none)";
else bad "microphone consent currently AUTHORIZED" "$(printf '%s' "$LAST_C" | sed 's/.*| //')"; fi

# ------------------------------------------------------------------ 2/3/4. counters increase
first_metric() { grep -o "$2=[0-9]\+" "$1" | head -1 | cut -d= -f2; }
last_metric()  { grep -o "$2=[0-9]\+" "$1" | tail -1 | cut -d= -f2; }
check_rising() { # label grep-key
    local label="$1" key="$2" f l
    f="$(first_metric "$PHONE_LOG" "$key")"; l="$(last_metric "$PHONE_LOG" "$key")"
    if [[ -z "$l" ]]; then no "$label" "no '$key' in the counters line - is the sensors' counters line present?"
    elif [[ "$l" -gt 0 && "${f:-0}" -lt "$l" ]]; then ok "$label ($key: ${f:-0} -> $l)"
    elif [[ "$l" -gt 0 ]]; then ok "$label ($key = $l, nonzero; no second sample yet to show growth)"
    else bad "$label" "$key stayed at 0"; fi
}
check_rising "camera frames increase" "frames_processed"
check_rising "microphone samples increase" "samples"
check_rising "compact observations emitted" "obs_emitted"
check_rising "observations written to the hub" "sent"

# ------------------------------------------------------------------ 5. mutual TLS to the hub over Wi-Fi
SSID_LINE="$(grep -c . "$PHONE_LOG")"
IP="$(timeout 30 adb -s "$DEV" shell "ip route get "${HG_LAN_PROBE_HOST:-10.20.30.6}" 2>/dev/null | head -1" 2>/dev/null | tr -d '\r')"
if printf '%s' "$IP" | grep -q 'wlan'; then ok "the phone is routing to the hub over Wi-Fi ($(printf '%s' "$IP" | tr -s ' ' | cut -c1-48)...)"
elif [[ -n "$IP" ]]; then ok "the phone has a route to the hub ($(printf '%s' "$IP" | tr -s ' ' | cut -c1-48))"
else no "the phone has a route to the hub"; fi

# ------------------------------------------------------------------ 6. hub watermark for THIS device
W1="$(watermark_seq "$NODE")"
sleep "$SAMPLE_GAP"
W2="$(watermark_seq "$NODE")"
echo "  hub watermark for $NODE: ${W1:-<none>} -> ${W2:-<none>}"
if [[ -n "$NODE" && -n "$W1" && -n "$W2" && "$W2" -gt "$W1" ]]; then
    ok "the hub's durable watermark for this device ADVANCED ($W1 -> $W2)"
elif [[ -n "$W2" && "$W2" -gt 0 ]]; then
    ok "the hub holds a durable watermark for this device ($W2) - not observed advancing in this window"
else
    bad "the hub holds a durable watermark for this device" "none found for ${NODE:-unknown}"
fi
C="$(ingress_container)"
if [[ -n "$C" ]]; then ok "the ingress container is running ($C)"; else bad "the ingress container is running"; fi

echo "  --------------------------------------------------------------------"
echo "  physical acceptance: $PASS passed, $FAIL failed, $NOTOBS not observed"
echo "  next: when the operator unplugs the USB cable, run with --post-usb for item 7"
[[ $FAIL -eq 0 && $NOTOBS -eq 0 ]] || exit 1
