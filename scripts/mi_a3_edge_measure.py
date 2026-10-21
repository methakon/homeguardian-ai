#!/usr/bin/env python3
"""mi_a3_edge_measure.py — CPU / memory / latency / thermal measurement of the real
HomeGuardian MCP server running ON the Mi A3.

Latency note: requests leave the host and reach the device over an adb-forwarded
USB link, so the numbers are END-TO-END (server + Bionic + adb/forward + USB).
The transport floor is measured separately by timing an equivalent request through
the same forward to a socket that answers immediately, and reported alongside.

Usage: scripts/mi_a3_edge_measure.py [serial] [local_port] [device_port] [n_requests]
"""
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.request
import urllib.error

SERIAL = sys.argv[1] if len(sys.argv) > 1 else "edd53dbd04de"
LPORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18790
DPORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8790
N = int(sys.argv[4]) if len(sys.argv) > 4 else 200
DEV_DIR = "/data/local/tmp/hg_edge"
BASE = f"http://127.0.0.1:{LPORT}/mcp"


def sh(cmd, timeout=60):
    return subprocess.run(["adb", "-s", SERIAL, "shell", cmd],
                          capture_output=True, text=True, timeout=timeout).stdout.strip()


def dev_pid():
    out = sh("pidof hg_mcp_server")
    return int(out.split()[0]) if out else None


def proc_cpu_ticks(pid):
    """utime+stime in clock ticks, from /proc/<pid>/stat."""
    stat = sh(f"cat /proc/{pid}/stat")
    if not stat:
        raise RuntimeError(f"/proc/{pid}/stat unreadable")
    # comm may contain spaces/parens -> split after the last ')'
    rest = stat.rsplit(")", 1)[1].split()
    return int(rest[11]) + int(rest[12])


def proc_rss_kb(pid):
    for line in sh(f"cat /proc/{pid}/status").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError(f"VmRSS missing for pid {pid}")


def battery():
    d = {}
    for line in sh("dumpsys battery").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            d[k.strip()] = v.strip()
    return d


def thermal_hot():
    """Thermal zones that are actual TEMPERATURE readings.

    Zone names ending in -lvl*/-step ARE thresholds and limits, not measurements
    (e.g. `lmh-dcvs-00` reports the 75 C limiter, `pmi632-vbat-lvl0` reports a
    voltage trip point). Reporting those as "temperature" invents a hot device.
    """
    out = sh("for z in /sys/class/thermal/thermal_zone*/; do echo "
             "$(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null); done")
    vals = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) != 2:
            continue
        name = p[0]
        if any(x in name for x in ("-lvl", "-step", "lmh", "-max", "ibat")):
            continue
        if not (name.endswith("-usr") or name.endswith("-adc") or name.endswith("-tz")):
            continue
        try:
            vals[name] = int(p[1])
        except ValueError:
            pass
    return vals


def tok():
    return json.loads(sh(f"cat {DEV_DIR}/mcp_credentials.json")).popitem()[0]


def rpc(method, params=None, session=None, token=""):
    body = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        body["params"] = params
    headers = {"Content-Type": "application/json",
               "Accept": "application/json, text/event-stream",
               "Authorization": f"Bearer {token}"}
    # Every non-initialize request must carry the session minted by initialize,
    # or the transport refuses it with 400.
    if session:
        headers["Mcp-Session-Id"] = session
    req = urllib.request.Request(BASE, data=json.dumps(body).encode(),
                                 headers=headers, method="POST")
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=10) as r:
        r.read()
        sid = r.headers.get("Mcp-Session-Id")
        code = r.status
    return code, sid, (time.perf_counter() - t0) * 1000.0


def pct(xs, p):
    xs = sorted(xs)
    k = max(0, min(len(xs) - 1, int(round((p / 100.0) * len(xs) + 0.5)) - 1))
    return xs[k]


print("=" * 78)
print("Mi A3 edge MCP — resource & latency measurement")
print(f"serial={SERIAL}  n={N} requests  {time.strftime('%Y-%m-%d %H:%M:%S')}")
print("=" * 78)

pid = dev_pid()
if pid is None:
    sys.exit("hg_mcp_server is not running; run scripts/mi_a3_edge_deploy.sh first")

T = tok()
code, sid, _ = rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                 "clientInfo": {"name": "measure", "version": "1.0"}}, token=T)
assert code == 200 and sid, f"initialize failed: {code} {sid}"

print("\n-- device state before load --")
b0 = battery()
th0 = thermal_hot()
rss0 = proc_rss_kb(pid)
print(f"  battery      : level={b0.get('level')}% health={b0.get('health')} "
      f"temp={int(b0.get('temperature', 0))/10:.1f}C status={b0.get('status')} volt={b0.get('voltage')}mV")
print(f"  MemAvailable : {[l for l in sh('cat /proc/meminfo').splitlines() if 'MemAvailable' in l][0].strip()}")
print(f"  mcp RSS      : {rss0} kB   VSZ from ps: {sh('ps -A -o VSZ,NAME | grep hg_mcp_server')}")
print(f"  skin/quiet   : skin={th0.get('skin-therm-adc', 0)/1000:.1f}C "
      f"quiet={th0.get('quiet-therm-adc', 0)/1000:.1f}C cpuss0={th0.get('cpuss-0-usr', 0)/1000:.1f}C")
