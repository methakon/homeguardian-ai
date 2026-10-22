#!/usr/bin/env python3
"""HG-AG-P0-TRUST-BOUNDARY-34 — Adversarial Trust Boundary Test Suite.

Verifies the 12 required invariant cases A through L against the task admission
guard and execution boundary:

  Case A: Forged task.author='owner' with approval reference
  Case B: Delete/false prohibited_actions or requires_approval (bypassing hint)
  Case C: Approval ID for wrong operation/target/device/worker
  Case D: Approval record invented in payload or repo demo data
  Case E: Expired, not-yet-valid, revoked, replayed grant
  Case F: Self-approval or unauthorized approver
  Case G: Missing auth context or spoofed auth method
  Case H: Canonical digest mismatch / tamper detection
  Case I: Concurrent identical privileged attempts (atomic single-use / race)
  Case J: Store locked / read-only / unavailable
  Case K: One valid approved operation (ALLOW / execute once)
  Case L: Benign test/fixture operation without approval (ALLOW)
"""
import concurrent.futures
import json
import os
import sqlite3
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from task_guard import (  # noqa: E402
    ApprovalStore,
    CanonicalOperation,
    Ledger,
    Refused,
    TrustedDispatchContext,
    authorize,
    validate,
)

CFG = json.load(open(os.path.join(HERE, "identity.json")))
RESULTS: list[tuple[str, bool, str]] = []


def record(case_id: str, desc: str, expected_code: str, fn) -> None:
    try:
        res = fn()
        got = "ADMIT"
        detail = str(res)
    except Refused as exc:
        got = exc.code
        detail = exc.detail
    ok = (got == expected_code)
    RESULTS.append((f"Case {case_id}: {desc}", ok, f"expected {expected_code}, got {got} ({detail[:60]})"))


def fresh_env():
    td = tempfile.mkdtemp(prefix="hg-p0-test-")
    db_path = os.path.join(td, "test_approvals.db")
    store = ApprovalStore(db_path)
    ledger = Ledger(os.path.join(td, "ledger.json"))
    return td, store, ledger


def valid_context(actor="methakon", method="test_fixture"):
    return TrustedDispatchContext(
        actor_id=actor,
        actor_roles=("operator",),
        auth_method=method,
        auth_subject="test-session",
        dispatcher_id="test_dispatcher",
        issued_at=time.time(),
        request_id=f"req-{os.urandom(6).hex()}",
    )


def base_task(**kwargs):
    t = {
        "task_id": "HG-P0-SPEC-001",
        "source": "issue:26#comment-test",
        "assigned_to": CFG["worker_id"],
        "expected_branch": CFG["branch"],
        "allowed_scope": ["scripts/worker/"],
        "objective": "test task guard boundary",
        "acceptance_tests": ["echo ok"],
        "prohibited_actions": ["oracle_change"],
        "requires_approval": [],
        "approvals": {},
        "author": "methakon",
    }
    t.update(kwargs)
    return t


# --- Case A: Forged task.author='owner' with approval reference ---
def test_case_a():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="attacker")
    task = base_task(
        author="owner",
        action="oracle_change",
        approvals={"oracle_change": "owner:oracle_change:approval-1"},
    )
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case B: Delete/false prohibited_actions or requires_approval ---
def test_case_b():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    # Attacker tries to change oracle by targeting deploy/oracle/ while omitting approval requests
    task = base_task(
        action="oracle_change",
        prohibited_actions=[],
        requires_approval=[],
        approvals={},
    )
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case C: Approval ID for wrong operation/target/device/worker ---
def test_case_c():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-wrong-op",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "handset_change",
        "target_resource": "system",
        "worker_id": CFG["worker_id"],
    })
    task = base_task(
        action="oracle_change",
        requires_approval=["oracle_change"],
        approvals={"oracle_change": "app-wrong-op"},
    )
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case D: Approval record invented in payload or repo demo data ---
def test_case_d():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    task = base_task(
        action="oracle_change",
        requires_approval=["oracle_change"],
        approvals={"oracle_change": "attacker-invented-approval-id-999"},
    )
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case E: Expired, not-yet-valid, revoked, replayed grant ---
def test_case_e_expired():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-exp",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "expires_at": time.time() - 3600,
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-exp"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


def test_case_e_not_yet_valid():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-nyv",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "not_before": time.time() + 3600,
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-nyv"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


def test_case_e_revoked():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-rev",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "revoked_at": time.time() - 100,
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-rev"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


