#!/usr/bin/env python3
"""Additional tests for approval, prohibited actions, and author authentication in task_guard."""
import json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from task_guard import ApprovalStore, Ledger, Refused, validate  # noqa: E402

CFG = json.load(open(os.path.join(HERE, "identity.json")))

def base_task(**over):
    t = {
        "task_id": "HG-SEC-TEST-01",
        "source": "issue:14#comment-123456",
        "assigned_to": CFG["worker_id"],
        "expected_branch": CFG["branch"],
        "allowed_scope": ["scripts/worker/"],
        "objective": "test approval and author checks",
        "acceptance_tests": ["echo ok"],
        "prohibited_actions": ["oracle_change", "handset_change"],
        "requires_approval": [],
        "approvals": {},
        "author": "methakon",
    }
    t.update(over)
    return t

def fresh_ledger():
    return Ledger(os.path.join(tempfile.mkdtemp(prefix="hg-lease-"), "ledger.json"))

def fresh_store():
    td = tempfile.mkdtemp(prefix="hg-appr-store-")
    return ApprovalStore(os.path.join(td, "approvals.db"))

# 1: prohibited action without approval should be rejected
def test_prohibited_without_approval():
    task = base_task(prohibited_actions=["oracle_change"], approvals={})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "PROHIBITED_ACTION", exc.code

# 2: prohibited action with explicit approval should be admitted
def test_prohibited_with_approval():
    task = base_task(prohibited_actions=["oracle_change"], approvals={"oracle_change": "owner:approval-1"})
    try:
        res = validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return res["decision"] == "ADMIT", "ADMIT"
    except Refused as exc:
        return False, exc.code

# 3: missing author field should be rejected
def test_missing_author():
    task = base_task(author=None)
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "MISSING_AUTHOR", exc.code

# 4: unauthorized author should be rejected
def test_unauthorized_author():
    task = base_task(author="random_user")
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "UNAUTHORIZED_AUTHOR", exc.code


def test_unauthorized_approver():
    task = base_task(approvals={"oracle_change": "badguy:approval-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "UNAUTHORIZED_APPROVER", exc.code


def test_self_approval():
    task = base_task(author="owner", approvals={"oracle_change": "owner:approval-1"})
    try:
        validate(task, CFG, fresh_ledger(), approvals_store=fresh_store())
        return False, "ADMIT"
    except Refused as exc:
        return exc.code == "SELF_APPROVAL", exc.code


cases = [
    ("prohibited without approval", "PROHIBITED_ACTION", test_prohibited_without_approval),
    ("prohibited with approval", "ADMIT", test_prohibited_with_approval),
    ("missing author", "MISSING_AUTHOR", test_missing_author),
    ("unauthorized author", "UNAUTHORIZED_AUTHOR", test_unauthorized_author),
    ("unauthorized approver", "UNAUTHORIZED_APPROVER", test_unauthorized_approver),
    ("self approval", "SELF_APPROVAL", test_self_approval),
]

for name, expect, fn in cases:
    ok, code = fn()
    print(f"{name:30} expected {expect:20} got {code:20} {'OK' if ok else 'FAIL'}")
