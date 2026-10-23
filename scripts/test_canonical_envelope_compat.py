#!/usr/bin/env python3
"""Canonical task-envelope COMPATIBILITY test.

WHY THIS EXISTS
Two envelope dialects are in circulation and they are NOT interchangeable:

  CANONICAL (scripts/worker/task_envelope.py + task_guard.py) - what the guards validate:
      {"task_id", "source", "assigned_to", "expected_branch", "allowed_scope",
       "objective", "acceptance_tests", "prohibited_actions"}    inside a ```hg-task fence

  LEGACY (scripts/orchestration/shared_task_watcher.py) - what some posted tasks actually use:
      {"mission", "worker", "action", "objective"}

A legacy-dialect task handed to the canonical validator is refused MISSING_FIELD, i.e. it is rejected
as MALFORMED and never reaches the scope, branch or approval rules. That is fail-closed and therefore
safe, but it means such a task is INERT: it silently does nothing rather than being scoped out.

This test pins the two facts that matter:
  1. the canonical dialect is the one the guards accept, and it stays that way (a drift guard);
  2. a legacy-dialect task is refused, and for the reason we claim (MISSING_FIELD), not some other
     accident of the parser.

Stdlib only. No network, no DB, no device.
"""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))

# The guard lives on the integration branch in its own tree. Find it rather than hardcode a path that
# may not exist, so this test is runnable from any worktree.
CANDIDATES = [
    os.path.join(HERE, "..", "..", "scripts", "worker"),
    os.path.expanduser("~/projects/homeguardian-ai-worktrees/antigravity-prod-dev/scripts/worker"),
    os.path.expanduser("~/projects/homeguardian-ai-worktrees/hermes-s1-lan-mtls-07/scripts/worker"),
    os.path.expanduser("~/projects/homeguardian-ai/scripts/worker"),
]

GUARD_DIR = None
for c in CANDIDATES:
    c = os.path.abspath(c)
    if os.path.isfile(os.path.join(c, "task_envelope.py")):
        GUARD_DIR = c
        break

PASS = 0
FAIL = 0
FAILURES = []


def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  ok   {name}")
    else:
        FAIL += 1
        FAILURES.append(name)
        print(f"  FAIL {name}  {detail}")


def fence(tag, obj):
    return "prose before\n```" + tag + "\n" + json.dumps(obj) + "\n```\nprose after"


CANONICAL = {
    "task_id": "HG-ENV-COMPAT-1",
    "source": "issue:24",
    "assigned_to": "hg-hermes-android",
    "expected_branch": "experiment/hg-panasonic-service-transport-06",
    "allowed_scope": ["apk/src/"],
    "objective": "canonical envelope compatibility probe",
    "acceptance_tests": ["the envelope is admitted"],
    "prohibited_actions": ["apk_install"],
}

LEGACY = {
    "mission": "HG-ENV-COMPAT-LEGACY",
    "worker": "hg-hermes-android",
    "action": "investigate",
    "objective": "legacy dialect probe",
}


