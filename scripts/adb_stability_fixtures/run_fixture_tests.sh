#!/usr/bin/env bash
# run_fixture_tests.sh — exercise adb_stability_preflight.sh against synthetic USB/ADB
# environments, so every verdict path is proven WITHOUT depending on real hardware.
#
# This exists because the Mi A3's link is unstable: a tool whose only test is "run it on the
# flaky phone" is untested in exactly the case that matters. Each fixture supplies its own
# sysfs tree, kernel-log file and fake `adb`, injected through the preflight's documented
# HG_* knobs. No real device is touched.
#
#   Fixture            Scenario                       Expected verdict
#   none               no Mi A3 connected             BLOCKED (exit 3)
#   panasonic_only     only the Panasonic ELUGA       BLOCKED (exit 3), never treated as target
#   flapping           Mi A3 repeatedly disconnecting FAIL    (exit 1)
#   stable             Mi A3 stable for the window    PASS    (exit 0)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFLIGHT="$HERE/../adb_stability_preflight.sh"
WORK="${HG_FIXTURE_WORK:-$(mktemp -d)}"

pass=0; fail=0
note() { printf '  %s\n' "$*"; }
verdict_ok() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "$1" "$2"
  else fail=$((fail+1)); printf '  [FAIL] %-16s got=%s want=%s\n' "$1" "$2" "$3"; fi
}
verdict_is() {
  if [ "$2" = "$3" ]; then pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "$1" "$2"
  else fail=$((fail+1)); printf '  [FAIL] %-16s got=%s want=%s\n' "$1" "$2" "$3"; fi
}

[ -f "$PREFLIGHT" ] || { echo "preflight not found at $PREFLIGHT"; exit 2; }
echo "preflight : $PREFLIGHT"
echo "workspace : $WORK"

# ---------------------------------------------------------------- fixture builder
make_fixture() {
  local name="$1" port="$2" vendor="$3" product="$4" present="$5" churn="$6" android="$7" api="$8" devcodename="$9"
  local d="$WORK/$name"
  mkdir -p "$d/sys/bus/usb/devices" "$d/bin"
  if [ "$port" != "none" ]; then
    mkdir -p "$d/sys/bus/usb/devices/$port"
    printf '%s\n' "$vendor"   > "$d/sys/bus/usb/devices/$port/idVendor"
    printf 'ff08\n'           > "$d/sys/bus/usb/devices/$port/idProduct"
    printf '%s\n' "$product"  > "$d/sys/bus/usb/devices/$port/product"
    printf '50\n'             > "$d/sys/bus/usb/devices/$port/devnum"
  fi
  : > "$d/klog.txt"

  # fake adb — parameterised by the fixture's env file
  cat > "$d/bin/adb" <<'FAKE'
#!/usr/bin/env bash
S="${FAKE_SERIAL:-SERIAL_REDACTED}"
PORT="${FAKE_PORT:-2-8}"
emit_churn() {
  [ "${FAKE_CHURN:-0}" = "1" ] || return 0
  [ -n "${FAKE_KLOG:-}" ] || return 0
  printf 'host kernel: usb %s: USB disconnect, device number %s\n' "$PORT" "$((9000+RANDOM))" >> "$FAKE_KLOG"
  printf 'host kernel: usb %s: new high-speed USB device number %s using xhci_hcd\n' "$PORT" "$((9000+RANDOM))" >> "$FAKE_KLOG"
}
case "${1:-}" in
  devices)
    echo "List of devices attached"
    if [ "${FAKE_PRESENT:-1}" = "1" ]; then
      if [ "${2:-}" = "-l" ]; then
        echo "$S	device usb:$PORT product:${FAKE_DEVICE:-laurel_sprout} model:Mi_A3 device:${FAKE_DEVICE:-laurel_sprout}"
      else
        echo "$S	device"
      fi
    fi
    emit_churn
    ;;
  -s)
    if [ "${3:-}" = "shell" ] && [ "${4:-}" = "getprop" ]; then
      case "${5:-}" in
        ro.build.version.release) echo "${FAKE_ANDROID:-11}" ;;
        ro.build.version.sdk)     echo "${FAKE_API:-30}" ;;
        ro.product.device)        echo "${FAKE_DEVICE:-laurel_sprout}" ;;
        ro.product.model)         echo "${FAKE_MODEL:-Mi A3}" ;;
      esac
    fi
    ;;
  version) echo "Android Debug Bridge version 1.0.41 (fixture)" ;;
