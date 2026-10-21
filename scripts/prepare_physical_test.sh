#!/usr/bin/env bash
# prepare_physical_test.sh — READ-ONLY preflight for a physical harness test on a designated phone.
#
# WHAT THIS DOES
#   Verifies every precondition for a physical camera/microphone test and prints a precise GO/NO-GO,
#   together with the exact install and start commands that WOULD be run. It runs NONE of them.
#
# WHAT THIS DELIBERATELY DOES NOT DO
#   * no `adb install`, no uninstall, no `pm` mutation of any kind
#   * no camera or microphone activation, no `am start`
#   * no device setting change, no app data access
#   Installing the APK and starting the capture service are PROHIBITED actions for this worker until
#   the owner records explicit authorization. Preparation is what can be done safely while waiting.
#
# WHY IT IS SEPARATE FROM THE TEST ITSELF
#   The read-only half of a physical test can be run at any time; the mutating half cannot. Keeping
#   them in one script is how a "preflight" quietly becomes an install. This script cannot install:
#   the guarded commands are printed, never executed, and the script greps itself to prove it.
#
# USAGE
#   scripts/prepare_physical_test.sh [--serial <SERIAL>] [--apk <path>] [--artifacts <dir>]
#
# EXIT
#   0  every precondition met — the printed commands are the complete remaining step
#   3  a precondition FAILED (the reason is printed)
#   2  usage/environment problem (adb or build-tools missing)

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
BT="${BUILD_TOOLS:-$HOME/Android/Sdk/build-tools/34.0.0}"
export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export PATH="$JAVA_HOME/bin:$PATH"

SERIAL=""
APK="$ROOT/hg_apk_build/homeguardian-harness.apk"
PKG="org.homeguardian.harness"

while [ $# -gt 0 ]; do
    case "$1" in
        --serial) SERIAL="${2:-}"; shift 2 ;;
        --apk)    APK="${2:-}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

rc=0
say()  { printf '  %s\n' "$*"; }
ok()   { printf '  OK   %s\n' "$*"; }
bad()  { printf '  FAIL %s\n' "$*"; rc=3; }
warn() { printf '  WARN %s\n' "$*"; }

echo "== physical test preflight (READ-ONLY) =="

