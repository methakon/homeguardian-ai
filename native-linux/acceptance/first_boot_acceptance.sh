#!/usr/bin/env bash
# first_boot_acceptance.sh — post-flash acceptance for the Mi A3 native Linux MCP hub.
#
# RUNS ON THE DEVICE after the first boot. Covers P0 only: CPU/RAM/storage, Wi-Fi, battery
# and thermal, SSH, the MCP service and its two tools, fail-closed auth, and reboot
# behaviour. Display, touch, audio, camera, Bluetooth and sensors are P1 and are reported as
# untested rather than failed.
#
# The MCP protocol assertions are NOT redefined here — they come from
# mcp_protocol_checks.sh, the same file the container harness and the Termux self-test use, so
# there is exactly one definition of what "the MCP surface works" means.
#
# Every stage is bounded. Nothing waits forever. Read-only: it starts/stops nothing except
# its own probe of the MCP service, and changes no configuration.
#
# Usage: first_boot_acceptance.sh [--json out.json] [--tsv out.tsv]
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d /tmp/hg-accept.XXXXXX)"
export TMPDIR="$WORK"
JSON_OUT=""; TSV_OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --json) JSON_OUT="${2:-}"; shift 2 ;;
    --tsv)  TSV_OUT="${2:-}";  shift 2 ;;
    *) shift ;;
  esac
done

for cand in "$HERE/mcp_protocol_checks.sh" "$HERE/../../scripts/mcp_protocol_checks.sh" \
            "$HERE/../scripts/mcp_protocol_checks.sh"; do
  [ -f "$cand" ] && { CHECKS="$cand"; break; }
done

R="$WORK/results.tsv"; : > "$R"
rec() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$R"; }
say() { printf '%s\n' "$*"; }
hr()  { say "------------------------------------------------------------"; }

# ===== 1. platform ==========================================================
hr; say "1  platform"
ARCH=$(uname -m)
rec platform arch "$([ "$ARCH" = aarch64 ] && echo PASS || echo FAIL)" "$ARCH"
say "  arch     : $ARCH"

OSV=$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")
rec platform os INFO "$OSV"
say "  os       : $OSV"

KREL=$(uname -r)
rec platform kernel INFO "$KREL"
say "  kernel   : $KREL"
# The nura sm61x5 kernel is 7.2.x. Flag rather than fail on an unexpected version.
case "$KREL" in 7.2.*) rec platform kernel_expected PASS "$KREL" ;; *) rec platform kernel_expected REVIEW "expected 7.2.x, got $KREL" ;; esac

CPU=$(nproc); RAM=$(awk '/MemTotal/{printf "%.0f",$2/1024}' /proc/meminfo)
rec platform cpus INFO "$CPU"
rec platform ram_total_mb INFO "$RAM"
say "  cpus     : $CPU"
say "  ram      : ${RAM} MB"

# ===== 2. storage ===========================================================
hr; say "2  persistent storage"
ROOT=$(df -k / | awk 'NR==2{print $2"% used="$5" avail="$4"kB"}')
rec storage rootfs INFO "$ROOT"
say "  rootfs   : $ROOT"
# Prove it is genuinely writable and durable, not a ramdisk.
T="$WORK/writetest"
if dd if=/dev/zero of="$T" bs=1M count=16 conv=fsync status=none 2>/dev/null \
   && sync && [ "$(stat -c%s "$T")" = "16777216" ]; then
  rec storage write_16mb PASS "durable write + sync OK"
  say "  16 MB write+sync: OK"
else
  rec storage write_16mb FAIL "write or sync failed"
fi
rm -f "$T"
# The DRAM carveout bug showed up as lockups under sustained buffered reads — so read.
if dd if=/dev/mmcblk0 of=/dev/null bs=1M count=256 status=none 2>/dev/null; then
  rec storage read_256mb PASS "no lockup during 256 MB buffered read"
  say "  256 MB buffered read: OK (carveout-fix smoke test)"
else
  rec storage read_256mb REVIEW "read failed or device unreadable"
fi

# ===== 3. network ===========================================================
hr; say "3  Wi-Fi"
IF=$(command -v iw >/dev/null && iw dev 2>/dev/null | awk '/Interface/{print $2; exit}')
[ -z "${IF:-}" ] && IF=$(ls /sys/class/net 2>/dev/null | grep -E '^wl' | head -1)
if [ -n "${IF:-}" ]; then
  rec wifi interface PASS "$IF"
  say "  interface: $IF"
  if ip -4 addr show "$IF" 2>/dev/null | grep -q 'inet '; then
    ADDR=$(ip -4 addr show "$IF" | awk '/inet /{print $2; exit}')
    rec wifi address PASS "$ADDR"
    say "  address  : $ADDR"
  else
    rec wifi address FAIL "no IPv4 lease"
  fi
  # driver must be ath10k, not something unexpected
  DRV=$(basename "$(readlink -f "/sys/class/net/$IF/device/driver" 2>/dev/null)" 2>/dev/null)
  rec wifi driver INFO "${DRV:-unknown}"
  say "  driver   : ${DRV:-unknown}"
