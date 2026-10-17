#!/usr/bin/env bash
# Deploy (install/update) the HomeGuardian edge APK over Wi-Fi.
#
#   edge_dev_deploy.sh [path/to.apk]
#
# Because EdgeService is `android:exported="false"` (deliberately - no other app
# may start the server), adb cannot start it directly. It does not need to: the
# BootReceiver also handles MY_PACKAGE_REPLACED, so *installing an update starts
# the service*. Deployment is therefore also the restart path, and it needs no
# screen interaction - unlike tapping the in-app Restart button.
#
# Exit: 0 ok, 2 identity mismatch, 3 device gone, 5 deploy failed.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

SRC_APK="${1:-$(dirname "${BASH_SOURCE[0]}")/../../hg_service_apk_build/homeguardian-edge-service.apk}"
SERIAL="${HG_EDGE_TARGET:-}"

[[ -f "$SRC_APK" ]] || hg_die "APK not found: $SRC_APK" 5

# --- resolve the target: wireless serial if already connected, else USB ------
if [[ -z "$SERIAL" ]]; then
  # Prefer a wireless transport of the known serial, else the USB one.
  if hg_state "$HG_EDGE_SERIAL" >/dev/null 2>&1; then
    SERIAL="$HG_EDGE_SERIAL"
  else
    # Any *other* serial present? Refuse rather than guess.
    mapfile -t others < <(adb devices | awk 'NR>1 && $2=="device" && $1!="'"$HG_EDGE_SERIAL"'" {print $1}')
    (( ${#others[@]} == 0 )) || hg_die "expected serial $HG_EDGE_SERIAL is absent but found: ${others[*]} - refusing to deploy to an unverified device" 3
    hg_die "no device $HG_EDGE_SERIAL attached (USB or Wi-Fi)" 3
  fi
fi

hg_assert_mi_a3 "$SERIAL"
hg_log "deploying $(basename "$SRC_APK") to $SERIAL"

local_sha="$(sha256sum "$SRC_APK" | awk '{print $1}')"
hg_log "local apk sha256: $local_sha"

# --- install (streamed over the wireless link) ------------------------------
# --no-incremental: without it adb first attempts an "Incremental Install", which
# this device refuses with "java.lang.IllegalArgumentException: Incremental
# installation not allowed." before falling back to a Streamed Install. The
# fallback works, but it costs a round trip and puts a confusing stack trace into
# the evidence. Skipping the attempt keeps the deploy a single clean pass.
t0=$(date +%s)
out="$(timeout 180 adb -s "$SERIAL" install -r --no-incremental "$SRC_APK" 2>&1)"; rc=$?
t1=$(date +%s)
printf '%s\n' "$out" | sed 's/^/  /' >&2
grep -qi 'Success' <<<"$out" || hg_die "install failed (rc=$rc)" 5
hg_log "installed in $((t1-t0))s"

# --- verify the device is actually running OUR build -------------------------
dev_apk_path="$(hg_adb "$SERIAL" shell pm path "$HG_EDGE_PKG" | tr -d '\r' | sed 's/^package://' | head -1)"
[[ -n "$dev_apk_path" ]] || hg_die "cannot resolve installed apk path" 5
dev_sha="$(hg_adb "$SERIAL" shell sha256sum "$dev_apk_path" | tr -d '\r' | awk '{print $1}')"

ver="$(hg_adb "$SERIAL" shell dumpsys package "$HG_EDGE_PKG" \
        | tr -d '\r' | sed -n 's/.*versionName=\([^ ]*\).*/\1/p' | head -1)"
vcode="$(hg_adb "$SERIAL" shell dumpsys package "$HG_EDGE_PKG" \
        | tr -d '\r' | grep -m1 -oE 'versionCode=[0-9]+' | cut -d= -f2)"
minsdk="$(hg_adb "$SERIAL" shell dumpsys package "$HG_EDGE_PKG" \
        | tr -d '\r' | grep -m1 -oE 'minSdk=[0-9]+' | cut -d= -f2)"
abi="$(hg_adb "$SERIAL" shell dumpsys package "$HG_EDGE_PKG" | tr -d '\r' | awk -F= '/primaryCpuAbi=/{print $2}' | head -1)"

echo
echo "deployment verified"
echo "  local sha256    : $local_sha"
echo "  device sha256   : $dev_sha"
echo "  match           : $([[ "$local_sha" == "$dev_sha" ]] && echo YES || echo 'NO - MISMATCH')"
echo "  versionName    : $ver  (code $vcode, minSdk $minsdk)"
echo "  primaryCpuAbi   : $abi"
echo "  installed path  : $dev_apk_path"

[[ "$local_sha" == "$dev_sha" ]] || hg_die "installed APK does not match the built artifact" 5

# --- the install itself should have restarted the service --------------------
hg_log "waiting for MY_PACKAGE_REPLACED to bring the service up"
for i in $(seq 1 20); do
  pid="$(hg_adb "$SERIAL" shell pidof "$HG_EDGE_PKG" | tr -d '\r')"
  [[ -n "$pid" ]] && { echo "  service pid     : $pid (restarted by the install, no screen interaction needed)"; break; }
  sleep 1
done
[[ -n "${pid:-}" ]] || hg_log "WARNING: service not up yet - check ./edge_dev_logs.sh"
