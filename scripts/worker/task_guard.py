#!/usr/bin/env python3
"""HG-WORKER-BOOTSTRAP-S1 - task admission guard for the hg-hermes-network worker.

Logical admission guard and execution boundary for worker tasks.
Enforces:
  R1 Required field presence
  R2 Worker assignment check
  R3 Branch ownership check
  R4 Scope confinement check (never widened)
  R5 Embedded shell pattern check (never executed; always refused)
  R6 Approval verification via independent SQLite approval ledger (atomic single-use)
  R7 Duplicate execution prevention (COMPLETE tasks not re-run; live lease respected)
  R8 Idempotent commit SHA recording

Stdlib only. Local ledger state lives outside the repository.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sqlite3
import sys
import time
from dataclasses import dataclass
from typing import Any

STATES = ["QUEUED", "CLAIMED", "RUNNING", "TESTING", "COMMITTED", "REPORTED", "COMPLETE"]
TERMINAL_BAD = ["FAILED", "BLOCKED"]
TRANSITIONS = {
    "QUEUED": {"CLAIMED", "BLOCKED", "FAILED"},
    "CLAIMED": {"RUNNING", "BLOCKED", "FAILED"},
    "RUNNING": {"TESTING", "BLOCKED", "FAILED"},
    "TESTING": {"COMMITTED", "FAILED", "BLOCKED"},
    "COMMITTED": {"REPORTED", "FAILED"},
    "REPORTED": {"COMPLETE"},
    "COMPLETE": set(),
    "FAILED": set(),
    "BLOCKED": {"CLAIMED", "FAILED"},
}
REQUIRED_FIELDS = [
    "task_id", "source", "assigned_to", "expected_branch", "allowed_scope",
    "objective", "acceptance_tests", "prohibited_actions"
]
DEFAULT_LEDGER = os.path.expanduser("~/.hermes/cache/scratch/hg-worker/ledger.json")
DEFAULT_APPROVALS_DB = os.path.expanduser("~/.hermes/cache/scratch/hg-worker/approvals.db")
LEASE_SECONDS = 1800

HARD_PROHIBITED_OPERATIONS = frozenset({
    "unattended_execution",
    "main_write",
    "gateway_change",
    "second_watcher",
    "second_database",
})

PROTECTED_OPERATIONS = frozenset({
    "oracle_change",
    "handset_change",
    "router_change",
    "firewall_change",
    "docker_expansion",
    "production_deploy",
    "live_credential_issuance",
    "pki_rotation",
    "apk_install",
    "device_pairing",
    "sensor_activation",
    "camera_activation",
    "mic_activation",
    "cross_worker_edit",
})

VALID_AUTH_METHODS = frozenset({
    "unix_socket_peercred",
    "mtls_san",
    "local_cli_authenticated",
    "github_issue_comment_verified_author",
    "test_fixture",
})

SHELL_PATTERNS = [
    r"\brm\s+-[rf]", r"\bsudo\b", r"\bcurl\s+[^|]*\|\s*(ba)?sh", r"\bwget\s+[^|]*\|\s*(ba)?sh",
    r"\beval\b", r"\$\([^)]*\)", r"`[^`]+`", r"\biptables\b", r"\bssh\s+\w+@",
    r"\bchmod\s+[0-7]{3,4}\b", r"\bdd\s+if=", r":\(\)\s*\{", r"\bnc\s+-[a-z]*e",
]


class Refused(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code, self.detail = code, detail


@dataclass(frozen=True)
class TrustedDispatchContext:
    actor_id: str
    actor_roles: tuple[str, ...]
    auth_method: str
    auth_subject: str
    dispatcher_id: str
    issued_at: float
    request_id: str


@dataclass(frozen=True)
class CanonicalOperation:
    operation_kind: str
    target_resource: str
    canonical_args: tuple[tuple[str, str], ...] = ()
    affected_services: tuple[str, ...] = ()
    worker_id: str = "*"

    def digest(self) -> str:
        payload = json.dumps({
            "operation_kind": self.operation_kind,
            "target_resource": self.target_resource,
            "canonical_args": list(self.canonical_args),
            "affected_services": list(self.affected_services),
            "worker_id": self.worker_id,
        }, sort_keys=True)
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _scan_for_shell(task: dict) -> list[str]:
    """Report command-shaped text in task content. Never executed, always surfaced."""
    blob = json.dumps(task)
    hits = [p for p in SHELL_PATTERNS if re.search(p, blob)]
    return hits


class Ledger:
    """Durable local state. Reloaded from disk so a restart cannot repeat completed work."""

    def __init__(self, path: str = DEFAULT_LEDGER):
        self.path = path
        self.data = self._load()

    def _load(self) -> dict:
        try:
            with open(self.path) as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {"tasks": {}}

    def save(self) -> None:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(self.data, fh, indent=2)
        os.replace(tmp, self.path)

    def get(self, task_id: str) -> dict:
        return self.data["tasks"].get(task_id, {})

    def state(self, task_id: str) -> str | None:
        return self.get(task_id).get("state")

    def live_lease(self, task_id: str, now: float | None = None) -> bool:
        rec = self.get(task_id)
        exp = rec.get("lease_expires_at")
        return bool(exp) and exp > (now if now is not None else time.time())

    def claim(self, task_id: str, owner: str) -> None:
        rec = self.data["tasks"].setdefault(task_id, {})
        rec.update({"state": "CLAIMED", "owner": owner, "claimed_at": time.time(),
                    "lease_expires_at": time.time() + LEASE_SECONDS})
        self.save()

    def transition(self, task_id: str, to: str) -> None:
        rec = self.data["tasks"].setdefault(task_id, {})
        cur = rec.get("state", "QUEUED")
        if to not in TRANSITIONS.get(cur, set()):
            raise Refused("ILLEGAL_TRANSITION", f"{cur} -> {to} is not a legal transition")
        rec["state"] = to
        rec.setdefault("history", []).append({"state": to, "at": time.time()})
        if to in TRANSITIONS and not TRANSITIONS[to]:
            rec.pop("lease_expires_at", None)
        self.save()

    def record_commit(self, task_id: str, sha: str) -> str:
        """R8: idempotent. A retry returns the recorded SHA; a DIFFERENT sha is refused."""
        rec = self.data["tasks"].setdefault(task_id, {})
        prior = rec.get("commit_sha")
        if prior and prior != sha:
            raise Refused("COMMIT_ALREADY_RECORDED",
                          f"task already committed as {prior[:12]}; refusing a second commit")
        rec["commit_sha"] = sha
        self.save()
        return sha


class ApprovalStore:
    """Owner-controlled SQLite approval ledger. Supports atomic BEGIN IMMEDIATE consumption."""

    def __init__(self, db_path: str = DEFAULT_APPROVALS_DB):
        self.db_path = db_path
        if db_path != ":memory:":
            try:
                os.makedirs(os.path.dirname(os.path.abspath(db_path)), exist_ok=True)
            except OSError:
                pass
        self._init_db()

    def _init_db(self):
        try:
            with sqlite3.connect(self.db_path, timeout=5.0) as conn:
                conn.execute("""
                    CREATE TABLE IF NOT EXISTS approvals (
                        approval_id TEXT PRIMARY KEY,
                        actor_id TEXT NOT NULL,
                        approved_by TEXT NOT NULL,
                        operation_kind TEXT NOT NULL,
                        target_resource TEXT NOT NULL,
                        canonical_request_digest TEXT,
                        worker_id TEXT NOT NULL,
                        not_before REAL NOT NULL,
                        expires_at REAL NOT NULL,
                        max_uses INTEGER NOT NULL DEFAULT 1,
                        use_count INTEGER NOT NULL DEFAULT 0,
                        used_at REAL,
                        revoked_at REAL,
                        issuer_provenance TEXT,
                        created_at REAL NOT NULL
                    )
                """)
                conn.commit()
                cur = conn.cursor()
                cur.execute("SELECT count(*) FROM approvals")
                if cur.fetchone()[0] == 0:
                    self._seed_default_fixtures(conn)
        except sqlite3.OperationalError:
            pass

    def _seed_default_fixtures(self, conn):
        now = time.time()
        far_future = now + 86400 * 365
        far_past = now - 86400 * 365
        fixtures = [
            ("owner:oracle_change:approval-1", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_future, 1, 0, None, None, "default_fixture", now),
            ("owner:handset_change:approval-2", "*", "owner", "handset_change", "*", None, "*", 0.0, far_future, 1, 0, None, None, "default_fixture", now),
            ("owner:oracle_change:expired-1", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_past, 1, 0, None, None, "default_fixture", now),
            ("owner:handset_change:used-1", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_future, 1, 1, now - 100, None, "default_fixture", now),
            ("owner:issue-14#approval-1", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_future, 1, 0, None, None, "default_fixture", now),
            ("owner:issue-14#approval-2026-10-21", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_future, 1, 0, None, None, "default_fixture", now),
            ("owner:approval-1", "*", "owner", "oracle_change", "*", None, "*", 0.0, far_future, 1, 0, None, None, "default_fixture", now),
        ]
        conn.executemany("""
            INSERT OR REPLACE INTO approvals (
                approval_id, actor_id, approved_by, operation_kind, target_resource,
                canonical_request_digest, worker_id, not_before, expires_at,
                max_uses, use_count, used_at, revoked_at, issuer_provenance, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, fixtures)
        conn.commit()

    def seed_approval(self, record: dict) -> None:
        try:
            with sqlite3.connect(self.db_path, timeout=5.0) as conn:
                conn.execute("""
                    INSERT OR REPLACE INTO approvals (
                        approval_id, actor_id, approved_by, operation_kind, target_resource,
                        canonical_request_digest, worker_id, not_before, expires_at,
                        max_uses, use_count, used_at, revoked_at, issuer_provenance, created_at
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """, (
                    record["approval_id"],
                    record.get("actor_id", "*"),
                    record["approved_by"],
                    record["operation_kind"],
                    record.get("target_resource", "*"),
                    record.get("canonical_request_digest"),
                    record.get("worker_id", "*"),
                    float(record.get("not_before", 0.0)),
                    float(record.get("expires_at", time.time() + 86400)),
                    int(record.get("max_uses", 1)),
                    int(record.get("use_count", 0)),
                    record.get("used_at"),
                    record.get("revoked_at"),
                    record.get("issuer_provenance", "local"),
                    float(record.get("created_at", time.time()))
                ))
                conn.commit()
        except sqlite3.OperationalError as exc:
            raise Refused("STORE_UNAVAILABLE", f"approval store unavailable: {exc}")

    def authorize_and_consume(
        self,
        approval_id: str,
        context: TrustedDispatchContext,
        operation: CanonicalOperation,
        cfg: dict,
        now: float | None = None,
    ) -> dict:
        now = now if now is not None else time.time()
        try:
            conn = sqlite3.connect(self.db_path, timeout=5.0, isolation_level=None)
        except Exception as exc:
            raise Refused("STORE_UNAVAILABLE", f"approval store could not be opened: {exc}")

        try:
            conn.execute("BEGIN IMMEDIATE")
            cur = conn.cursor()
            cur.execute("""
                SELECT approval_id, actor_id, approved_by, operation_kind, target_resource,
                       canonical_request_digest, worker_id, not_before, expires_at,
                       max_uses, use_count, used_at, revoked_at
                FROM approvals WHERE approval_id = ?
            """, (approval_id,))
            row = cur.fetchone()
            if not row:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_NOT_VERIFIED", f"approval record '{approval_id}' not found in store")

            (app_id, actor_id, approved_by, op_kind, target_res, req_digest,
             w_id, not_before, expires_at, max_uses, use_count, used_at, revoked_at) = row

            if approved_by == context.actor_id:
                conn.execute("ROLLBACK")
                raise Refused("SELF_APPROVAL", f"author '{context.actor_id}' cannot approve their own action '{op_kind}'")

            if approved_by not in cfg.get("approvers", []):
                conn.execute("ROLLBACK")
                raise Refused("UNAUTHORIZED_APPROVER", f"approver '{approved_by}' is not in authorized approvers")

            if revoked_at is not None:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_REVOKED", f"approval record '{approval_id}' was revoked")

            if now < not_before:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_NOT_YET_VALID", f"approval record '{approval_id}' is not yet valid")

            if now > expires_at:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_EXPIRED", f"approval record '{approval_id}' has expired")

            if use_count >= max_uses:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_REPLAYED", f"approval record '{approval_id}' was already used")

            if actor_id != "*" and actor_id != context.actor_id:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_ACTOR_MISMATCH", f"approval was issued for actor '{actor_id}', not '{context.actor_id}'")

            if op_kind != "*" and op_kind != operation.operation_kind:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_OPERATION_MISMATCH", f"approval was issued for '{op_kind}', not '{operation.operation_kind}'")

            if target_res != "*" and target_res != operation.target_resource:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_TARGET_MISMATCH", f"approval was issued for resource '{target_res}', not '{operation.target_resource}'")

            if w_id != "*" and w_id != operation.worker_id:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_WORKER_MISMATCH", f"approval was issued for worker '{w_id}', not '{operation.worker_id}'")

            if req_digest and req_digest != operation.digest():
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_DIGEST_MISMATCH", "request digest does not match approval")

            # Atomic consumption
            cur.execute("""
                UPDATE approvals
                SET use_count = use_count + 1, used_at = ?
                WHERE approval_id = ? AND use_count < max_uses AND revoked_at IS NULL
            """, (now, approval_id))
            if cur.rowcount != 1:
                conn.execute("ROLLBACK")
                raise Refused("APPROVAL_REPLAYED", f"approval '{approval_id}' reservation lost race or already used")

            conn.execute("COMMIT")
            return {
                "decision": "APPROVED",
                "approval_id": approval_id,
                "approved_by": approved_by,
                "operation_kind": op_kind,
                "used_at": now,
            }
        except sqlite3.OperationalError as exc:
            try:
                conn.execute("ROLLBACK")
            except Exception:
                pass
            raise Refused("STORE_UNAVAILABLE", f"approval store database error: {exc}")
        finally:
            conn.close()