# --- self-check: this script must not contain a runnable mutating command -------------------------
# Scoped to what EXECUTES: comment lines and the `cat <<'EOF'` block (which only PRINTS the commands)
# are removed first, so the check sees code, not prose about code.
#
# The pattern matches a mutating verb in COMMAND POSITION, not the bare word. Three earlier versions
# of this guard were wrong, and each is worth stating because each was a different mistake:
#   1. matching the literal string "adb" missed `"$ADB" install ...` and reported "no runnable
#      install" while a runnable install was present. Match the SUBCOMMAND, not the binary's spelling.
#   2. scanning the whole file flagged the heredoc that merely documents the commands. Strip bodies.
#   3. matching the verb anywhere on a runnable line flagged this script's own MESSAGES - "the APK
#      cannot install here", "an uninstall (which deletes app data)" - so the script refused to run
#      at all. A guard that cannot pass is as useless as one that cannot fail. Require command
#      position: start of a statement, or immediately after an adb token.
SELF="${BASH_SOURCE[0]}"
RUNNABLE=$(awk '
    /^[[:space:]]*#/ { next }                     # comments do not run
    /<<-?[[:space:]]*['"'"'"]?EOF/ { inheredoc=1; next }
    inheredoc && /^[[:space:]]*EOF[[:space:]]*$/ { inheredoc=0; next }
    inheredoc { next }                             # heredoc bodies are printed, not executed
    { print }
' "$SELF")

MUTATE='(^|[;&|])[[:space:]]*("[^"]*[Aa][Dd][Bb][^"]*"|\$\{?ADB\}?|adb)[^;&|]*[[:space:]](install|uninstall|push|reboot)([[:space:]]|$)|(^|[;&|])[[:space:]]*(install|uninstall|push|reboot)([[:space:]]|$)'

if printf '%s\n' "$RUNNABLE" | grep -nE "$MUTATE" >/dev/null; then
    bad "this script contains a runnable device-mutating command — refusing to continue"
    printf '%s\n' "$RUNNABLE" | grep -nE "$MUTATE" | sed 's/^/       /'
    exit 3
fi
ok "self-check: no runnable install/uninstall/push/reboot command outside the printed block"

# --- toolchain -------------------------------------------------------------------------------------
[ -x "$ADB" ] || { echo "  FATAL adb not executable at $ADB" >&2; exit 2; }
[ -x "$BT/aapt" ] || { echo "  FATAL aapt not executable at $BT" >&2; exit 2; }
ok "toolchain present (adb, aapt)"

# --- artifact --------------------------------------------------------------------------------------
if [ ! -f "$APK" ]; then
    bad "artifact missing: $APK  (build it first: apk/build_apk.sh)"
else
    size=$(stat -c%s "$APK")
    if [ "$size" -lt 100000 ]; then
        bad "artifact is $size bytes — too small to be the harness; a hash of this is meaningless"
    else
        ok "artifact: $(basename "$APK")  ${size} bytes  sha256 $(sha256sum "$APK" | cut -d' ' -f1)"
    fi
fi

# --- artifact identity: package, sdk bounds, signer -------------------------------------------------
art_pkg=$("$BT/aapt" dump badging "$APK" 2>/dev/null | grep -oP "(?<=^package: name=')[^']+")
art_minsdk=$("$BT/aapt" dump badging "$APK" 2>/dev/null | grep -oP "(?<=^sdkVersion:')[0-9]+")
art_tgtsdk=$("$BT/aapt" dump badging "$APK" 2>/dev/null | grep -oP "(?<=targetSdkVersion:')[0-9]+")
art_signer=$("$BT/apksigner" verify --print-certs "$APK" 2>/dev/null | grep -i 'SHA-256 digest' | head -1 | awk '{print $NF}')

say "artifact package : $art_pkg   minSdk ${art_minsdk:-?}  targetSdk ${art_tgtsdk:-?}"
say "artifact signer  : ${art_signer:-<unreadable>}"

if [ "$art_pkg" != "$PKG" ]; then
    bad "artifact package '$art_pkg' is not '$PKG' — wrong artifact for this test"
fi

# --- device ----------------------------------------------------------------------------------------
if [ -z "$SERIAL" ]; then
    mapfile -t devs < <("$ADB" devices | awk 'NR>1 && $2=="device" {print $1}')
    if [ "${#devs[@]}" -eq 0 ]; then
        bad "no device in 'device' state; nothing attached"
    elif [ "${#devs[@]}" -gt 1 ]; then
        bad "multiple devices attached (${devs[*]}) — pass --serial to name the designated phone"
    else
        SERIAL="${devs[0]}"
        ok "device auto-selected: $SERIAL"
    fi
fi

if [ -n "$SERIAL" ]; then
    state=$("$ADB" -s "$SERIAL" get-state 2>&1 | tr -d '\r')
    if [ "$state" != "device" ]; then
        bad "device $SERIAL is not ready: $state"
    else
        model=$("$ADB" -s "$SERIAL" shell getprop ro.product.model 2>/dev/null | tr -d '\r')
        api=$("$ADB" -s "$SERIAL" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')
        abi=$("$ADB" -s "$SERIAL" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')
        ok "device: $model  API $api  $abi"

        # API vs minSdk
        if [ -n "$art_minsdk" ] && [ -n "$api" ] && [ "$api" -lt "$art_minsdk" ]; then
            bad "device API $api < artifact minSdk $art_minsdk — the APK cannot install here"
        else
            ok "device API $api >= minSdk ${art_minsdk:-?}"
        fi

        # targetSdk vs the platform's install floor (Android 11 refuses targetSdk < 23)
        if [ -n "$art_tgtsdk" ] && [ -n "$api" ] && [ "$api" -ge 30 ] && [ "$art_tgtsdk" -lt 23 ]; then
            bad "device API $api refuses targetSdk $art_tgtsdk (< 23)"
        fi

        # abi
        case "$abi" in
            arm64-v8a|armeabi-v7a) ok "abi supported by the bundled native library" ;;
            *) warn "device abi '$abi' — confirm the .so matches before testing" ;;
        esac

        # --- the signing question, answered from the DEVICE, not from memory ------------------------
        # A signature conflict only exists between the SAME package. Comparing the signer of a
        # DIFFERENT installed package against this artifact proves nothing, and reading a short digest
        # out of `dumpsys` is not a comparison — `dumpsys` prints an abbreviated rendering.
        existing_path=$("$ADB" -s "$SERIAL" shell pm path "$PKG" 2>/dev/null | tr -d '\r' | sed 's/^package://' | head -1)
        if [ -z "$existing_path" ]; then
            ok "'$PKG' is not installed on this device — this would be a fresh install, so no"
            say  "     upgrade-signature check applies. (It IS already installed on the Panasonic.)"
        else
            say "'$PKG' is already installed at $existing_path"
            tmp=$(mktemp -d)
            if "$ADB" -s "$SERIAL" pull "$existing_path" "$tmp/existing.apk" >/dev/null 2>&1; then
                inst_signer=$("$BT/apksigner" verify --print-certs "$tmp/existing.apk" 2>/dev/null \
                              | grep -i 'SHA-256 digest' | head -1 | awk '{print $NF}')
                if [ -n "$inst_signer" ] && [ "$inst_signer" = "$art_signer" ]; then
                    ok "installed signer MATCHES the artifact — an in-place update is signature-valid"
                elif [ -n "$inst_signer" ]; then
                    bad "installed signer $inst_signer != artifact $art_signer — an update would need"
                    say  "     the installed package's keystore, or an uninstall (which deletes app data)"
                else
                    bad "could not read the installed signer — do not conclude either way"
                fi
            else
                bad "could not pull the installed APK — cannot verify the signature relationship"
            fi
            rm -rf "$tmp"
        fi

        # --- permissions the test will need, and whether they are currently granted ----------------
        for perm in CAMERA RECORD_AUDIO; do
            g=$(command -v aapt >/dev/null 2>&1; :)
            if [ -n "$existing_path" ]; then
                st=$("$ADB" -s "$SERIAL" shell dumpsys package "$PKG" 2>/dev/null | tr -d '\r' \
                     | grep -A2 "android.permission.$perm" | grep -oE 'granted=[a-z]+' | head -1)
                say "permission $perm: ${st:-not reported (app may not be installed, or not requested)}"
            fi
        done
    fi
fi

# --- the Panasonic, when it is the designated device -------------------------------------------------
say "designated devices known to this lane:"
say "  Panasonic ELUGA I7  LRW86SC6RO8TZ99H   Android 9 / API 28   (harness already installed)"
say "  Xiaomi Mi A3        edd53dbd04de       Android 11 / API 30  (harness NOT installed)"
if ! "$ADB" devices | awk 'NR>1{print $1}' | grep -q '^LRW86SC6RO8TZ99H$'; then
    warn "the Panasonic is NOT on the ADB transport right now"
fi

echo
echo "== the commands this preflight did NOT run =="
cat <<EOF
  # 1. install (PROHIBITED until the owner records authorization)
  $ADB -s ${SERIAL:-<serial>} install -r "$APK"

  # 2. grant the runtime permissions the test needs, AFTER the on-screen consent prompt
  $ADB -s ${SERIAL:-<serial>} shell pm grant $PKG android.permission.CAMERA
  $ADB -s ${SERIAL:-<serial>} shell pm grant $PKG android.permission.RECORD_AUDIO

  # 3. start capture (PROHIBITED: activates the camera) — owner consent must be present FIRST
  $ADB -s ${SERIAL:-<serial>} shell am start -n $PKG/.MainActivity

  # 4. the evidence to capture for the test record
  $ADB -s ${SERIAL:-<serial>} shell dumpsys activity services $PKG   # isForeground, service type
EOF
echo

if [ "$rc" -eq 0 ]; then
    echo "  GO — every precondition met. The remaining step needs owner authorization to run."
else
    echo "  NO-GO — a precondition failed; see the FAIL lines above."
fi
exit "$rc"