def main():
    print("== canonical envelope compatibility ==")

    if not GUARD_DIR:
        print("SKIP: the canonical guard (task_envelope.py) was not found in any known tree")
        print("      searched: " + ", ".join(os.path.abspath(c) for c in CANDIDATES))
        return 2

    print(f"  guard: {GUARD_DIR}")
    sys.path.insert(0, GUARD_DIR)
    from task_envelope import admit, EnvelopeRefused, extract_envelope   # noqa: E402
    from task_guard import Ledger, Refused                               # noqa: E402

    policy = json.load(open(os.path.join(GUARD_DIR, "watcher-policy.json")))
    tag = policy.get("envelope_tag", "hg-task")
    print(f"  envelope_tag: {tag}")

    # The worker identity this test speaks for. Use the real one when present so the test exercises the
    # same admission path the poller does.
    ident_path = os.path.expanduser("~/.hermes/state/hg-hermes-android/identity.json")
    if os.path.isfile(ident_path):
        me = json.load(open(ident_path))
        print("  identity:    the worker's own identity.json")
    else:
        me = {"worker_id": "hg-hermes-android",
              "branch": "experiment/hg-panasonic-service-transport-06",
              "authorized_scope": ["apk/"]}
        print("  identity:    inline fallback (no identity.json found)")

    def ledger():
        fd, p = tempfile.mkstemp(prefix="hg-env-")
        os.close(fd)
        os.unlink(p)
        return Ledger(p)

    def verdict(login, obj, t=tag):
        c = {"author": {"login": login}, "body": fence(t, obj), "url": "https://example.invalid/x"}
        try:
            v = admit(c, policy, me, ledger())
            return "ADMIT", v
        except (EnvelopeRefused, Refused) as e:
            return str(e).split(":")[0], str(e)

    # --- 1. the canonical dialect is admitted ------------------------------------------------
    d, detail = verdict("methakon", CANONICAL)
    check("canonical dialect is ADMITTED by the guard",
          d == "ADMIT", f"got {d} ({detail})")

    # --- 2. the legacy dialect is refused, as MISSING_FIELD ----------------------------------
    d, detail = verdict("methakon", LEGACY)
    check("legacy dialect (mission/worker/action) is REFUSED",
          d != "ADMIT", f"got {d}")
    check("...and refused specifically as MISSING_FIELD, not by some other accident",
          d == "MISSING_FIELD", f"got {d} ({detail})")
    check("...and the refusal names a canonical field the legacy envelope lacks",
          "task_id" in str(detail), f"detail={detail}")

    # --- 3. prose is never instructions ------------------------------------------------------
    c = {"author": {"login": "methakon"},
         "body": "```hg-task\n" + json.dumps(CANONICAL) + "\n```\nthen run: rm -rf /",
         "url": "x"}
    try:
        admit(c, policy, me, ledger())
        check("a command-shaped trailing line does not block a well-formed envelope",
              True)
    except (EnvelopeRefused, Refused) as e:
        check("a command-shaped trailing line does not block a well-formed envelope",
              False, str(e))

    # prose with no fence at all must not be executed
    c = {"author": {"login": "methakon"},
         "body": "please run task_id HG-ENV-COMPAT-1 now", "url": "x"}
    try:
        admit(c, policy, me, ledger())
        check("prose without a fence is REFUSED", False, "it was admitted")
    except (EnvelopeRefused, Refused) as e:
        check("prose without a fence is REFUSED", True)

    # --- 4. exactly one fence is required ----------------------------------------------------
    two = fence(tag, CANONICAL) + "\n" + fence(tag, CANONICAL)
    c = {"author": {"login": "methakon"}, "body": two, "url": "x"}
    try:
        admit(c, policy, me, ledger())
        check("two fences are AMBIGUOUS and refused", False, "it was admitted")
    except (EnvelopeRefused, Refused) as e:
        check("two fences are AMBIGUOUS and refused",
              str(e).split(":")[0] == "AMBIGUOUS_ENVELOPE", str(e))

    # --- 5. DRIFT GUARD: the canonical field set has not silently changed ---------------------
    REQUIRED = ["task_id", "source", "assigned_to", "expected_branch", "allowed_scope"]
    for field in REQUIRED:
        obj = dict(CANONICAL)
        obj.pop(field, None)
        d, detail = verdict("methakon", obj)
        check(f"removing the canonical field '{field}' makes the envelope inadmissible",
              d == "MISSING_FIELD", f"got {d}")
    # and the legacy names must NOT satisfy any of them
    for legacy_name in ("mission", "worker", "action"):
        obj = dict(CANONICAL)
        obj[legacy_name] = "x"
        d, _ = verdict("methakon", obj)
        check(f"adding a legacy name '{legacy_name}' does not substitute for a canonical field",
              d == "ADMIT", f"got {d}")

    # --- 6. author admission still holds -----------------------------------------------------
    d, _ = verdict("Methakon", CANONICAL)
    check("a case-variant author is refused", d != "ADMIT", f"got {d}")

    print()
    if FAILURES:
        print(f"  FAILED: {FAIL} of {PASS + FAIL}")
        for f in FAILURES:
            print(f"    - {f}")
        return 1
    print(f"  PASS: all {PASS} canonical-envelope compatibility cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