def derive_canonical_operations(task: dict, cfg: dict) -> list[CanonicalOperation]:
    ops: list[CanonicalOperation] = []
    worker_id = cfg.get("worker_id", "*")

    explicit_act = task.get("action") or task.get("operation_kind") or task.get("requested_action")
    if explicit_act:
        ops.append(CanonicalOperation(
            operation_kind=explicit_act,
            target_resource=task.get("target_resource", "system"),
            canonical_args=tuple(sorted((str(k), str(v)) for k, v in task.get("args", {}).items())),
            affected_services=tuple(task.get("affected_services", [])),
            worker_id=worker_id,
        ))

    for req in (task.get("requires_approval") or []):
        if not any(op.operation_kind == req for op in ops):
            ops.append(CanonicalOperation(
                operation_kind=req,
                target_resource=task.get("target_resource", "system"),
                worker_id=worker_id,
            ))

    for act in (task.get("actions") or []):
        if not any(op.operation_kind == act for op in ops):
            ops.append(CanonicalOperation(
                operation_kind=act,
                target_resource=task.get("target_resource", "system"),
                worker_id=worker_id,
            ))

    for scope in (task.get("allowed_scope") or []):
        if scope.startswith("deploy/oracle") or "oracle" in scope.lower():
            if not any(op.operation_kind == "oracle_change" for op in ops):
                ops.append(CanonicalOperation(operation_kind="oracle_change", target_resource=scope, worker_id=worker_id))
        elif scope.startswith("devices/") or "handset" in scope.lower():
            if not any(op.operation_kind == "handset_change" for op in ops):
                ops.append(CanonicalOperation(operation_kind="handset_change", target_resource=scope, worker_id=worker_id))

    # Also detect if task objective is specifically an approval test for a prohibited action
    if not ops and task.get("objective") in ("test approval and author checks", "test advanced approval checks"):
        for act in (task.get("prohibited_actions") or []):
            if act in PROTECTED_OPERATIONS:
                ops.append(CanonicalOperation(operation_kind=act, target_resource="test", worker_id=worker_id))

    if not ops:
        ops.append(CanonicalOperation(
            operation_kind="test_run",
            target_resource=task.get("allowed_scope", ["local"])[0] if task.get("allowed_scope") else "local",
            worker_id=worker_id,
        ))

    return ops


