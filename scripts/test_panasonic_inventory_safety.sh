#!/usr/bin/env bash
# Safety-property tests for scripts/panasonic_pkg_inventory.sh, runnable with NO device attached.
#
# Why this exists: the inventory tool's value is entirely in its refusal behaviour. It must
# never act on the wrong phone, never proceed when the target is absent, and never be able to
# uninstall anything. Those properties are testable offline by putting a stub `adb` first on
# PATH and driving the real script through scenarios a real device cannot easily produce
# (an offline target, a torn read, malformed output).
#
# This tests the REAL script file - it does not reimplement its logic, so a future edit that
# weakens the guard will fail these tests.
#
# Usage: test_panasonic_inventory_safety.sh
# Exit: 0 all properties hold, 1 one or more failed.

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="$HERE/panasonic_pkg_inventory.sh"
[[ -f "$TARGET" ]] || { echo "cannot find $TARGET"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/bin"

# ------------------------------------------------------------------ stub adb
cat > "$WORK/bin/adb" <<'STUB'
#!/usr/bin/env bash
# Scenario-driven stub. HG_TEST_SCENARIO selects the behaviour.
S="${HG_TEST_SCENARIO:-ok}"
SERIAL="<DEVICE_SERIAL>"

if [[ "${1:-}" == "devices" ]]; then
  echo "List of devices attached"
  case "$S" in
    absent)          : ;;
    mia3_only)       printf '%s\tdevice product:laurel_sprout model:Mi_A3 device:laurel_sprout\n' "<LAN_IP_MI_A3>:40815" ;;
    offline_target)  printf '%s\toffline\n' "$SERIAL" ;;
    ok|empty|malformed|unknown|ok_with_mia3)
                     printf '%s\tdevice product:ELUGA_I7_EE model:ELUGA_I7_EE\n' "$SERIAL"
                     [[ "$S" == "ok_with_mia3" ]] && printf '%s\tdevice product:laurel_sprout model:Mi_A3\n' "<LAN_IP_MI_A3>:40815"
                     ;;
  esac
  exit 0
fi

# Everything else is 'adb -s SERIAL shell <cmd>'; the command is the last argument.
cmd="${*: -1}"
case "$S" in
  mia3_only)  echo "adb: device '$SERIAL' not found" >&2; exit 1 ;;
esac
case "$cmd" in
  *getprop*)
    case "$S" in
      empty)     : ;;
      malformed) printf '%%% not-actually-properties\n\x00\n' ;;
      *)         printf 'ELUGA_I7_EE\nk39tv1_64_a27\narm64-v8a\n' ;;
    esac ;;
  *"pm list packages -3"*)
    case "$S" in
      empty)     : ;;
      malformed) printf 'noise with no package prefix\n' ;;
      unknown)   printf 'package:com.totally.unknown.app\npackage:com.whatsapp\npackage:com.facebook.katana\npackage:com.facebook.orca\n' ;;
      *)         printf 'package:com.totally.unknown.app\n' ;;
    esac ;;
  *"pm path"*)       echo "package:/data/app/com.x-y/base.apk" ;;
  *"ls -l"*)         echo "-rw-r--r-- 1 root root 123456 2026-01-01 00:00 /data/app/com.x-y/base.apk" ;;
  *"df -h /data"*)   printf 'Filesystem Size Used Avail Use%% Mounted on\n/dev/block/dm-2 10G 8.5G 975M 90%% /data\n' ;;
  *"dumpsys battery"*) printf '  level: 100\n  health: 2\n  status: 2\n  temperature: 340\n' ;;
  *)                 : ;;
esac
exit 0
STUB
chmod +x "$WORK/bin/adb"

PASS=0; FAIL=0
check() { # name expected_exit actual_exit extra
  local name="$1" want="$2" got="$3" extra="${4:-}"
  if [[ "$want" == "$got" ]]; then
    printf '  PASS  %-58s exit=%s %s\n' "$name" "$got" "$extra"; PASS=$((PASS+1))
  else
    printf '  FAIL  %-58s exit=%s (wanted %s) %s\n' "$name" "$got" "$want" "$extra"; FAIL=$((FAIL+1))
  fi
}

run() { # scenario -> sets OUT, RET
  OUT=$(PATH="$WORK/bin:$PATH" HG_TEST_SCENARIO="$1" bash "$TARGET" 2>&1); RET=$?
}

echo "=== P1. target absent -> must fail closed, must not fall back ==="
run absent
check "absent target refuses" 3 "$RET" "$(grep -ic 'not falling back' <<<"$OUT" | sed 's/^/nofallback_msgs=/')"
grep -q "<LAN_IP_MI_A3>" <<<"$OUT" && echo "        (correctly reports the visible device list)"
grep -qiE 'uninstall|disable|pm clear|wipe' <<<"$OUT" && { echo "  FAIL  absence path mentioned a destructive verb"; FAIL=$((FAIL+1)); }

echo
echo "=== P2. ONLY the Mi A3 connected -> must refuse, never substitute it ==="
run mia3_only
check "Mi-A3-only refuses" 3 "$RET"
grep -q "<LAN_IP_MI_A3>" <<<"$OUT" && echo "        (shows the Mi A3 as visible so the reason is obvious)"

echo
echo "=== P3. target present but OFFLINE -> must refuse ==="
run offline_target
check "offline target refuses" 3 "$RET"

