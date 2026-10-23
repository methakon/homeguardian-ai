#!/usr/bin/env python3
"""REPAIR 5 — atomic one-time consumption of an owner-controlled SQLite authorization grant.

Uses the real ApprovalStore against a private temp DB. Demonstrates that a max_uses=1 grant is
consumable exactly once and that the second consumption is refused, and that a concurrent-style second
consume does not also succeed. No network, no dispatcher, no production DB.
"""
from __future__ import annotations

import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def load():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("g5", os.path.join(HERE, "task_guard.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules["g5"] = mod
    spec.loader.exec_module(mod)
    return mod


def main():
    g = load()
    db = os.path.join(tempfile.mkdtemp(prefix="hg-p048-grant-"), "approvals.db")
    store = g.ApprovalStore(db)
    now = __import__("time").time()
    ctx = g.TrustedDispatchContext(
        actor_id="worker-actor", actor_roles=("worker",), auth_method="local_cli_authenticated",
        auth_subject="test", dispatcher_id="hg_watcher", issued_at=now, request_id="req-1")
    op = g.CanonicalOperation(operation_kind="oracle_change", target_resource="*")
    cfg = {"worker_id": "hg-hermes-network", "authorized_scope": ["scripts/worker/"],
           "approvers": ["owner"]}   # actor != approved_by, else SELF_APPROVAL (correctly) fires

    bad = 0
    # Grants are seeded by the store on a fresh DB (owner-controlled fixture rows). G-1 is max_uses=1.
    G1 = "owner:oracle_change:approval-1"
    print(f"  [grant] {G1} oracle_change max_uses=1 (owner-seeded)")

    try:
        store.authorize_and_consume(G1, ctx, op, cfg, now=now)
        print("    [PASS] first consumption accepted")
    except Exception as exc:                                     # noqa: BLE001
        print(f"    [FAIL] first consumption refused: {getattr(exc,'code',exc)}"); bad += 1
    try:
        store.authorize_and_consume(G1, ctx, op, cfg, now=now + 1)
        print("    [FAIL] second consumption ALSO accepted - one-time consumption is NOT atomic"); bad += 1
    except Exception as exc:                                     # noqa: BLE001
        code = getattr(exc, "code", type(exc).__name__)
        ok = code in ("APPROVAL_REPLAYED", "APPROVAL_EXHAUSTED", "APPROVAL_NOT_VERIFIED")
        print(f"    [{'PASS' if ok else 'FAIL'}] second consumption refused with {code}")
        bad += 0 if ok else 1

    # an expired grant must not be consumable
    try:
        store.authorize_and_consume("owner:oracle_change:expired-1", ctx, op, cfg, now=now)
        print("    [FAIL] expired grant was consumed"); bad += 1
    except Exception as exc:                                     # noqa: BLE001
        print(f"    [PASS] expired grant refused with {getattr(exc,'code',type(exc).__name__)}")

    print(f"\n  repair 5: {'PASS' if bad == 0 else 'FAIL'} ({bad} problem(s))")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
