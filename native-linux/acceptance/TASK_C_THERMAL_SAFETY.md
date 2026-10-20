# Task C — Thermal safety profile

**This document does not claim the thermal problem is resolved. It is not.** The mitigation
below is a *defensive operating posture* designed around a known, measured deficiency.

---

## 1. The actual limitation (measured, not inferred)

From the built device tree for this board:

```
rf-pa0-thermal  -> pm6125_adc_tm channel 0   critical 125000   (no passive trip)
quiet-thermal   -> pm6125_adc_tm channel 1   critical 125000   (no passive trip)
xo-thermal      -> pm6125_adc_tm channel 2   critical 125000   (no passive trip)
```

1. **There is no CPU-die temperature sensor in this kernel.** No TSENS die sensing. So the
   system cannot read the temperature of the thing generating the heat.
2. **There is no passive cooling trip point.** All three zones are `critical`-only at 125 °C.
   With `CONFIG_THERMAL_GOV_STEP_WISE=y` but no passive trips to govern, the governor has
   nothing to act on. **There is no CPU throttling.**
3. **The three sensors watch the wrong things.** An RF power amplifier, a "quiet" rail and a
   crystal oscillator. None of them tracks CPU core temperature. The SoC could be thermally
   saturated while all three zones read nominal.
4. **125 °C critical means the first thermal response is a hard shutdown**, not a throttle.
   The governor's only available action is to cut power at a temperature the CPU may already
   have exceeded unobserved.

The upstream TSENS series (15 die zones, 85 °C passive trip, tested by its author on this exact
board) is **not** in `v7.2.3-r0`. It would be the real fix. It is not present.

**Consequence for this project:** this is a genuinely poor platform for an unattended,
always-on compute node, and the reason is architectural — not something a config file fixes.

## 2. Why "thermal modules exist" is not a mitigation

The modules being present (`qcom-spmi-adc-tm5`, `qcom-spmi-adc5`, `qcom-spmi-temp-alarm`)
means the *plumbing* works. It does not mean the *measurement* is of the CPU. An earlier check
of mine reported this whole stack absent and I corrected it — but the correction cuts the
opposite way from reassurance: the stack is present **and it still cannot see the CPU**.

## 3. Defensive operating profile

The design principle: **keep average power dissipation far below anything that could matter,
and never assume a sensor will save you.**

### 3.1 Bounded CPU utilisation

```ini
# /etc/systemd/system/hg-mcp.service.d/20-thermal.conf
[Service]
# Pin the service away from CPU0 (which also handles most IRQs on this SoC)
AllowedCPUs=1-7
CPUWeight=20
CPUQuota=25%
IOWeight=20
```

Reasoning: quantisation to a quarter of one CPU keeps the sustained thermal load in the
"phone idle" envelope the chassis was designed for. The MCP server is a low-rate request
handler — measured at **8.7 ms/request** on x86-64 — so it does not need more.

### 3.2 Memory limits (bounds the leak→swap→I/O-storm path)

```ini
MemoryMax=256M
MemoryHigh=192M
MemorySwapMax=64M
TasksMax=64
```

Measured idle RSS was **6,780 kB**, so 256 MB is roughly a 37× headroom. A leak is caught by
the cgroup long before it becomes a memory-pressure/thermal event.

### 3.3 Restricted background workloads

```ini
Nice=10
IOSchedulingClass=idle
IOSchedulingPriority=7
```

Plus, host-wide:

- **No desktop, no display manager** — the rootfs has no GUI at all.
- **No indexing / search services.**
- **Unattended-upgrades disabled**; patching is a deliberate manual action, because an
  unattended apt run is exactly the kind of burst this profile exists to avoid.
- **journald capped** so logging cannot fill storage and trigger compaction:
  `SystemMaxUse=64M`.

### 3.4 Startup thermal gate

```bash
# /usr/libexec/homeguardian/thermal-gate.sh  (runs as ExecStartPre, bounded)
# Reads the PMIC zones and REFUSES to start if anything looks wrong.
# Deliberately conservative because the sensors do not measure the CPU.
for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/temp" ] || continue
    t=$(cat "$z/temp"); name=$(cat "$z/type" 2>/dev/null)
    if [ "$t" -ge 90000 ]; then
        logger -t hg-thermal "REFUSING START: ${name:-unknown} at ${t}mC"
        exit 1
    fi
done
```

The 90 °C gate is **not** a safety guarantee — it cannot see the CPU. It exists to catch the
one case the sensors *can* see and to fail closed rather than start into an unknown state.

### 3.5 Safe shutdown strategy

- `Restart=on-failure` with `RestartSec=30` and `StartLimitBurst=5` — a service that keeps
  dying is a signal, not something to retry forever at full tilt.
- `ExecStopPost` writes a shutdown marker with the last-read zone temperatures, so a
  thermal event leaves evidence.
- **No automatic thermal-triggered workload scaling**, because there is no trustworthy
  temperature signal to trigger on. Fabricating a control loop on an untrusted sensor is worse
  than having none.

### 3.6 Duty-cycled operation instead of continuous

The strongest available mitigation is not a config value: **do not run it continuously.**

```ini
# Run the hub in a bounded daily window rather than 24/7
# e.g. OnCalendar=*-*-* 07:00 .. 23:00, with the unit stopped overnight.
```

For an always-on hub this is a real functional compromise, and it is the honest trade against a
platform with no CPU thermal feedback.

## 4. Hard precondition on deployment

> **No unattended deployment until actual hardware thermal behaviour is measured.**

That is not a formality. The required measurement is:

- Run the real workload on the physical device for an extended period.
- Read the three PMIC zones *and* correlate against observed CPU load.
- Determine empirically the sustained load at which the device becomes uncomfortable or
  unstable — since the SoC's own temperature is unobservable, the detector has to be
  behavioural (throughput degradation, service restarts, kernel messages) not thermal.
- Only then set `CPUQuota`, and only with a measured basis.

Until that measurement exists, every number above is a **conservative starting point chosen to
avoid heat, not a validated limit**.

## 5. Summary

| | |
|---|---|
| CPU die temperature readable? | **No** |
| CPU throttling present? | **No** |
| Thermal response available? | hard shutdown at 125 °C only |
| Mitigation type | workload bounding + duty cycling + fail-closed gates |
| Thermal problem resolved? | **No — mitigated defensively, and that is all** |
| Unattended deployment | **not authorised pending physical measurement** |