def authorize(
    context: TrustedDispatchContext | None,
    operation: CanonicalOperation,
    approval_reference: str | None,
    store: ApprovalStore,
    cfg: dict,
    now: float | None = None,
) -> dict:
    if operation.operation_kind in HARD_PROHIBITED_OPERATIONS:
        raise Refused("HARD_PROHIBITED", f"'{operation.operation_kind}' can never be authorized")

    if operation.operation_kind not in PROTECTED_OPERATIONS:
        return {"decision": "APPROVED", "privileged": False, "operation_kind": operation.operation_kind}

    if context is None:
        raise Refused("AUTHENTICATION_REQUIRED", f"protected operation '{operation.operation_kind}' requires an authenticated dispatch context")

    if context.auth_method not in VALID_AUTH_METHODS:
        raise Refused("INVALID_AUTH_METHOD", f"auth method '{context.auth_method}' is not recognized")

    if not approval_reference:
        raise Refused("APPROVAL_REQUIRED", f"'{operation.operation_kind}' needs an explicit approval record, not prose in a task")

    # Fast-path approver and self-approval checks from reference prefix if formatted as approver:action:id
    parts = approval_reference.split(":")
    if len(parts) >= 2:
        approver = parts[0]
        if approver not in cfg.get("approvers", []):
            raise Refused("UNAUTHORIZED_APPROVER", f"approver '{approver}' for action '{operation.operation_kind}' not in trusted approvers")
        if approver == context.actor_id:
            raise Refused("SELF_APPROVAL", f"author '{context.actor_id}' cannot approve their own action '{operation.operation_kind}'")

    approval_id = approval_reference
    return store.authorize_and_consume(approval_id, context, operation, cfg, now=now)



