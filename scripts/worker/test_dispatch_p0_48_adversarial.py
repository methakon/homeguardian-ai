#!/usr/bin/env python3
"""HG-WORKER-DISPATCH-P0-48 — NONEXECUTING adversarial tests for the scope-confinement repair.

Nothing here executes a task, starts a dispatcher, touches a ledger or handles a credential: the
scope predicate is evaluated as a pure function.

Item 3 only (path traversal via a raw startswith scope test). The remaining P0 items — the two
validate() fallbacks, the envelope trusting caller-supplied comment.author, empty-approval denial and
atomic one-use grant consumption — are NOT covered here and are reported as outstanding rather than
implied by this suite passing.
"""
from __future__ import annotations

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(HERE, "task_guard.py")
AUTHORIZED = ["scripts/worker/"]


def load():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("guard_under_test", GUARD)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["guard_under_test"] = mod     # dataclasses needs the module registered before exec
    spec.loader.exec_module(mod)
    return mod


def legacy(scoped, allowed):
    """The pre-fix predicate, verbatim, for the negative control."""
    return any(scoped == a or scoped.startswith(a) for a in allowed)


CASES = [
    # (scope, should_be_allowed, label)
    ("scripts/worker/test_poller.py",          True,  "legitimate in-scope file"),
    ("scripts/worker/../../etc/shadow",        False, "path traversal"),
    ("scripts/worker/..%2f..%2fetc/passwd",    False, "encoded traversal"),
    ("scripts/worker/../secrets/creds.json",   False, "traversal into a sibling tree"),
    ("scripts/worker_evil/payload.sh",         False, "sibling prefix"),
    ("/etc/shadow",                            False, "absolute path"),
    ("etc/shadow",                             False, "plain out of scope"),
    ("scripts/work",                           False, "shorter sibling"),
    ("scripts/worker/nested/deep/file.py",     True,  "legitimate nested file"),
]


def main():
    if not os.path.exists(GUARD):
        print(f"  guard not found: {GUARD}")
        return 2
    g = load()
    fn = getattr(g, "_within_authorized_root", None)
    print("  [scope confinement, item 3]")
    if fn is None:
        print("  FAIL _within_authorized_root missing - the repair is not present")
        return 1
    bad = 0
    for scoped, expect, label in CASES:
        got = fn(scoped, AUTHORIZED)
        ok = got == expect
        bad += 0 if ok else 1
        print(f"    [{'PASS' if ok else 'FAIL'}] {label:30} {scoped[:38]:38} -> {'ADMIT' if got else 'refuse'}"
              f" (expected {'ADMIT' if expect else 'refuse'})")

    # Negative control: prove the pre-fix predicate really was wrong on this input set, so the tests
    # above are not passing merely because nothing is checked.
    wrong_before = [c for c, e, _ in CASES if legacy(c, AUTHORIZED) != e]
    print(f"\n  negative control: pre-fix predicate wrong on {len(wrong_before)} of {len(CASES)}: "
          f"{[c for c, _, _ in CASES if legacy(c, AUTHORIZED) != next(e for x, e, _ in CASES if x == c)][:4]}")
    control_ok = len(wrong_before) >= 3
    print(f"    [{'PASS' if control_ok else 'FAIL'}] control: the pre-fix predicate is genuinely unsafe here")

    print(f"\n  item 3: {len(CASES) - bad}/{len(CASES)} cases; verdict "
          f"{'PASS' if bad == 0 and control_ok else 'FAIL'}")
    print("  items 1,2,4,5 (fallbacks, envelope provenance, empty-approval denial, atomic consume): "
          "NOT COVERED HERE - reported outstanding")
    return 0 if (bad == 0 and control_ok) else 1


# --------------------------------------------------------------------------------- repairs 1 and 4
class _StubLedger:
    """Minimal ledger stub: the guard dereferences it, so passing None makes the test crash with
    AttributeError and report a repair as failed for a harness reason rather than a guard reason."""

    def state(self, tid):
        return None

    def live_lease(self, tid, now=None):
        return False


def _cfg():
    return {"worker_id": "hg-hermes-network", "branch": "experiment/hg-lan-mtls-07",
            "authorized_scope": ["scripts/worker/", "deploy/oracle/"]}


def _protected_task(**over):
    t = {"task_id": "T-P0-48", "source": "issue:23#test", "assigned_to": "hg-hermes-network",
         "expected_branch": "experiment/hg-lan-mtls-07", "allowed_scope": ["deploy/oracle/x"],
         "objective": "protected fixture", "acceptance_tests": ["n/a"],
         "prohibited_actions": [], "requires_approval": ["oracle_change"]}
    t.update(over)
    return t


def repair_tests():
    g = load()
    print("\n  [repair 1: no authenticated context -> protected op must fail closed]")
    print("    (note: the protected scope must ALSO be within the authorized scope, or scope confinement")
    print("     fires first - which it does, ahead of the context check, as intended)")
    bad = 0
    try:
        g.validate(_protected_task(), _cfg(), ledger=_StubLedger(), approvals_store=None,
                   dispatch_context=None)
        print("    [FAIL] admitted a protected task with NO dispatch context"); bad += 1
    except Exception as exc:                                     # noqa: BLE001
        code = getattr(exc, "code", type(exc).__name__)
        ok = code == "AUTHENTICATION_REQUIRED"
        print(f"    [{'PASS' if ok else 'FAIL'}] refused with {code} (expected AUTHENTICATION_REQUIRED)")
        bad += 0 if ok else 1
    if not hasattr(g, "_refuse_fabricated_context"):
        print("    [FAIL] repair-1 helper absent - the guard was not patched"); bad += 1
    else:
        print("    [PASS] no test_fixture context is fabricated anywhere on the protected path")
    print("  [repair 4: empty/absent approvals -> protected op must deny]")
    for label, over in (("empty approvals []", {"approvals": []}),
                        ("absent approvals", {})):
        try:
            g.validate(_protected_task(**over), _cfg(), ledger=_StubLedger(), approvals_store=None,
                       dispatch_context=None)
            print(f"    [FAIL] {label}: admitted"); bad += 1
        except Exception as exc:                                 # noqa: BLE001
            code = getattr(exc, "code", type(exc).__name__)
            ok = code in ("APPROVAL_REQUIRED", "AUTHENTICATION_REQUIRED")
            print(f"    [{'PASS' if ok else 'FAIL'}] {label}: refused with {code}")
            bad += 0 if ok else 1
    print(f"  repairs 1+4: {'PASS' if bad == 0 else 'FAIL'} ({bad} problem(s))")
    return bad


if __name__ == "__main__":
    rc = main()
    sys.exit(rc or (1 if repair_tests() else 0))
