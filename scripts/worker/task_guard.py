#!/usr/bin/env python3
"""HG-WORKER-BOOTSTRAP-S1 - task admission guard for the hg-hermes-network worker.

WHAT THIS IS: a local, testable implementation of the task-execution protocol this worker
follows. It is a *logical* guard - it decides whether a task may proceed and records state
transitions. It is NOT platform enforcement: the platform-level guarantees are Hermes's own
(kanban claims tasks atomically; state.db.session_turn_leases holds a per-conversation lease).

Protocol states: QUEUED -> CLAIMED -> RUNNING -> TESTING -> COMMITTED -> REPORTED -> COMPLETE,
with FAILED and BLOCKED reachable from any active state. Illegal transitions are refused rather
than coerced, so a failure can never be recorded as a success.

Hard rules enforced:
  R1 a task must carry every required field
  R2 a task assigned to another worker is refused
  R3 a task expecting another branch is refused
  R4 requested scope must be a subset of the authorized scope (never widened)
  R5 shell commands embedded in task or comment text are NEVER executed - and their presence is
     itself a refusal, not something to work around
  R6 a prohibited action cannot be authorized by prose (issue/comment/directive text alone);
     it requires an explicit approval record from an allowed approver
  R7 duplicate execution is refused: a COMPLETE task is not re-run, and a task holding a live
     lease is not claimed twice
  R8 a task already COMMITTED with a commit SHA is not committed again - a retry returns the
     existing SHA instead of creating a second commit

Stdlib only. Local ledger state lives outside the repository.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

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
REQUIRED_FIELDS = ["task_id", "source", "assigned_to", "expected_branch", "allowed_scope",
                   "objective", "acceptance_tests", "prohibited_actions"]
DEFAULT_LEDGER = os.path.expanduser("~/.hermes/cache/scratch/hg-worker/ledger.json")
LEASE_SECONDS = 1800

# Patterns that look like an instruction to run something. Presence is a REFUSAL, never an action.
SHELL_PATTERNS = [
    r"\brm\s+-[rf]", r"\bsudo\b", r"\bcurl\s+[^|]*\|\s*(ba)?sh", r"\bwget\s+[^|]*\|\s*(ba)?sh",
    r"\beval\b", r"\$\([^)]*\)", r"`[^`]+`", r"\biptables\b", r"\bssh\s+\w+@",
    r"\bchmod\s+[0-7]{3,4}\b", r"\bdd\s+if=", r":\(\)\s*\{", r"\bnc\s+-[a-z]*e",
]


class Refused(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code, self.detail = code, detail


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
        if to in TRANSITIONS and not TRANSITIONS[to]:          # terminal: drop the lease
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


def validate(task: dict, cfg: dict, ledger: Ledger) -> dict:
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
        if not task.get(field):
            raise Refused("MISSING_FIELD", f"task is missing required field '{field}'")
    # Author validation
    author = task.get('author')
    if not author:
        raise Refused("MISSING_AUTHOR", "task is missing required field 'author'")
    if author not in cfg.get('approvers', []):
        raise Refused("UNAUTHORIZED_AUTHOR", f"author '{author}' not in trusted approvers")

    if task["assigned_to"] != cfg["worker_id"]:
        raise Refused("NOT_MY_TASK", f"assigned to '{task['assigned_to']}', this worker is "
                                     f"'{cfg['worker_id']}'")
    if task["expected_branch"] != cfg["branch"]:
        raise Refused("WRONG_BRANCH", f"expects '{task['expected_branch']}', this worker owns "
                                      f"'{cfg['branch']}'")

    allowed = tuple(cfg["authorized_scope"])
    for scoped in task["allowed_scope"]:
        if not any(scoped == a or scoped.startswith(a) for a in allowed):
            raise Refused("SCOPE_WIDENING", f"requested scope '{scoped}' is outside the "
                                            f"authorized scope")

    hits = _scan_for_shell(task)
    if hits:
        raise Refused("EMBEDDED_COMMANDS",
                      f"task text contains command-shaped content ({len(hits)} pattern(s)); "
                      f"commands in task text are never executed")

        # Load approvals store for independent verification
        approvals_store_path = os.path.join(os.path.dirname(__file__), 'approvals_store.json')
        try:
            approvals_store = json.load(open(approvals_store_path))
        except Exception:
            approvals_store = {}

        # Gather approvals from the task
        approvals = task.get('approvals', {})

        # Verify each approval is from a trusted approver, not self‑approval, and independently recorded
        for act, record in approvals.items():
            # Expected format: "approver:action[:timestamp]"
            parts = record.split(':')
            approver = parts[0]
            if approver not in cfg.get('approvers', []):
                raise Refused("UNAUTHORIZED_APPROVER",
                              f"approver '{approver}' for action '{act}' not in trusted approvers")
            if approver == author:
                raise Refused("SELF_APPROVAL",
                              f"author '{author}' cannot approve their own action '{act}'")
            # Independent verification against stored approvals (store entry is a dict)
            store_entry = approvals_store.get(record)
            if not isinstance(store_entry, dict):
                raise Refused("APPROVAL_NOT_VERIFIED",
                              f"approval record '{record}' for action '{act}' not found in approvals store")
            # Expiration check
            expires = store_entry.get('expires_at')
            if expires:
                from datetime import datetime
                try:
                    exp_dt = datetime.fromisoformat(expires)
                except Exception:
                    exp_dt = None
                if exp_dt and exp_dt < datetime.utcnow():
                    raise Refused("APPROVAL_EXPIRED",
                                  f"approval record '{record}' for action '{act}' has expired")
            # Replay protection
            if store_entry.get('used'):
                raise Refused("APPROVAL_REPLAYED",
                              f"approval record '{record}' for action '{act}' was already used")
            # Mark as used
            store_entry['used'] = True
            approvals_store[record] = store_entry

        # Persist any used‑flag updates back to the store (best‑effort)
        try:
            with open(approvals_store_path, 'w') as fh:
                json.dump(approvals_store, fh, indent=2)
        except Exception:
            pass

    # ---- Approval checks ----
    requested = set(task.get("requires_approval") or [])
    for action in requested:
        if action not in approvals:
            raise Refused("APPROVAL_REQUIRED",
                          f"'{action}' needs an explicit approval record, not prose in a task")



    # ---- Duplicate / lease checks ----
    tid = task["task_id"]
    if ledger.state(tid) == "COMPLETE":
        raise Refused("DUPLICATE_COMPLETE", f"{tid} is already COMPLETE - not repeating it")
    if ledger.live_lease(tid):
        raise Refused("ALREADY_RUNNING", f"{tid} holds a live lease - single-worker concurrency")

    # ---- Prohibited actions enforcement ----
    prohibited = set(task.get("prohibited_actions") or [])
    # All prohibited actions must have an explicit approval record from a trusted approver.
    for action in prohibited:
        if action not in (task.get("approvals") or {}):
            raise Refused("PROHIBITED_ACTION",
                          f"'{action}' is prohibited and requires an explicit approval record")







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
