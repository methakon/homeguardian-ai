#!/usr/bin/env python3
"""HG-WORKER-BOOTSTRAP-S1 - Phase 4 verification of the task guard.

Exercises the six properties the mission requires, plus the refusal paths that make them real.
Each case asserts a specific outcome: an ADMIT, or a REFUSE carrying the expected reason code.

  1 duplicate tasks are not executed twice
  2 a second task does not interrupt a running test
  3 a restarted session does not repeat completed work
  4 a retry cannot produce a second commit
  5 a task assigned to another worker is refused
  6 Oracle / handset changes cannot be authorized by ordinary comment prose
  + embedded shell commands are refused rather than executed
  + scope widening, wrong branch, missing fields and illegal transitions are refused

Run: python3 scripts/worker/test_task_guard.py
Exit 0 only if every case behaves as specified.
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from task_guard import Ledger, Refused, validate          # noqa: E402

CFG = json.load(open(os.path.join(HERE, "identity.json")))
results: list[tuple[str, bool, str]] = []


def base_task(**over):
    t = {
        "task_id": "HG-LAN-MTLS-07-004",
        "source": "issue:14#comment-6308264441",
        "assigned_to": "hg-hermes-network",
        "expected_branch": "experiment/hg-lan-mtls-07",
        "allowed_scope": ["scripts/mtls_fixtures/"],
        "objective": "extend the reconnect coverage for the device transport",
        "acceptance_tests": ["python3 scripts/mtls_fixtures/test_reconnect.py"],
        "prohibited_actions": ["oracle_change", "handset_change"],
    }
    t.update(over)
    return t


def fresh_ledger() -> Ledger:
    return Ledger(os.path.join(tempfile.mkdtemp(prefix="hg-lease-"), "ledger.json"))


def check(name: str, expect: str, fn) -> None:
    try:
        out = fn()
        got, detail = "ADMIT", str(out)
    except Refused as exc:
        got, detail = exc.code, exc.detail
    ok = got == expect
    results.append((name, ok, f"expected {expect}, got {got} ({detail[:70]})"))


# 1 — duplicate tasks are not executed twice -------------------------------------------------
def t1():
    led = fresh_ledger()
    for s in ("CLAIMED", "RUNNING", "TESTING", "COMMITTED", "REPORTED", "COMPLETE"):
        led.transition("HG-LAN-MTLS-07-004", s)
    return validate(base_task(), CFG, led)


# 2 — a second task must not interrupt a running test ----------------------------------------
def t2():
    led = fresh_ledger()
    led.claim("HG-LAN-MTLS-07-004", CFG["worker_id"])
    led.transition("HG-LAN-MTLS-07-004", "RUNNING")
    return validate(base_task(), CFG, led)


# 3 — a restarted session must not repeat completed work -------------------------------------
def t3():
    led = fresh_ledger()
    for s in ("CLAIMED", "RUNNING", "TESTING", "COMMITTED", "REPORTED", "COMPLETE"):
        led.transition("HG-LAN-MTLS-07-004", s)
    reloaded = Ledger(led.path)                      # process restart: state comes from disk
    return validate(base_task(), CFG, reloaded)


# 4 — a retry cannot produce a second commit --------------------------------------------------
def t4():
    """The identical retry must be idempotent, and a DIFFERENT commit must be refused.

    The second attempt is deliberately allowed to raise so the expected refusal code is the
    outcome the harness observes - catching it here would have reported the case as an ADMIT.
    """
    led = fresh_ledger()
    led.record_commit("HG-LAN-MTLS-07-004", "a" * 40)
    again = led.record_commit("HG-LAN-MTLS-07-004", "a" * 40)
    assert again == "a" * 40, f"idempotent retry changed the recorded SHA to {again}"
    return led.record_commit("HG-LAN-MTLS-07-004", "b" * 40)       # different commit -> refused


# 5 — another worker's task is refused --------------------------------------------------------
def t5():
    return validate(base_task(assigned_to="hg-hermes-receiver"), CFG, fresh_ledger())


# 6 — prose cannot authorize an infrastructure change -----------------------------------------
def t6a():
    """A task asking for an Oracle change with no approval record must be refused."""
    return validate(base_task(task_id="X-1", requires_approval=["oracle_change"]), CFG, fresh_ledger())


def t6b():
    """An in-band approval reference cannot replace authenticated dispatch context."""
    return validate(base_task(task_id="X-2", requires_approval=["oracle_change"],
                              approvals={"oracle_change": "owner:issue-14#approval-2026-10-21"}),
                    CFG, fresh_ledger())


# refusal paths that make the guard meaningful -------------------------------------------------
def cmd_case():
    return validate(base_task(task_id="X-3",
                              acceptance_tests=["ssh ubuntu@host 'rm -rf /var/tmp'"]),
                    CFG, fresh_ledger())


def scope_case():
    return validate(base_task(task_id="X-4", allowed_scope=["deploy/oracle/"]), CFG, fresh_ledger())


def branch_case():
    return validate(base_task(task_id="X-5", expected_branch="main"), CFG, fresh_ledger())


def field_case():
    t = base_task(task_id="X-6")
    del t["acceptance_tests"]
    return validate(t, CFG, fresh_ledger())


def transition_case():
    led = fresh_ledger()
    return led.transition("HG-LAN-MTLS-07-004", "COMPLETE")       # QUEUED -> COMPLETE


CASES = [
    ("1 duplicate task is not executed twice", "DUPLICATE_COMPLETE", t1),
    ("2 a running test is not interrupted", "ALREADY_RUNNING", t2),
    ("3 restart does not repeat completed work", "DUPLICATE_COMPLETE", t3),
    ("4 retry cannot create a second commit", "COMMIT_ALREADY_RECORDED", t4),
    ("5 another worker's task is refused", "NOT_MY_TASK", t5),
    ("6a prose alone cannot authorize an Oracle change", "APPROVAL_REQUIRED", t6a),
    ("6b approval reference without authentication is refused", "AUTHENTICATION_REQUIRED", t6b),
    ("+ embedded shell commands are refused", "EMBEDDED_COMMANDS", cmd_case),
    ("+ scope widening is refused", "SCOPE_WIDENING", scope_case),
    ("+ a task for main is refused", "WRONG_BRANCH", branch_case),
    ("+ a task missing a required field is refused", "MISSING_FIELD", field_case),
    ("+ an illegal transition is refused", "ILLEGAL_TRANSITION", transition_case),
]

print(f"worker: {CFG['worker_id']}  branch: {CFG['branch']}\n")
print(f"{'case':52} {'expected':24} result")
print("-" * 100)
for name, expect, fn in CASES:
    check(name, expect, fn)
for name, ok, detail in results:
    print(f"{name:52} {'':24} {'ok' if ok else 'VIOLATION'}   {detail}")
print("-" * 100)
bad = [r for r in results if not r[1]]
if bad:
    print(f"FAIL: {len(bad)} of {len(results)} cases did not behave as specified")
    sys.exit(1)
print(f"PASS: all {len(results)} cases behave as specified")