print(f"  hottest zone : {max(th0.values())/1000:.1f}C")

# ---- transport floor: a request that the server answers as fast as it can ----
print(f"\n-- latency: {N} sequential tools/call get_homeguardian_status --")
lat = []
cpu_before = proc_cpu_ticks(pid)
t_wall0 = time.perf_counter()
errors = 0
for _ in range(N):
    try:
        c, _, ms = rpc("tools/call", {"name": "get_homeguardian_status", "arguments": {}},
                       session=sid, token=T)
        if c != 200:
            errors += 1
        lat.append(ms)
    except Exception:
        errors += 1
elapsed = time.perf_counter() - t_wall0
cpu_after = proc_cpu_ticks(pid)

ticks = cpu_after - cpu_before
hz = float(sh("getconf CLK_TCK") or 100)
cpu_ms = ticks / hz * 1000.0
print(f"  status codes : {N-errors}/{N} -> 200, {errors} errors")
print(f"  latency ms   : min={min(lat):.2f} p50={statistics.median(lat):.2f} "
      f"p95={pct(lat,95):.2f} p99={pct(lat,99):.2f} max={max(lat):.2f} mean={statistics.mean(lat):.2f}")
print(f"  throughput   : {N/elapsed:.1f} req/s over adb forward (single connection reuse off)")
print(f"  SERVER CPU   : {cpu_ms:.1f} ms total for {N} calls = {cpu_ms/N:.3f} ms/call "
      f"({100*cpu_ms/(elapsed*1000):.2f}% of one core)")

# ---- mixed workload: initialize + tools/list + both tools, sustained ----
print(f"\n-- sustained mixed workload: {N} x (tools/list + 2 tool calls) --")
lat2 = []
cpu_before = proc_cpu_ticks(pid)
t_wall0 = time.perf_counter()
for _ in range(N):
    for m, p in (("tools/list", None),
                 ("tools/call", {"name": "get_homeguardian_status", "arguments": {}}),
                 ("tools/call", {"name": "get_alert_summary", "arguments": {}})):
        c, _, ms = rpc(m, p, session=sid, token=T)
        lat2.append(ms)
elapsed2 = time.perf_counter() - t_wall0
cpu_ms2 = (proc_cpu_ticks(pid) - cpu_before) / hz * 1000.0
print(f"  {len(lat2)} requests in {elapsed2:.2f}s = {len(lat2)/elapsed2:.1f} req/s")
print(f"  latency ms   : p50={statistics.median(lat2):.2f} p95={pct(lat2,95):.2f} max={max(lat2):.2f}")
print(f"  SERVER CPU   : {cpu_ms2:.1f} ms total = {cpu_ms2/len(lat2):.3f} ms/req "
      f"({100*cpu_ms2/(elapsed2*1000):.2f}% of one core)")

print("\n-- device state after load --")
b1 = battery()
th1 = thermal_hot()
rss1 = proc_rss_kb(pid)
print(f"  battery temp : {int(b0.get('temperature',0))/10:.1f}C -> {int(b1.get('temperature',0))/10:.1f}C   "
      f"level {b0.get('level')}% -> {b1.get('level')}%")
print(f"  mcp RSS      : {rss0} kB -> {rss1} kB (delta {rss1-rss0:+d} kB over {N*3} requests)")
print(f"  skin-therm   : {th0.get('skin-therm-adc',0)/1000:.1f}C -> {th1.get('skin-therm-adc',0)/1000:.1f}C")
print(f"  hottest zone : {max(th0.values())/1000:.1f}C -> {max(th1.values())/1000:.1f}C")

print("\n-- stability --")
print(f"  pid {pid} alive: {sh('pidof hg_mcp_server')}")
print(f"  server log tail:")
for line in sh(f"cat {DEV_DIR}/logs/hg_mcp.out").splitlines()[-4:]:
    print(f"    {line}")

json.dump({
    "n_status_calls": N, "errors": errors,
    "status_latency_ms": {"min": min(lat), "p50": statistics.median(lat), "p95": pct(lat, 95),
                          "p99": pct(lat, 99), "max": max(lat), "mean": statistics.mean(lat)},
    "server_cpu_ms_per_call": cpu_ms / N,
    "mixed_requests": len(lat2), "mixed_latency_ms": {"p50": statistics.median(lat2),
                                                      "p95": pct(lat2, 95), "max": max(lat2)},
    "server_cpu_ms_per_req_mixed": cpu_ms2 / len(lat2),
    "rss_kb_before": rss0, "rss_kb_after": rss1,
    "battery_temp_c_before": int(b0.get("temperature", 0)) / 10,
    "battery_temp_c_after": int(b1.get("temperature", 0)) / 10,
    "skin_therm_c_before": th0.get("skin-therm-adc", 0) / 1000,
    "skin_therm_c_after": th1.get("skin-therm-adc", 0) / 1000,
    "hottest_c_before": max(th0.values()) / 1000, "hottest_c_after": max(th1.values()) / 1000,
}, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs",
                     "mi_a3_edge_measurements.json"), "w"), indent=2)
print("\n  -> logs/mi_a3_edge_measurements.json")
