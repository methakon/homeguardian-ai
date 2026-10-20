#!/bin/sh
# thermal-gate.sh - ExecStartPre guard for the HomeGuardian MCP service.
#
# HONEST LIMITATION: this board has NO CPU die temperature sensor. The three PMIC
# zones (rf-pa0, quiet, xo) monitor an RF power amplifier, a quiet rail and a
# crystal oscillator. None of them reports CPU temperature. This script therefore
# CANNOT protect the CPU. It exists to fail closed on the one case the hardware
# *can* see, rather than starting a service into an unknown thermal state.
#
# Exit codes:
#   0 - proceed
#   1 - refuse to start (a readable zone is at/above the gate)

set -u

GATE_MC=90000          # 90.0 C. Conservative by design; NOT a CPU safety limit.
MARKER="/var/lib/homeguardian/thermal-gate.log"

log() { logger -t hg-thermal "$*" 2>/dev/null; echo "$*"; }

worst_mc=0
worst_name="none"
worst_readable=0

for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/temp" ] || continue
    t=$(cat "$z/temp" 2>/dev/null) || continue
    case "$t" in ''|*[!0-9-]*) continue ;; esac
    name=$(cat "$z/type" 2>/dev/null || basename "$z")
    worst_readable=$((worst_readable + 1))
    if [ "$t" -gt "$worst_mc" ]; then
        worst_mc=$t
        worst_name=$name
    fi
done

if [ "$worst_readable" -eq 0 ]; then
    log "WARNING: no readable thermal zones. Cannot gate on temperature."
    log "         Proceeding is a deliberate risk decision - the CPU is unmonitored."
    # Fail OPEN here would be unsafe for a hub nobody is watching, but failing
    # closed on a missing sensor would block a device whose sensor set is simply
    # different. Record it loudly and proceed; the operator decision is in the docs.
    printf '%s  NO_SENSORS  gate not applied\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$MARKER" 2>/dev/null
    exit 0
fi

log "thermal check: $worst_readable zone(s), worst ${worst_name} at $((worst_mc/1000))C (gate $((GATE_MC/1000))C)"

if [ "$worst_mc" -ge "$GATE_MC" ]; then
    log "REFUSING START: ${worst_name} at ${worst_mc}mC >= ${GATE_MC}mC"
    printf '%s  REFUSED  %s=%smC\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$worst_name" "$worst_mc" >> "$MARKER" 2>/dev/null
    exit 1
fi

printf '%s  PASS  %s=%smC zones=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$worst_name" "$worst_mc" "$worst_readable" >> "$MARKER" 2>/dev/null
exit 0
