#!/usr/bin/env python3
"""Extended security tests for task_guard approval store semantics.
Covers expiration, replay protection, and missing verification.
"""
import json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from task_guard import ApprovalStore, Ledger, Refused, validate  # noqa: E402

CFG = json.load(open(os.path.join(HERE, "identity.json")))

def base_task(**over):
    t = {
        "task_id": "HG-SEC-TEST-02",
        "source": "issue:14#comment-123456",
        "assigned_to": CFG["worker_id"],
        "expected_branch": CFG["branch"],
        "allowed_scope": ["scripts/worker/"],
        "objective": "test advanced approval checks",
        "acceptance_tests": ["echo ok"],
        "prohibited_actions": ["oracle_change"],
        "requires_approval": [],
        "approvals": {},
        "author": "methakon",
    }
    t.update(over)
    return t

def fresh_ledger():
    return Ledger(os.path.join(tempfile.mkdtemp(prefix="hg-lease-"), "ledger.json"))

def fresh_store():
    td = tempfile.mkdtemp(prefix="hg-sec-store-")
    return ApprovalStore(os.path.join(td, "approvals.db"))

# 1: approval record not present in store → APPROVAL_NOT_VERIFIED
def test_approval_not_verified():
    task = base_task(approvals={"oracle_change": "owner:oracle_change:missing"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "APPROVAL_NOT_VERIFIED", exc.code

# 2: approval has expired → APPROVAL_EXPIRED
def test_approval_expired():
    task = base_task(approvals={"oracle_change": "owner:oracle_change:expired-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "APPROVAL_EXPIRED", exc.code

# 3: approval already used → APPROVAL_REPLAYED
def test_approval_replayed():
    task = base_task(approvals={"oracle_change": "owner:handset_change:used-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "APPROVAL_REPLAYED", exc.code

# 4: unauthorized approver (already covered elsewhere but repeat for completeness)
def test_unauthorized_approver():
    task = base_task(approvals={"oracle_change": "badguy:oracle_change:approval-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "UNAUTHORIZED_APPROVER", exc.code

# 5: self‑approval (already covered elsewhere but repeat)
def test_self_approval():
    task = base_task(author="owner", approvals={"oracle_change": "owner:oracle_change:approval-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "SELF_APPROVAL", exc.code

# 6: prohibited action without approval → PROHIBITED_ACTION
def test_prohibited_without_approval():
    task = base_task(prohibited_actions=["oracle_change"], approvals={})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "PROHIBITED_ACTION", exc.code

# 7: prohibited action with valid approval → ADMIT
def test_prohibited_with_approval():
    task = base_task(prohibited_actions=["oracle_change"], approvals={"oracle_change": "owner:oracle_change:approval-1"})
    try:
        res = validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return res["decision"] == "ADMIT", "ADMIT"
    except Refused as exc:
        return False, exc.code

cases = [
    ("approval not verified", "APPROVAL_NOT_VERIFIED", test_approval_not_verified),
    ("approval expired", "APPROVAL_EXPIRED", test_approval_expired),
    ("approval replayed", "APPROVAL_REPLAYED", test_approval_replayed),
    ("unauthorized approver", "UNAUTHORIZED_APPROVER", test_unauthorized_approver),
    ("self approval", "SELF_APPROVAL", test_self_approval),
    ("prohibited without approval", "PROHIBITED_ACTION", test_prohibited_without_approval),
    ("prohibited with approval", "ADMIT", test_prohibited_with_approval),
]

for name, expect, fn in cases:
    ok, code = fn()
    print(f"{name:30} expected {expect:20} got {code:20} {'OK' if ok else 'FAIL'}")