esac
exit 0
FAKE
  chmod +x "$d/bin/adb"

  cat > "$d/env" <<EOF
export HG_SYSFS_ROOT="$d/sys"
export HG_KLOG="$d/klog.txt"
export HG_ADB="$d/bin/adb"
export HG_WINDOW="8"
export HG_POLL="2"
export FAKE_SERIAL="$name-serial-do-not-print"
export FAKE_PORT="$port"
export FAKE_PRESENT="$present"
export FAKE_CHURN="$churn"
export FAKE_KLOG="$d/klog.txt"
export FAKE_ANDROID="$android"
export FAKE_API="$api"
export FAKE_DEVICE="$devcodename"
EOF
  echo "$d"
}

echo
echo "=== building fixtures ==="
F_NONE=$(make_fixture none          none  ""     ""          0 0 11 30 laurel_sprout)
F_PAN=$(make_fixture panasonic_only 2-2  0e8d   ELUGA_I7_EE 0 0 11 30 laurel_sprout)
F_FLAP=$(make_fixture flapping      2-8  2717   "Mi A3"     1 1 11 30 laurel_sprout)
F_STAB=$(make_fixture stable        2-8  2717   "Mi A3"     1 0 11 30 laurel_sprout)
note "none / panasonic_only / flapping / stable created"

run() {
  local d="$1" out
  # shellcheck disable=SC1090
  ( . "$d/env"; bash "$PREFLIGHT" ) >"$d/out.txt" 2>&1
  echo $?
}

echo
echo "=== fixture results ==="

echo "--- fixture: none (no Mi A3 connected) ---"
rc=$(run "$F_NONE"); verdict_is "none" "$rc" "3"
grep -q 'VERDICT: BLOCKED' "$F_NONE/out.txt" && { pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "none-verdict" "BLOCKED"; } || { fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "none-verdict" "no BLOCKED line"; }

echo "--- fixture: panasonic_only (must NOT be mistaken for the target) ---"
rc=$(run "$F_PAN"); verdict_is "panasonic_only" "$rc" "3"
grep -qi 'MediaTek' "$F_PAN/out.txt" && { pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "panasonic-ident" "identified as MediaTek, ignored"; } || { fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "panasonic-ident" "vendor not called out"; }
if grep -q 'Mi A3 CANDIDATE' "$F_PAN/out.txt"; then fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "panasonic-trap" "Panasonic was treated as the target"; else pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "panasonic-trap" "never treated as target"; fi

echo "--- fixture: flapping (Mi A3 repeatedly disconnecting) ---"
rc=$(run "$F_FLAP"); verdict_is "flapping" "$rc" "1"
grep -q 'VERDICT' "$F_FLAP/out.txt" && grep -q 'FAIL' "$F_FLAP/out.txt" && { pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "flapping-verdict" "FAIL"; } || { fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "flapping-verdict" "expected FAIL"; }

echo "--- fixture: stable (Mi A3 stable for the window) ---"
rc=$(run "$F_STAB"); verdict_is "stable" "$rc" "0"
grep -q 'PASS — Mi A3 stable' "$F_STAB/out.txt" && { pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "stable-verdict" "PASS"; } || { fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "stable-verdict" "no PASS line"; }

echo "--- cross-cutting: serials must never be printed ---"
leak=0
for d in "$F_NONE" "$F_PAN" "$F_FLAP" "$F_STAB"; do
  grep -q 'do-not-print' "$d/out.txt" && { leak=1; echo "  leak in $(basename "$d")"; }
done
if [ "$leak" -eq 0 ]; then pass=$((pass+1)); printf '  [PASS] %-16s %s\n' "redaction" "no serial in any output"; else fail=$((fail+1)); printf '  [FAIL] %-16s %s\n' "redaction" "serial leaked"; fi

echo
echo "=== fixtures: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] || exit 1
