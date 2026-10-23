#!/usr/bin/env python3
"""mi_a3_edge_service_measure.py — CPU / RAM / battery / thermal measurement of the
managed edge-service app on the Mi A3.

Measures the APP PROCESS (which is what now owns the engine), not a shell-launched
binary, so the numbers are the ones an operator would see on a real deployment.
The MCP request rate is read from the app's own probe log, so CPU can be reported
per MCP request as well as per second.

Usage: scripts/mi_a3_edge_service_measure.py [serial] [seconds]
"""
import json
import os
import re
import statistics
import subprocess
import sys
import time

SERIAL = sys.argv[1] if len(sys.argv) > 1 else "edd53dbd04de"
WINDOW_S = int(sys.argv[2]) if len(sys.argv) > 2 else 60
PKG = "org.homeguardian.edgeservice"
EXT_LOG = f"/sdcard/Android/data/{PKG}/files/probe.log"


def sh(cmd, timeout=60):
    return subprocess.run(["adb", "-s", SERIAL, "shell", cmd],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


def pid():
    out = sh(f"pidof {PKG}")
    return int(out.split()[0]) if out else None


def cpu_ticks(p):
    rest = sh(f"cat /proc/{p}/stat").rsplit(")", 1)[1].split()
    return int(rest[11]) + int(rest[12])


def mem(p):
    d = {}
    for line in sh(f"cat /proc/{p}/status").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            # Values carry a unit ("95880 kB") — keep only the number.
            d[k.strip()] = v.strip().split()[0] if v.strip() else ""
    return d


def battery():
    d = {}
    for line in sh("dumpsys battery").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            d[k.strip()] = v.strip()
    return d


def thermals():
    out = sh("for z in /sys/class/thermal/thermal_zone*/; do echo "
             "$(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null); done")
    vals = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) != 2:
            continue
        name = p[0]
        # -lvl*/-step/lmh/-max/ibat are THRESHOLDS, not temperatures.
        if any(x in name for x in ("-lvl", "-step", "lmh", "-max", "ibat")):
            continue
        if not (name.endswith("-usr") or name.endswith("-adc") or name.endswith("-tz")):
            continue
        try:
            vals[name] = int(p[1])
        except ValueError:
            pass
    return vals


def meminfo():
    d = {}
    for line in sh("cat /proc/meminfo").splitlines():
        k = line.split(":")[0]
        v = line.split()[1]
        d[k] = int(v)
    return d


def probe_log():
    return sh(f"cat {EXT_LOG} 2>/dev/null")


def count_probes(text):
    ok = len(re.findall(r"PROBE_OK", text))
    fail = len(re.findall(r"PROBE_FAIL", text))
    lat = [int(m) for m in re.findall(r"PROBE_OK (\d+)ms", text)]
    return ok, fail, lat


p = pid()
if p is None:
    sys.exit(f"{PKG} is not running — start the service first")

print("=" * 78)
print(f"Mi A3 edge service — resource measurement   window={WINDOW_S}s")
print("=" * 78)

b0, t0, m0 = battery(), thermals(), meminfo()
st0, ticks0 = mem(p), cpu_ticks(p)
log0 = probe_log()
ok0, fail0, _ = count_probes(log0)

print("\n-- before --")
print(f"  app pid {p}   RSS {st0.get('VmRSS','?')} kB   PSS {st0.get('VmHWM','?')} kB(peak)")
print(f"  device MemAvailable {m0['MemAvailable']/1024:.0f} MB")
print(f"  battery level={b0.get('level')}% health={b0.get('health')} "
      f"temp={int(b0.get('temperature',0))/10:.1f}C status={b0.get('status')}")
print(f"  skin-therm {t0.get('skin-therm-adc',0)/1000:.1f}C   "
      f"hottest {max(t0.values())/1000:.1f}C")
print(f"  probe log: {ok0} ok / {fail0} fail so far")

samples = []
t_wall0 = time.time()
while time.time() - t_wall0 < WINDOW_S:
    s = mem(p)
    samples.append((time.time() - t_wall0, int(s.get("VmRSS", 0)), cpu_ticks(p)))
    time.sleep(5)

elapsed = time.time() - t_wall0
ticks = samples[-1][2] - ticks0
hz = float(sh("getconf CLK_TCK") or 100)
cpu_ms = ticks / hz * 1000.0

log1 = probe_log()
ok1, fail1, lats = count_probes(log1)
probes = (ok1 - ok0) + (fail1 - fail0)

b1, t1, m1 = battery(), thermals(), meminfo()
st1 = mem(p)

print(f"\n-- over {elapsed:.0f}s --")
print(f"  MCP probes completed : {probes}  (ok {ok1-ok0}, fail {fail1-fail0})")
print(f"  APP CPU              : {cpu_ms:.0f} ms of CPU over {elapsed:.0f}s "
      f"= {100*cpu_ms/(elapsed*1000):.2f}% of one core")
if probes:
    print(f"  APP CPU per MCP probe: {cpu_ms/probes:.2f} ms")
rss = [s[1] for s in samples]
print(f"  RSS samples (kB)     : min {min(rss)} max {max(rss)} last {rss[-1]}  "
      f"(growth {rss[-1]-rss[0]:+d} kB)")
if lats:
    print(f"  probe latency ms     : min {min(lats)} p50 {statistics.median(lats):.0f} max {max(lats)}")

print("\n-- after --")
print(f"  battery temp {int(b0.get('temperature',0))/10:.1f}C -> {int(b1.get('temperature',0))/10:.1f}C   "
      f"level {b0.get('level')}% -> {b1.get('level')}%")
print(f"  skin-therm {t0.get('skin-therm-adc',0)/1000:.1f}C -> {t1.get('skin-therm-adc',0)/1000:.1f}C   "
      f"hottest {max(t0.values())/1000:.1f}C -> {max(t1.values())/1000:.1f}C")
print(f"  device MemAvailable {m0['MemAvailable']/1024:.0f} MB -> {m1['MemAvailable']/1024:.0f} MB")
print(f"  app RSS {st0.get('VmRSS')} kB -> {st1.get('VmRSS')} kB   "
      f"peak VmHWM {st1.get('VmHWM')} kB")

out = {
    "window_s": elapsed, "app_pid": p,
    "app_cpu_ms": cpu_ms, "app_cpu_pct_one_core": 100 * cpu_ms / (elapsed * 1000),
    "mcp_probes": probes, "app_cpu_ms_per_probe": (cpu_ms / probes) if probes else None,
    "rss_kb_min": min(rss), "rss_kb_max": max(rss), "rss_kb_last": rss[-1],
    "rss_kb_peak_vmhwm": st1.get("VmHWM"),
    "battery_temp_c_before": int(b0.get("temperature", 0)) / 10,
    "battery_temp_c_after": int(b1.get("temperature", 0)) / 10,
    "battery_level_before": b0.get("level"), "battery_level_after": b1.get("level"),
    "skin_therm_c_before": t0.get("skin-therm-adc", 0) / 1000,
    "skin_therm_c_after": t1.get("skin-therm-adc", 0) / 1000,
    "hottest_c_before": max(t0.values()) / 1000, "hottest_c_after": max(t1.values()) / 1000,
    "memavailable_kb_before": m0["MemAvailable"], "memavailable_kb_after": m1["MemAvailable"],
    "probe_latency_ms": {"min": min(lats), "p50": statistics.median(lats), "max": max(lats)} if lats else None,
}
dest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs",
                    "mi_a3_edge_service_measurements.json")
json.dump(out, open(dest, "w"), indent=2)
print(f"\n  -> {os.path.normpath(dest)}")
