#!/usr/bin/env python3
"""Capture LAN-acceptance evidence as a verifiable artifact, not a pasted summary.

Runs the exposure gate against a host and writes the RAW output plus its sha256 and the gate's own
exit code, so a later reader can check what was actually measured instead of trusting a claim. It
never rewrites a verdict: a FAIL is recorded as FAIL, and SKIPPED gates are recorded as skipped,
because a gate that could not be exercised is not a pass.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "verify_lan_exposure.py")
DEFAULT_OUT = os.path.expanduser("~/.hermes/cache/scratch/hg-lan-acceptance")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--observation-port", type=int, default=8443)
    ap.add_argument("--mcp-port", type=int, default=8790)
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()

    if not os.path.exists(GATE):
        print(f"gate not found: {GATE}"); return 2
    os.makedirs(a.out, exist_ok=True)
    ts = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    cmd = [sys.executable, GATE, "--host", a.host,
           "--observation-port", str(a.observation_port), "--mcp-port", str(a.mcp_port)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    raw = p.stdout + p.stderr
    digest = hashlib.sha256(raw.encode()).hexdigest()

    passed = len(re.findall(r'^\s*PASS\s', raw, re.M))
    failed = len(re.findall(r'^\s*FAIL\s', raw, re.M))
    info = len(re.findall(r'^\s*INFO\s', raw, re.M))
    skipped = len(re.findall(r'not applicable|SKIPPED', raw, re.I))

    base = os.path.join(a.out, f"lan-acceptance-{ts}-{a.host}")
    open(base + ".txt", "w").write(raw)
    record = {
        "captured_utc": ts, "target": a.host, "observation_port": a.observation_port,
        "mcp_port": a.mcp_port, "gate_exit_code": p.returncode,
        "gate_stdout_sha256": digest, "raw_output_file": base + ".txt",
        "counts": {"PASS": passed, "FAIL": failed, "INFO": info, "skipped_or_na": skipped},
        "verdict": "PASS" if p.returncode == 0 else "FAIL",
        "note": ("A verdict of PASS requires the gate to exit 0. SKIPPED gates are not passes. "
                 "Read the raw file and its sha256 rather than this summary."),
    }
    json.dump(record, open(base + ".json", "w"), indent=2)
    print(json.dumps({k: record[k] for k in ("captured_utc", "target", "gate_exit_code",
                                             "counts", "verdict")}, indent=2))
    print(f"  raw evidence: {base}.txt")
    print(f"  sha256      : {digest}")
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