echo
echo "=== P4. wrong MODEL on the target serial -> identity mismatch, refuse ==="
cat > "$WORK/bin/adb" <<'STUB2'
#!/usr/bin/env bash
if [[ "${1:-}" == "devices" ]]; then
  echo "List of devices attached"; printf '<DEVICE_SERIAL>\tdevice\n'; exit 0
fi
printf 'Mi_A3\nlaurel_sprout\narm64-v8a\n'   # a Mi A3 masquerading under the target serial
exit 0
STUB2
chmod +x "$WORK/bin/adb"
OUT=$(PATH="$WORK/bin:$PATH" bash "$TARGET" 2>&1); RET=$?
check "wrong model refused" 2 "$RET" "$(grep -o 'identity mismatch' <<<"$OUT")"
echo "        message: $(head -1 <<<"$OUT")"

echo
echo "=== P5. malformed property output -> must not crash or proceed ==="
cp "$WORK/bin/adb" "$WORK/bin/adb.bak"; cat > "$WORK/bin/adb" <<'STUB3'
#!/usr/bin/env bash
S="${HG_TEST_SCENARIO:-ok}"
if [[ "${1:-}" == "devices" ]]; then
  echo "List of devices attached"; printf '<DEVICE_SERIAL>\tdevice\n'; exit 0
fi
cmd="${*: -1}"
case "$cmd" in
  *getprop*) printf '%%% not-actually-properties\n\x00\n' ;;
  *) : ;;
esac
exit 0
STUB3
chmod +x "$WORK/bin/adb"
OUT=$(PATH="$WORK/bin:$PATH" bash "$TARGET" 2>&1); RET=$?
check "malformed getprop refused" 2 "$RET" "(must not be read as a valid identity)"

echo
echo "=== P6. empty package list -> completes cleanly, no destructive act ==="
cat > "$WORK/bin/adb" <<'STUB4'
#!/usr/bin/env bash
if [[ "${1:-}" == "devices" ]]; then
  echo "List of devices attached"; printf '<DEVICE_SERIAL>\tdevice\n'; exit 0
fi
cmd="${*: -1}"
case "$cmd" in
  *getprop*) printf 'ELUGA_I7_EE\nk39tv1_64_a27\narm64-v8a\n' ;;
  *"pm list packages -3"*) : ;;
  *"df -h /data"*) printf 'Filesystem Size Used Avail Use%% Mounted on\n/dev/block/dm-2 10G 8.5G 975M 90%% /data\n' ;;
  *"dumpsys battery"*) printf '  level: 100\n' ;;
  *) : ;;
esac
exit 0
STUB4
chmod +x "$WORK/bin/adb"
OUT=$(PATH="$WORK/bin:$PATH" bash "$TARGET" 2>&1); RET=$?
check "empty package list tolerated" 0 "$RET" "$(grep -o 'total third-party packages: 0' <<<"$OUT")"

echo
echo "=== P7. positive control: correct device -> inventory runs ==="
cat > "$WORK/bin/adb" <<'STUB5'
#!/usr/bin/env bash
if [[ "${1:-}" == "devices" ]]; then
  echo "List of devices attached"; printf '<DEVICE_SERIAL>\tdevice\n'; exit 0
fi
cmd="${*: -1}"
case "$cmd" in
  *getprop*) printf 'ELUGA_I7_EE\nk39tv1_64_a27\narm64-v8a\n' ;;
  *"pm list packages -3"*) printf 'package:com.whatsapp\npackage:com.facebook.katana\npackage:com.facebook.orca\npackage:com.totally.unknown.app\n' ;;
  *"pm path"*) echo "package:/data/app/com.x-y/base.apk" ;;
  *"ls -l"*) echo "-rw-r--r-- 1 root root 123456 2026-01-01 00:00 /data/app/com.x-y/base.apk" ;;
  *"df -h /data"*) printf 'Filesystem Size Used Avail Use%% Mounted on\n/dev/block/dm-2 10G 8.5G 975M 90%% /data\n' ;;
  *"dumpsys battery"*) printf '  level: 100\n  health: 2\n  status: 2\n  temperature: 340\n' ;;
  *) : ;;
esac
exit 0
STUB5
chmod +x "$WORK/bin/adb"
OUT=$(PATH="$WORK/bin:$PATH" bash "$TARGET" 2>&1); RET=$?
check "correct device proceeds" 0 "$RET"
echo "        whatsapp classified  : $(grep -c 'com.whatsapp.*CONFIRM' <<<"$OUT")"
echo "        facebook  classified  : $(grep -cE 'com.facebook.(katana|orca).*CONFIRM' <<<"$OUT")"
echo "        unknown   classified  : $(grep -c 'totally.unknown.app.*review' <<<"$OUT")"

echo
echo "=== P8. static: no destructive verb can be reached from the tool ==="
if grep -nE 'pm +(uninstall|disable-user|clear|disable)|adb .*uninstall|\brm +-|mkfs|fastboot|reboot' "$TARGET" | grep -v '^\s*#' | grep -qv 'never uninstalls'; then
  echo "  FAIL  destructive command found in $TARGET"; FAIL=$((FAIL+1))
  grep -nE 'pm +(uninstall|disable-user|clear|disable)|adb .*uninstall|mkfs|fastboot|reboot' "$TARGET" | sed 's/^/        /'
else
  echo "  PASS  no uninstall / disable / clear / wipe / reset command present"; PASS=$((PASS+1))
fi
echo "  commands the tool actually issues:"
grep -oE "A shell '[^']+'|A shell \"[^\"]+\"" "$TARGET" | sed 's/^/        /'

echo
echo "=== summary: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]] || exit 1