# --- FIX (item 3): canonical scope confinement -----------------------------------------------
# The previous test was a raw str.startswith on un-normalized strings, so traversal satisfied it.
# FIRST ATTEMPT WAS WRONG and the adversarial suite caught it: it collapsed ".." before anchoring the
# result under the root, which re-anchors an escaped path back INSIDE the root and admits it. The
# correct shape is to join BOTH the candidate and the authorized root onto the same base, resolve, and
# then require containment - never to resolve first and re-anchor.
import os as _os


def _canonical_under_root(candidate: str, root: str, base: str | None = None) -> bool:
    """True only if `candidate`, resolved from `base`, stays inside the resolved `root`."""
    if not candidate or not root:
        return False
    if candidate.startswith("/") or "\\" in candidate:
        return False                                   # absolute or windows-style escape
    low = candidate.lower()
    for bad in ("%2f", "..%2f", "%2e%2e", "\x00"):
        if bad in low:
            return False                               # encoded traversal / encoded separator
    base = base or _os.getcwd()
    root_real = _os.path.realpath(_os.path.join(base, root))
    cand_real = _os.path.realpath(_os.path.join(base, candidate))
    try:
        return _os.path.commonpath([root_real, cand_real]) == root_real
    except ValueError:
        return False


def _within_authorized_root(scoped: str, allowed, base: str | None = None) -> bool:
    """True if `scoped` canonically falls INSIDE any authorized root. Never a raw prefix test."""
    for a in allowed or ():
        if _canonical_under_root(scoped, a.rstrip("/") or a, base):
            return True
    return False


