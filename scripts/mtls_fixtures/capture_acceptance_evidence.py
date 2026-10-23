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


SELF_TEST_SAMPLE = """exposure gate against 10.0.0.1 (observation 8443, MCP 8790)

  FAIL  1 receiver reachable on the LAN address              (closed port)
  INFO  2 fail-closed without a client certificate           (not applicable - no LAN listener)
  INFO  3 valid client certificate authenticates             (not applicable - no LAN listener)
  PASS  4 MCP port NOT reachable on the LAN address          (closed while listening on loopback)
  INFO  5 ICMP (informational only)                          (link probe)

NOTE: 2 check(s) SKIPPED (no LAN listener) - not counted as passing

FAIL: 1 required check(s) did not hold
"""


def self_test():
    """Negative control: pin the counting bug so it cannot come back unnoticed."""
    statuses = re.findall(r'^\s*(PASS|FAIL|INFO|SKIP)\s+\d', SELF_TEST_SAMPLE, re.M)
    new_skipped = sum(1 for s in statuses if s in ("INFO", "SKIP"))
    legacy_skipped = len(re.findall(r'not applicable|SKIPPED', SELF_TEST_SAMPLE, re.I))
    print(f"  status lines parsed: {statuses}")
    print(f"  fixed counter -> skipped={new_skipped}   (gate's own NOTE says 2)")
    print(f"  legacy counter -> skipped={legacy_skipped}  (counted the NOTE line as an extra SKIP)")
    note = re.search(r'NOTE:\s*(\d+)\s+check\(s\) SKIPPED', SELF_TEST_SAMPLE)
    authoritative = int(note.group(1)) if note else -1
    legacy2 = sum(1 for s in statuses if s in ("INFO", "SKIP"))   # my second wrong answer
    ok = (statuses.count("PASS") == 1 and statuses.count("FAIL") == 1
          and authoritative == 2 and legacy_skipped == 3 and legacy2 == 3)
    print(f"  authoritative (gate NOTE) -> skipped={authoritative}")
    print(f"  wrong answer #2 (count INFO rows, incl. the informational ICMP row) -> {legacy2}")
    print(f"  [{'PASS' if ok else 'FAIL'}] negative control: the fix corrects a bug that really existed")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=("--self-test" not in sys.argv))
    ap.add_argument("--observation-port", type=int, default=8443)
    ap.add_argument("--mcp-port", type=int, default=8790)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if not os.path.exists(GATE):
        print(f"gate not found: {GATE}"); return 2
    os.makedirs(a.out, exist_ok=True)
    ts = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    cmd = [sys.executable, GATE, "--host", a.host,
           "--observation-port", str(a.observation_port), "--mcp-port", str(a.mcp_port)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    raw = p.stdout + p.stderr
    digest = hashlib.sha256(raw.encode()).hexdigest()

    # Count STATUS LINES ONLY. The previous counter searched the whole text for 'SKIPPED', so it
    # also counted the word inside the gate's own trailing "NOTE: N check(s) SKIPPED" line and
    # inflated the figure by one - it reported 3 skipped where the gate itself reported 2.
    statuses = re.findall(r'^\s*(PASS|FAIL|INFO|SKIP)\s+\d', raw, re.M)
    passed = statuses.count("PASS")
    failed = statuses.count("FAIL")
    info = statuses.count("INFO")
    # The gate STATES the skipped count; do not re-derive it. My two attempts were both wrong: a
    # text-wide search counted the word inside the NOTE line itself, and counting INFO rows counted
    # an informational ICMP row as skipped (INFO is not SKIPPED). Read the gate's own number, and
    # only fall back to per-row detail if the NOTE is absent.
    note = re.search(r'NOTE:\s*(\d+)\s+check\(s\) SKIPPED', raw)
    if note:
        skipped = int(note.group(1))
        skipped_source = "gate NOTE line (authoritative)"
    else:
        skipped = len(re.findall(r'not applicable|no LAN listener to test', raw, re.I))
        skipped_source = "per-row detail (NOTE absent)"

    base = os.path.join(a.out, f"lan-acceptance-{ts}-{a.host}")
    open(base + ".txt", "w").write(raw)
    record = {
        "captured_utc": ts, "target": a.host, "observation_port": a.observation_port,
        "mcp_port": a.mcp_port, "gate_exit_code": p.returncode,
        "gate_stdout_sha256": digest, "raw_output_file": base + ".txt",
        "counts": {"PASS": passed, "FAIL": failed, "INFO": info, "skipped": skipped},
        "skipped_source": skipped_source,
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