def test_case_e_replayed():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-used",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "max_uses": 1,
        "use_count": 1,
        "used_at": time.time() - 100,
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-used"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case F: Self-approval or unauthorized approver ---
def test_case_f_self():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="owner")
    store.seed_approval({
        "approval_id": "app-self",
        "actor_id": "owner",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
    })
    task = base_task(author="owner", action="oracle_change", approvals={"oracle_change": "app-self"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


def test_case_f_unauthorized_approver():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-bad-approver",
        "actor_id": "methakon",
        "approved_by": "untrusted_user",
        "operation_kind": "oracle_change",
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-bad-approver"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case G: No auth context or spoofed dispatcher headers/fields ---
def test_case_g_no_context():
    td, store, ledger = fresh_env()
    task = base_task(action="oracle_change", author=None, approvals={"oracle_change": "some-id"})
    return validate(task, CFG, ledger, dispatch_context=None, approvals_store=store)


def test_case_g_spoofed_auth_method():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon", method="forged_header_method")
    store.seed_approval({
        "approval_id": "app-g",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
    })
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-g"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case H: Canonical digest mismatch / tamper detection ---
def test_case_h():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-digest",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "canonical_request_digest": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    })
    task = base_task(
        action="oracle_change",
        args={"vm_id": "tampered_target"},
        approvals={"oracle_change": "app-digest"},
    )
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case I: Concurrent identical privileged attempts (AT MOST one execution) ---
def test_case_i():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-race-1",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "max_uses": 1,
    })

    def run_attempt(i):
        task = base_task(task_id=f"HG-P0-RACE-{i}", action="oracle_change", approvals={"oracle_change": "app-race-1"})
        try:
            validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)
            return "ADMIT"
        except Refused as exc:
            return exc.code

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
        f1 = ex.submit(run_attempt, 1)
        f2 = ex.submit(run_attempt, 2)
        outcomes = [f1.result(), f2.result()]

    admit_count = outcomes.count("ADMIT")
    replayed_count = outcomes.count("APPROVAL_REPLAYED")
    if admit_count == 1 and replayed_count == 1:
        return "AT_MOST_ONE_EXECUTION"
    raise Refused("RACE_SAFETY_VIOLATION", f"outcomes: {outcomes}")


