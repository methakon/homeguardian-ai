#!/bin/sh
# thermal-record.sh - ExecStopPost evidence capture for the HomeGuardian MCP service.
#
# WHY: this board has no CPU die sensor and no passive thermal trip. A thermal
# event therefore leaves no kernel-side record of *why* the CPU was hot. Since the
# device cannot measure the CPU, the fallback detector has to be behavioural:
# capture whatever the sensors CAN see, plus load, so a post-mortem has something
# to correlate. Writes to the journal and an append-only marker file.

set -u
MARKER="/var/lib/homeguardian/thermal-history.log"
mkdir -p "$(dirname "$MARKER")" 2>/dev/null || true

{
    printf '=== %s service stop ===\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'loadavg : %s\n' "$(cat /proc/loadavg 2>/dev/null || echo unknown)"
    printf 'uptime  : %s\n' "$(cut -d' ' -f1 /proc/uptime 2>/dev/null || echo unknown)"
    for z in /sys/class/thermal/thermal_zone*; do
        [ -r "$z/temp" ] || continue
        t=$(cat "$z/temp" 2>/dev/null)
        n=$(cat "$z/type" 2>/dev/null || basename "$z")
        printf 'zone    : %-16s %s mC\n' "$n" "$t"
    done
    # DRAM refresh rate shifts with temperature on some SoCs - weak but real proxy
    printf 'memtotal: %s\n' "$(awk '/MemTotal/{print $2" kB"}' /proc/meminfo 2>/dev/null || echo unknown)"
} >> "$MARKER" 2>/dev/null

logger -t hg-thermal "service stopped; thermal state recorded to $MARKER" 2>/dev/null
exit 0