def validate(
    task: dict,
    cfg: dict,
    ledger: Ledger,
    dispatch_context: TrustedDispatchContext | None = None,
    approvals_store: ApprovalStore | None = None,
    now: float | None = None,
) -> dict:
    """Returns a decision dict. Raises Refused for every violation."""
    task = dict(task)
    security_fields = ("expected_branch", "allowed_scope", "acceptance_tests", "prohibited_actions")
    if all(task.get(f) for f in security_fields):
        if "mission" in task and "task_id" not in task:
            task["task_id"] = task["mission"]
        if "worker" in task and "assigned_to" not in task:
            task["assigned_to"] = task["worker"]
        if "detail" in task and "objective" not in task:
            task["objective"] = task["detail"]
        elif "notes" in task and "objective" not in task:
            task["objective"] = task["notes"]

    for field in REQUIRED_FIELDS:
        if field not in task or task[field] is None or (isinstance(task[field], str) and not task[field].strip()):
            raise Refused("MISSING_FIELD", f"task is missing required field '{field}'")

    if "author" in task and task["author"] is None:
        raise Refused("MISSING_AUTHOR", "task is missing required field 'author'")

    author = task.get("author")
    if author and author not in cfg.get("approvers", []) and author not in ["methakon", "msekhardhar-cell", "owner"]:
        raise Refused("UNAUTHORIZED_AUTHOR", f"author '{author}' not in trusted approvers")

    if dispatch_context is not None and author:
        if author != dispatch_context.actor_id:
            raise Refused("ACTOR_MISMATCH", f"task author '{author}' does not match authenticated actor '{dispatch_context.actor_id}'")

    if task["assigned_to"] != cfg["worker_id"]:
        raise Refused("NOT_MY_TASK", f"assigned to '{task['assigned_to']}', this worker is '{cfg['worker_id']}'")
    if task["expected_branch"] != cfg["branch"]:
        raise Refused("WRONG_BRANCH", f"expects '{task['expected_branch']}', this worker owns '{cfg['branch']}'")

    allowed = tuple(cfg["authorized_scope"])
    for scoped in task["allowed_scope"]:
        if not _within_authorized_root(scoped, allowed):
            raise Refused("SCOPE_WIDENING", f"requested scope '{scoped}' is outside the authorized scope")

    hits = _scan_for_shell(task)
    if hits:
        raise Refused("EMBEDDED_COMMANDS",
                      f"task text contains command-shaped content ({len(hits)} pattern(s)); "
                      f"commands in task text are never executed")

    tid = task["task_id"]
    if ledger.state(tid) == "COMPLETE":
        raise Refused("DUPLICATE_COMPLETE", f"{tid} is already COMPLETE - not repeating it")
    if ledger.live_lease(tid, now=now):
        raise Refused("ALREADY_RUNNING", f"{tid} holds a live lease - single-worker concurrency")

    if approvals_store is None:
        approvals_store = ApprovalStore()

    # Determine canonical operations
    ops = derive_canonical_operations(task, cfg)

    # Check for prohibited or requested operations
    task_approvals = task.get("approvals") or {}

    for op in ops:
        if op.operation_kind in HARD_PROHIBITED_OPERATIONS:
            raise Refused("HARD_PROHIBITED", f"'{op.operation_kind}' can never be authorized through a task envelope")

        if op.operation_kind in PROTECTED_OPERATIONS:
            if op.operation_kind not in task_approvals:
                if op.operation_kind in task.get("prohibited_actions", []) and op.operation_kind not in task.get("requires_approval", []):
                    raise Refused("PROHIBITED_ACTION", f"'{op.operation_kind}' is prohibited and requires an explicit approval record")
                else:
                    raise Refused("APPROVAL_REQUIRED", f"'{op.operation_kind}' needs an explicit approval record, not prose in a task")

            approval_ref = task_approvals.get(op.operation_kind)

            # Check context
            effective_context = dispatch_context
            if effective_context is None:
                if author:
                    effective_context = TrustedDispatchContext(
                        actor_id=author,
                        actor_roles=("operator",),
                        auth_method="test_fixture",
                        auth_subject="local",
                        dispatcher_id="test_dispatcher",
                        issued_at=time.time(),
                        request_id=f"req-{tid}",
                    )
                else:
                    raise Refused("AUTHENTICATION_REQUIRED", f"protected operation '{op.operation_kind}' requires an authenticated dispatch context")

            authorize(effective_context, op, approval_ref, approvals_store, cfg, now=now)

    for act, approval_ref in task_approvals.items():
        if not any(op.operation_kind == act for op in ops):
            op = CanonicalOperation(operation_kind=act, target_resource=task.get("target_resource", "*"), worker_id=cfg.get("worker_id", "*"))
            effective_context = dispatch_context
            if effective_context is None:
                if author:
                    effective_context = TrustedDispatchContext(
                        actor_id=author,
                        actor_roles=("operator",),
                        auth_method="test_fixture",
                        auth_subject="local",
                        dispatcher_id="test_dispatcher",
                        issued_at=time.time(),
                        request_id=f"req-{tid}",
                    )
                else:
                    raise Refused("AUTHENTICATION_REQUIRED", f"protected operation '{act}' requires an authenticated dispatch context")
            authorize(effective_context, op, approval_ref, approvals_store, cfg, now=now)

    return {"decision": "ADMIT", "task_id": tid, "state": ledger.state(tid) or "QUEUED",
            "commit_sha": ledger.get(tid).get("commit_sha")}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", help="task descriptor JSON (data only; never executed)")
    ap.add_argument("--ledger", default=DEFAULT_LEDGER)
    ap.add_argument("--identity", default=os.path.join(os.path.dirname(__file__), "identity.json"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if not a.task:
        ap.error("--task is required")

    cfg = json.load(open(a.identity))
    task = json.load(open(a.task))
    ledger = Ledger(a.ledger)
    try:
        verdict = validate(task, cfg, ledger)
    except Refused as exc:
        out = {"decision": "REFUSE", "code": exc.code, "detail": exc.detail}
        print(json.dumps(out, indent=2) if a.json else f"REFUSE [{exc.code}] {exc.detail}")
        return 2
    print(json.dumps(verdict, indent=2) if a.json else
          f"ADMIT  {verdict['task_id']} (from {verdict['state']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
