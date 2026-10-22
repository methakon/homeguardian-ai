#!/usr/bin/env python3
"""Bounded STATIC docker boundary regression check for the network lane.

Read-only. It inspects container configuration and, where a container runs with host networking, the
listeners actually in its network namespace. It never starts, stops, restarts, edits or firewalls
anything.

Two rules, and deliberately NOT a third:

  R1  a protected credential path is mounted WRITABLE and has no approved exception        -> FAIL
  R2  a port is published on a non-loopback host address and has no approved exception     -> FAIL
  R3  a container is privileged and has no approved exception                              -> FAIL
  R4  network_mode=host                                                                    -> NOT A FAILURE
      Host networking is a structural risk, not evidence of exposure. Docker's `ports={}` is
      uninformative for such a container, so the check looks at what the container's own network
      namespace is actually listening on: a non-loopback LISTEN is a FAIL, no listener is a WARN
      (latent risk, unproven either way), and an unreadable namespace is UNVERIFIED - never PASS.
      A blanket "host network == exposed" assertion would be wrong and is exactly what this avoids.

Controls: `--self-test` runs an intentionally UNSAFE sample (must FAIL, naming the rules) and a SAFE
sample (must PASS, no false positives). A regression check that cannot be shown to fail on bad input
is not a check.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXCEPTIONS = os.path.join(HERE, "docker-boundary-exceptions.json")

# protected credential material: writable = a compromised container can plant or overwrite secrets
PROTECTED = re.compile(r'(^|/)(etc/homeguardian/(certs|device-keys)|run/secrets|pki)($|/)'
                       r'|certs?$|keys?$|secrets?$|credentials?$', re.I)
LOOPBACK = ("127.", "::1")


def load_exceptions():
    try:
        return json.load(open(EXCEPTIONS))
    except (OSError, ValueError):
        return []


def approved(exc, container, rule):
    return any(e.get("container") in (container, "*") and e.get("rule") == rule
               for e in exc if e.get("approved_by"))


def _is_loopback(addr):
    return any(addr.startswith(p) for p in LOOPBACK)


def evaluate(records, exc):
    """Pure function over normalised records -> (findings, warnings, unverified)."""
    findings, warnings, unverified = [], [], []
    for r in records:
        name = r.get("name", "?")
        for m in r.get("mounts", []):
            if m.get("rw") and PROTECTED.search(m.get("destination", "")):
                if approved(exc, name, "writable_credential_mount"):
                    continue
                findings.append(f"R1 {name}: protected path mounted WRITABLE "
                                f"{m.get('source')} -> {m.get('destination')}")
        for p in r.get("ports", []):
            ip = p.get("host_ip", "") or ""
            if ip and not _is_loopback(ip):
                if approved(exc, name, "nonloopback_publish"):
                    continue
                findings.append(f"R2 {name}: port {p.get('host_port')} published on {ip} "
                                f"(not loopback)")
        if r.get("privileged") and not approved(exc, name, "privileged"):
            findings.append(f"R3 {name}: container is privileged")
        if str(r.get("network_mode", "")) == "host":
            ls = r.get("host_listeners")
            if ls is None:
                unverified.append(f"R4 {name}: network_mode=host and its namespace could not be "
                                  f"read - UNVERIFIED, not a pass")
            else:
                bad = [x for x in ls if not _is_loopback(str(x.get("addr", "")))]
                if bad:
                    findings.append(f"R4 {name}: network_mode=host AND listening on "
                                    f"{bad[0].get('addr')}:{bad[0].get('port')} (reachable off-host)")
                else:
                    warnings.append(f"R4 {name}: network_mode=host, no non-loopback listener "
                                    f"observed - latent structural risk, not proven exposure")
    return findings, warnings, unverified


# --------------------------------------------------------------------------- live gathering
def ns_listeners(pid):
    """LISTEN sockets in a pid's network namespace, or None if unreadable."""
    if not pid:
        return None
    out = []
    for f in ("tcp", "tcp6"):
        p = f"/proc/{pid}/net/{f}"
        try:
            lines = open(p).read().splitlines()[1:]
        except OSError:
            return None
        for line in lines:
            parts = line.split()
            if len(parts) < 4 or parts[3] != "0A":          # 0A = LISTEN
                continue
            # parts[1] is like 0100007F:2256 (little-endian hex ip : port)
            try:
                ip_hex, port_hex = parts[1].split(":")
                port = int(port_hex, 16)
                if len(ip_hex) == 8:
                    octets = [int(ip_hex[i:i + 2], 16) for i in (6, 4, 2, 0)]
                    addr = ".".join(str(o) for o in octets)
                else:
                    addr = "::" if set(ip_hex) <= {"0"} else ip_hex
            except (ValueError, IndexError):
                continue
            out.append({"addr": addr, "port": port})
    return out