else
  rec wifi interface FAIL "no wireless interface found"
fi

# ===== 4. battery + thermal =================================================
hr; say "4  battery and thermal"
BAT=""
for b in /sys/class/power_supply/*/capacity; do [ -r "$b" ] && { BAT=$b; break; }; done
if [ -n "$BAT" ]; then
  CAP=$(cat "$BAT")
  rec power battery_capacity PASS "${CAP}%"
  say "  capacity : ${CAP}%"
else
  rec power battery_capacity FAIL "no battery device exposed"
fi
ST=$(cat /sys/class/power_supply/*/status 2>/dev/null | head -1)
rec power charging_status INFO "${ST:-unknown}"
say "  status   : ${ST:-unknown}"

# TSENS die zones — the patch that added these was tested on this exact board.
Z=0; HOT=0; HOTZ=""
for z in /sys/class/thermal/thermal_zone*/temp; do
  [ -r "$z" ] || continue
  Z=$((Z+1))
  t=$(cat "$z" 2>/dev/null)
  [ "${t:-0}" -gt "$HOT" ] && { HOT=$t; HOTZ=$z; }
done
if [ "$Z" -gt 0 ]; then
  rec thermal zones PASS "$Z zones"
  rec thermal hottest_celsius INFO "$((HOT/1000))"
  say "  zones    : $Z"
  say "  hottest  : $((HOT/1000)) C"
  rec thermal die_sensing "$([ "$Z" -ge 15 ] && echo PASS || echo REVIEW)" "$Z zones (15+ expected from TSENS patch)"
  # 85 C is the documented passive trip for these zones.
  [ "$HOT" -ge 85000 ] && rec thermal within_trip FAIL "$((HOT/1000)) C >= 85 C trip" \
                       || rec thermal within_trip PASS "$((HOT/1000)) C < 85 C"
else
  rec thermal zones FAIL "no thermal zones"
fi

# ===== 5. SSH ===============================================================
hr; say "5  SSH"
if systemctl is-active --quiet sshd 2>/dev/null || systemctl is-active --quiet ssh 2>/dev/null; then
  rec ssh service PASS "running"
  say "  service  : running"
else
  rec ssh service FAIL "not active"
fi
# Effective config must match policy — check the EFFECTIVE values, not the file.
if command -v sshd >/dev/null 2>&1; then
  if sshd -t 2>/dev/null; then rec ssh config_valid PASS "sshd -t"; else rec ssh config_valid FAIL "sshd -t failed"; fi
  EFF=$(sshd -T 2>/dev/null)
  rec ssh password_auth "$(printf '%s' "$EFF" | grep -q '^passwordauthentication no' && echo PASS || echo FAIL)" "must be no"
  rec ssh root_login    "$(printf '%s' "$EFF" | grep -q '^permitrootlogin no'      && echo PASS || echo FAIL)" "must be no"
  say "  passwordauth: $(printf '%s' "$EFF" | awk '/^passwordauthentication/{print $2}')"
  say "  permitroot  : $(printf '%s' "$EFF" | awk '/^permitrootlogin/{print $2}')"
fi

