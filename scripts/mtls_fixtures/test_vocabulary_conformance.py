#!/usr/bin/env python3
"""Does the LAN/mTLS harness speak the SAME refusal vocabulary as the owning lanes?

Compares every token this harness can emit against the canonical set observed in
DeviceProtocol.cpp @ 02ab07c and ObservationTransportPolicyTest.java @ cfb96ef. A harness that
invents its own tokens looks correct in isolation and mis-classifies in integration.
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vocabulary import CANONICAL, REQUIRED_FOR_LAN_PATH, PROVENANCE, base   # noqa: E402

EMITTERS = ["test_mtls_validation.py", "test_revocation_and_identity.py",
            "test_failure_recovery_real.py", "verify_lan_exposure.py"]
# Judge only DECISION strings - the values returned or recorded as an outcome. Matching any quoted
# `credential*` string also caught dict FIELD NAMES ("credential_id"), which is not a refusal token
# at all: a scanner that cannot tell a reason from a key reports failures that do not exist.
TOKENISH = re.compile(
    r'(?:return\s+(?:False|True),\s*|return\s+|outcome\.append\(\s*|append\(\s*|\()'
    r'f?["\']([a-z][a-z0-9_]{3,60}(?::[^"\']{0,40})?)["\']')
# explicit safety valve for anything that is structurally a token but semantically a field name
IGNORE = {"credential_id", "device_id", "identity_id"}


def main():
    print("  canonical set observed from:")
    for f, sha in PROVENANCE.items():
        print(f"    {f} @ {sha}")
    emitted, foreign = set(), {}
    for fn in EMITTERS:
        p = os.path.join(HERE, fn)
        if not os.path.exists(p):
            continue
        txt = open(p, errors="replace").read()
        for m in TOKENISH.findall(txt):
            t = base(m)
            # only judge strings that look like this harness's own vocabulary
            if t in IGNORE:
                continue
            if t in CANONICAL:
                emitted.add(t)
            elif re.search(r'^(credential|device|identity|consent)_', t):
                foreign.setdefault(t, []).append(fn)
    print(f"\n  harness emits {len(emitted)} canonical token(s): {sorted(emitted)}")
    bad = 0
    if foreign:
        bad = 1
        print("  [FAIL] NON-CANONICAL tokens emitted (integration would mis-classify these):")
        for t, files in sorted(foreign.items()):
            print(f"      {t}   in {', '.join(sorted(set(files)))}")
    else:
        print("  [PASS] every emitted token is canonical")
    missing = REQUIRED_FOR_LAN_PATH - emitted
    if missing:
        print(f"\n  [GAP] no coverage for canonical token(s): {sorted(missing)}")
        print("        -> a LAN-first camera-to-hub path must be able to refuse these; the harness")
        print("           currently cannot demonstrate it. Reported, NOT counted as a pass.")
    else:
        print("\n  [PASS] LAN-path refusal tokens all covered")
    print(f"\n  conformance: {'PASS' if not bad else 'FAIL'}")
    return bad


if __name__ == "__main__":
    sys.exit(main())