# --- Case J: Store locked/read-only/unavailable ---
def test_case_j():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    # Simulate DB corruption / unavailable by pointing to an invalid non-directory path
    broken_store = ApprovalStore("/proc/read_only_fake/approvals.db")
    task = base_task(action="oracle_change", approvals={"oracle_change": "app-1"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=broken_store)


# --- Case K: One valid approved operation (ALLOW / execute once) ---
def test_case_k():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    store.seed_approval({
        "approval_id": "app-valid-k",
        "actor_id": "methakon",
        "approved_by": "owner",
        "operation_kind": "oracle_change",
        "max_uses": 1,
    })
    task = base_task(task_id="HG-P0-VALID-K", action="oracle_change", approvals={"oracle_change": "app-valid-k"})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


# --- Case L: Benign test/fixture operation without approval (ALLOW) ---
def test_case_l():
    td, store, ledger = fresh_env()
    ctx = valid_context(actor="methakon")
    task = base_task(task_id="HG-P0-BENIGN-L", action="test_run", prohibited_actions=["oracle_change"], approvals={})
    return validate(task, CFG, ledger, dispatch_context=ctx, approvals_store=store)


def run_all():
    print("== HG-AG-P0-TRUST-BOUNDARY-34 Adversarial Verification ==")
    record("A", "Forged task.author='owner' with approval", "ACTOR_MISMATCH", test_case_a)
    record("B", "Omit requires_approval on protected op", "APPROVAL_REQUIRED", test_case_b)
    record("C", "Approval ID for wrong operation/target", "APPROVAL_OPERATION_MISMATCH", test_case_c)
    record("D", "Approval ID invented / not in store", "APPROVAL_NOT_VERIFIED", test_case_d)
    record("E1", "Approval record expired", "APPROVAL_EXPIRED", test_case_e_expired)
    record("E2", "Approval record not yet valid", "APPROVAL_NOT_YET_VALID", test_case_e_not_yet_valid)
    record("E3", "Approval record revoked", "APPROVAL_REVOKED", test_case_e_revoked)
    record("E4", "Approval record already replayed/consumed", "APPROVAL_REPLAYED", test_case_e_replayed)
    record("F1", "Self-approval attempted", "SELF_APPROVAL", test_case_f_self)
    record("F2", "Unauthorized approver", "UNAUTHORIZED_APPROVER", test_case_f_unauthorized_approver)
    record("G1", "Missing auth context on protected action", "MISSING_AUTHOR", test_case_g_no_context)
    record("G2", "Spoofed/unrecognized auth method", "INVALID_AUTH_METHOD", test_case_g_spoofed_auth_method)
    record("H", "Canonical digest mismatch", "APPROVAL_DIGEST_MISMATCH", test_case_h)
    record("I", "Two concurrent identical attempts (atomic single-use)", "ADMIT", test_case_i)
    record("J", "Store unavailable/read-only", "STORE_UNAVAILABLE", test_case_j)
    record("K", "One valid approved operation", "ADMIT", test_case_k)
    record("L", "Benign test operation without approval", "ADMIT", test_case_l)

    print()
    print(f"{'Case':50} {'Status':10} {'Details'}")
    print("-" * 100)
    failed = 0
    for name, ok, detail in RESULTS:
        status = "PASS" if ok else "FAIL"
        if not ok:
            failed += 1
        print(f"{name:50} {status:10} {detail}")
    print("-" * 100)
    if failed > 0:
        print(f"FAILED: {failed} of {len(RESULTS)} tests failed")
        sys.exit(1)
    else:
        print(f"PASS: all {len(RESULTS)} adversarial invariants verified successfully")


import unittest


class TestP0TrustBoundary(unittest.TestCase):
    def test_case_a(self):
        with self.assertRaises(Refused) as cm:
            test_case_a()
        self.assertEqual(cm.exception.code, "ACTOR_MISMATCH")

    def test_case_b(self):
        with self.assertRaises(Refused) as cm:
            test_case_b()
        self.assertEqual(cm.exception.code, "APPROVAL_REQUIRED")

    def test_case_c(self):
        with self.assertRaises(Refused) as cm:
            test_case_c()
        self.assertEqual(cm.exception.code, "APPROVAL_OPERATION_MISMATCH")

    def test_case_d(self):
        with self.assertRaises(Refused) as cm:
            test_case_d()
        self.assertEqual(cm.exception.code, "APPROVAL_NOT_VERIFIED")

    def test_case_e_expired(self):
        with self.assertRaises(Refused) as cm:
            test_case_e_expired()
        self.assertEqual(cm.exception.code, "APPROVAL_EXPIRED")

    def test_case_e_not_yet_valid(self):
        with self.assertRaises(Refused) as cm:
            test_case_e_not_yet_valid()
        self.assertEqual(cm.exception.code, "APPROVAL_NOT_YET_VALID")

    def test_case_e_revoked(self):
        with self.assertRaises(Refused) as cm:
            test_case_e_revoked()
        self.assertEqual(cm.exception.code, "APPROVAL_REVOKED")

    def test_case_e_replayed(self):
        with self.assertRaises(Refused) as cm:
            test_case_e_replayed()
        self.assertEqual(cm.exception.code, "APPROVAL_REPLAYED")

    def test_case_f_self(self):
        with self.assertRaises(Refused) as cm:
            test_case_f_self()
        self.assertEqual(cm.exception.code, "SELF_APPROVAL")

    def test_case_f_unauthorized_approver(self):
        with self.assertRaises(Refused) as cm:
            test_case_f_unauthorized_approver()
        self.assertEqual(cm.exception.code, "UNAUTHORIZED_APPROVER")

    def test_case_g_no_context(self):
        with self.assertRaises(Refused) as cm:
            test_case_g_no_context()
        self.assertEqual(cm.exception.code, "MISSING_AUTHOR")

    def test_case_g_spoofed_auth_method(self):
        with self.assertRaises(Refused) as cm:
            test_case_g_spoofed_auth_method()
        self.assertEqual(cm.exception.code, "INVALID_AUTH_METHOD")

    def test_case_h(self):
        with self.assertRaises(Refused) as cm:
            test_case_h()
        self.assertEqual(cm.exception.code, "APPROVAL_DIGEST_MISMATCH")

    def test_case_i(self):
        self.assertEqual(test_case_i(), "AT_MOST_ONE_EXECUTION")

    def test_case_j(self):
        with self.assertRaises(Refused) as cm:
            test_case_j()
        self.assertEqual(cm.exception.code, "STORE_UNAVAILABLE")

    def test_case_k(self):
        res = test_case_k()
        self.assertEqual(res["decision"], "ADMIT")

    def test_case_l(self):
        res = test_case_l()
        self.assertEqual(res["decision"], "ADMIT")


if __name__ == "__main__":
    run_all()