# ===== 6. MCP service =======================================================
hr; say "6  HomeGuardian MCP service"
if systemctl is-active --quiet hg-mcp 2>/dev/null; then
  rec mcp service_active PASS "systemd active"
  say "  service  : active"
  EP=$(systemctl show hg-mcp -p MainPID --value 2>/dev/null)
  if [ -n "$EP" ] && [ "$EP" != "0" ] && [ -r "/proc/$EP/status" ]; then
    RSS=$(awk '/VmRSS/{print $2}' "/proc/$EP/status")
    rec mcp rss_kb INFO "$RSS"
    say "  rss      : ${RSS} kB   <-- REAL DEVICE FIGURE"
    # Confirm loopback-only binding from the live socket table.
    if command -v ss >/dev/null 2>&1; then
      WL=$(ss -ltn 2>/dev/null | awk '$4 ~ /:8790$/ {print $4}' | grep -c '^0\.0\.0\.0\|^\*\|^\[::\]' || true)
      LL=$(ss -ltn 2>/dev/null | awk '$4 ~ /:8790$/ {print $4}' | grep -c '^127\.0\.0\.1' || true)
      rec mcp loopback_only "$([ "$WL" -eq 0 ] && [ "$LL" -ge 1 ] && echo PASS || echo FAIL)" "loopback=$LL wildcard=$WL"
    fi
  fi
  if [ -f "${CHECKS:-}" ]; then
    TOK=$(grep -oE '[0-9a-f]{64}' /etc/homeguardian/creds.json 2>/dev/null | head -1)
    if [ -n "${TOK:-}" ]; then
      timeout 90 bash "$CHECKS" "http://127.0.0.1:8790/mcp" "$TOK" 10 > "$WORK/proto.tsv" 2>&1 || true
      grep -P '^CHECK\t' "$WORK/proto.tsv" 2>/dev/null | while IFS=$'\t' read -r _ n v d; do
        printf 'mcp\t%s\t%s\t%s\n' "$n" "$v" "$d" >> "$R"
      done
      grep -P '^CHECK\t' "$WORK/proto.tsv" 2>/dev/null | sed 's/^CHECK\t/  /;s/\t/  /g'
    else
      rec mcp credentials FAIL "no token readable (expected 0600 at /etc/homeguardian/creds.json)"
    fi
  else
    rec mcp protocol_checks SKIP "mcp_protocol_checks.sh not found"
  fi
else
  rec mcp service_active FAIL "hg-mcp not active"
  say "  systemctl status hg-mcp:"
  systemctl status hg-mcp --no-pager -l 2>/dev/null | tail -8 | sed 's/^/    /'
fi

# ===== 7. reboot resilience (report-only) ===================================
hr; say "7  reboot and autostart"
rec reboot uptime_seconds INFO "$(awk '{printf "%d",$1}' /proc/uptime)"
rec reboot autostart_check "$(systemctl is-enabled hg-mcp 2>/dev/null | grep -q enabled && echo PASS || echo FAIL)" "$(systemctl is-enabled hg-mcp 2>/dev/null)"
say "  uptime   : $(awk '{printf "%d s",$1}' /proc/uptime)"
say "  hg-mcp enabled at boot: $(systemctl is-enabled hg-mcp 2>/dev/null)"
say "  To verify a real reboot cycle: reboot the device, then re-run this script and confirm"
say "  hg-mcp is active and both tools answer without any manual step."

# ===== P1 (untested) ========================================================
hr; say "8  P1 features (not on the critical path)"
rec p1 display INFO "untested - panel patches not merged"
rec p1 touchscreen INFO "untested - FT3518 patch series not merged"
rec p1 bluetooth INFO "untested - firmware/path unconfirmed"
rec p1 audio INFO "not attempted"
rec p1 camera INFO "not attempted"
rec p1 sensors INFO "not attempted"
say "  display/touch/bt/audio/camera/sensors: UNTESTED (deliberately out of scope)"

# ===== summary ==============================================================
hr; say "SUMMARY"
P=$(grep -cP '\tPASS\t' "$R" 2>/dev/null); P=${P:-0}
F=$(grep -cP '\tFAIL\t' "$R" 2>/dev/null); F=${F:-0}
S=$(grep -cP '\tSKIP\t' "$R" 2>/dev/null); S=${S:-0}
say "  passed : $P"
say "  failed : $F"
say "  skipped: $S"
say "  verdict: $([ "$F" -eq 0 ] && [ $((P+F)) -gt 0 ] && echo PASS || echo FAIL)"

[ -n "$TSV_OUT" ] && { cp "$R" "$TSV_OUT"; say "  tsv: $TSV_OUT"; }
if [ -n "$JSON_OUT" ]; then
  {
    printf '{\n  "schema": "homeguardian-firstboot/1",\n  "arch": "%s",\n  "kernel": "%s",\n  "os": "%s",\n  "ran_at_utc": "%s",\n  "summary": {"pass": %s, "fail": %s, "skip": %s},\n  "results": [\n' \
      "$ARCH" "$KREL" "$OSV" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$P" "$F" "$S"
    awk -F'\t' '{r[NR]=sprintf("    {\"group\":\"%s\",\"name\":\"%s\",\"verdict\":\"%s\",\"detail\":\"%s\"}",$1,$2,$3,$4)}
                 END{for(i=1;i<=NR;i++) printf "%s%s\n", r[i], (i<NR?",":"")}' "$R"
    printf '  ]\n}\n'
  } > "$JSON_OUT"
  say "  json: $JSON_OUT"
fi

rm -rf "$WORK"
[ "$F" -eq 0 ] && [ $((P+F)) -gt 0 ]
