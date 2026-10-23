#!/usr/bin/env python3
"""Generate the cross-receiver contract fixture (HG-ANDROID-CROSS-RECEIVER-10).

WHY THIS EXISTS
The Android transport classifies the hub's refusal reason into RETRY / PERMANENT / SUSPEND. That
classification decides whether the phone retries, drops one observation, or STOPS TALKING. If the
Android token set has drifted from what the receiver actually emits, the failure is silent and one
directional in the worst way:

    an Android-only token      -> the hub never sends it, so a whole rule is dead code
    a receiver-only token      -> the Android side does not recognise it, and because an
                                  UNKNOWN reason fails closed to PERMANENT, a condition that should
                                  SUSPEND the transport (device revoked) instead just drops a single
                                  observation and the phone keeps pushing to a hub that has refused it

So the fixture is generated FROM THE C++ SOURCE, not from a hand-copied list. A copied list would
agree with whatever the Android side already believes and would prove nothing - exactly the trap the
canonical-envelope work hit where a test drifted from the doc it was meant to guard.

WHAT IT PRODUCES
  hg_verify/cross_receiver_fixture.json
    - `runtime_tokens`: every refusal token the receiver can emit, extracted from
      reject_reason_to_string() in src/net/DeviceProtocol.cpp
    - `runtime_default_token`: the receiver's fail-closed default token
    - `android_sets`: the Android transport's PERMANENT / SUSPENDING / RETRYABLE sets
    - `classification`: token -> verdict, for every runtime token
    - `source`: the exact commit each side was read from, so the fixture is attributable

It EXITS NON-ZERO when the two vocabularies disagree, so a drift cannot be committed silently.

Stdlib only. Reads git objects; opens no socket and touches no device.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The receiver side is pinned to a commit so the fixture is reproducible. The Android side is read
# from the working tree, because that is the code being changed.
RECEIVER_REF = os.environ.get("HG_RECEIVER_REF", "02ab07c34c45459857b0cdb3b715edf97d570f5b")
TRANSPORT = os.path.join(REPO, "apk/src/org/homeguardian/harness/ObservationTransport.java")
OUT = os.path.join(REPO, "hg_verify/cross_receiver_fixture.json")


def git_show(ref: str, path: str) -> str:
    r = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=REPO,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"cannot read {path} at {ref}: {r.stderr.strip()}")
    return r.stdout


def runtime_tokens() -> tuple[set, str]:
    """Every token the C++ receiver can actually put on the wire, plus its fail-closed default."""
    src = git_show(RECEIVER_REF, "src/net/DeviceProtocol.cpp")
    body = src.split("reject_reason_to_string", 1)[1]
    # Stop at the end of the mapping function so a later function's strings are not swept in.
    body = body.split("\n}\n", 1)[0]
    tokens = set(re.findall(r'case\s+RejectReason::[a-z_]+\s*:\s*return\s+"([a-z_]+)"', body))
    tokens.discard("ok")                     # "ok" means ACCEPTED, not a refusal
    m = re.search(r'\n\s*return\s+"([a-z_]+)"\s*;', body)
    if not m:
        raise SystemExit("could not find the receiver's fail-closed default token")
    return tokens, m.group(1)


def android_sets() -> dict:
    src = open(TRANSPORT, encoding="utf-8").read()

    def grab(name: str) -> list:
        m = re.search(name + r"\s*=\s*new HashSet<String>\(Arrays\.asList\((.*?)\)\)", src, re.S)
        if not m:
            raise SystemExit(f"could not find {name} in ObservationTransport.java")
        return re.findall(r'"([^"]+)"', m.group(1))

    return {"PERMANENT": grab("PERMANENT_REASONS"),
            "SUSPENDING": grab("SUSPENDING_REASONS"),
            "RETRYABLE": grab("RETRYABLE_REASONS")}


def classify(token: str, sets: dict) -> str:
    """Mirror ObservationTransport.classifyReason EXACTLY, including its precedence.

    SUSPENDING is checked first, then PERMANENT, then RETRYABLE, and anything unrecognised is
    PERMANENT (fail closed). If this mirror and the Java disagree, the Java test says so.
    """
    if token is None:
        return "PERMANENT"
    if token in sets["SUSPENDING"]:
        return "SUSPEND"
    if token in sets["PERMANENT"]:
        return "PERMANENT"
    if token in sets["RETRYABLE"]:
        return "RETRY"
    return "PERMANENT"


def main() -> int:
    rt, rt_default = runtime_tokens()
    sets = android_sets()
    mine = set(sets["PERMANENT"]) | set(sets["SUSPENDING"]) | set(sets["RETRYABLE"])

    only_mine = sorted(mine - rt)
    only_runtime = sorted(rt - mine)

    print("== cross-receiver refusal vocabulary ==")
    print(f"  receiver {RECEIVER_REF[:7]}: {len(rt)} tokens")
    print(f"  android transport       : {len(mine)} tokens")
    print(f"  only on the android side: {only_mine or 'NONE'}")
    print(f"  only on the receiver side: {only_runtime or 'NONE'}")

    # A token only the receiver emits is the DANGEROUS direction: the Android side does not
    # recognise it, so it falls through to PERMANENT. Report which of those should have SUSPENDED,
    # because that is the difference between "stop" and "keep pushing to a hub that refused us".
    missed_suspend = [t for t in only_runtime
                      if t in ("not_paired", "credential_mismatch", "device_revoked",
                               "device_unknown", "device_unpaired", "identity_mismatch",
                               "unauthorized", "consent_withdrawn", "consent_missing",
                               "capture_disabled")]
    suspicious = [t for t in only_runtime if t in ("replay", "stale")]

    if only_runtime:
        print()
        print("  !! the receiver can emit tokens the android side does not know")
        if missed_suspend:
            print(f"  !! of those, these SHOULD suspend and would instead just drop one "
                  f"observation: {missed_suspend}")
        if suspicious:
            print(f"  !! these are likely the same condition under a different name: {suspicious}")

    fixture = {
        "generated_from": {
            "receiver_ref": RECEIVER_REF,
            "receiver_path": "src/net/DeviceProtocol.cpp",
            "android_path": "apk/src/org/homeguardian/harness/ObservationTransport.java",
            "android_sha256": hashlib.sha256(open(TRANSPORT, "rb").read()).hexdigest(),
        },
        "runtime_tokens": sorted(rt),
        "runtime_default_token": rt_default,
        "android_sets": {k: sorted(v) for k, v in sets.items()},
        "classification": {t: classify(t, sets) for t in sorted(rt)},
        "drift": {"only_android": only_mine, "only_receiver": only_runtime},
    }

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(fixture, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"\n  wrote {os.path.relpath(OUT, REPO)}")

    if only_mine or only_runtime:
        print("\n  RESULT: DRIFT - the fixture records it; the drift guard must fail loudly")
        return 1
    print("\n  RESULT: vocabularies agree exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