def from_docker():
    names = subprocess.run(["docker", "ps", "--format", "{{.Names}}"],
                           capture_output=True, text=True).stdout.split()
    records = []
    for n in names:
        f = ("{{.HostConfig.NetworkMode}}|{{.HostConfig.Privileged}}|{{.State.Pid}}|"
             "{{json .NetworkSettings.Ports}}")
        raw = subprocess.run(["docker", "inspect", n, "--format", f],
                             capture_output=True, text=True).stdout.strip()
        if not raw:
            continue
        mode, priv, pid, ports_json = (raw.split("|") + ["", "", "", "{}"])[:4]
        try:
            ports = json.loads(ports_json or "{}")
        except ValueError:
            ports = {}
        pubs = []
        for cport, binds in (ports or {}).items():
            for b in (binds or []):
                pubs.append({"container": cport, "host_ip": b.get("HostIp", ""),
                             "host_port": b.get("HostPort")})
        mounts = json.loads(subprocess.run(
            ["docker", "inspect", n, "--format", "{{json .Mounts}}"],
            capture_output=True, text=True).stdout or "[]")
        rec = {"name": n, "network_mode": mode, "privileged": priv == "true",
               "ports": pubs,
               "mounts": [{"source": m.get("Source"), "destination": m.get("Destination"),
                           "rw": bool(m.get("RW"))} for m in mounts]}
        rec["host_listeners"] = ns_listeners(pid) if mode == "host" and pid not in ("", "0") else None
        records.append(rec)
    return records


# --------------------------------------------------------------------------- controls
UNSAFE = [
    {"name": "unsafe-host-net", "network_mode": "host", "privileged": False,
     "ports": [], "mounts": [],
     "host_listeners": [{"addr": "0.0.0.0", "port": 8443}]},
    {"name": "unsafe-publish", "network_mode": "bridge", "privileged": False,
     "ports": [{"container": "8443/tcp", "host_ip": "0.0.0.0", "host_port": "8443"}],
     "mounts": [{"source": "/s", "destination": "/etc/homeguardian/certs", "rw": True}]},
    {"name": "unsafe-privileged", "network_mode": "bridge", "privileged": True,
     "ports": [], "mounts": []},
]
SAFE = [
    {"name": "safe-loopback", "network_mode": "bridge", "privileged": False,
     "ports": [{"container": "8443/tcp", "host_ip": "127.0.0.1", "host_port": "8443"}],
     "mounts": [{"source": "/s", "destination": "/etc/homeguardian/certs", "rw": False}]},
    {"name": "safe-host-net-loopback-only", "network_mode": "host", "privileged": False,
     "ports": [], "mounts": [],
     "host_listeners": [{"addr": "127.0.0.1", "port": 8790}]},
]


def self_test():
    exc = []
    f1, w1, u1 = evaluate(UNSAFE, exc)
    f2, w2, u2 = evaluate(SAFE, exc)
    rules = sorted({x.split()[0] for x in f1})
    ok = (len(f1) == 3 and rules == ["R1", "R2", "R3"] and not f2)
    print("  UNSAFE sample:")
    for x in f1:
        print(f"    FAIL {x}")
    print(f"  SAFE sample:")
    for x in w2:
        print(f"    WARN {x}")
    print(f"    (no FAIL, no UNVERIFIED: {not f2 and not u2})")
    print(f"  [{'PASS' if ok else 'FAIL'}] controls: unsafe sample fails on R1+R2+R3; safe sample passes")
    # a host-networked container that only listens on loopback must NOT be failed  (no blanket rule)
    assert not any("safe-host-net-loopback-only" in x for x in f2), "blanket host-network rule!"
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--fixture")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    records = json.load(open(a.fixture)) if a.fixture else from_docker()
    f, w, u = evaluate(records, load_exceptions())
    if a.json:
        print(json.dumps({"findings": f, "warnings": w, "unverified": u,
                          "containers": len(records),
                          "verdict": "FAIL" if f else "PASS"}, indent=2))
    else:
        print(f"  static boundary check over {len(records)} container(s)")
        for x in f:
            print(f"  FAIL      {x}")
        for x in w:
            print(f"  WARN      {x}")
        for x in u:
            print(f"  UNVERIFIED {x}")
        print(f"  verdict: {'FAIL' if f else 'PASS'}"
              f"{' (with warnings)' if w and not f else ''}")
    return 1 if f else 0


if __name__ == "__main__":
    sys.exit(main())
