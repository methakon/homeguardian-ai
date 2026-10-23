#!/usr/bin/env python3
"""HG-WORKER-WATCHER-ACTIVATION-S1 - trusted task-envelope parsing.

A GitHub issue comment is UNTRUSTED INPUT. On a public repository anyone can comment, so the
channel that delivers tasks is also the channel an attacker would use to deliver instructions.
This module is the boundary:

  * only a fenced ```hg-task block is treated as a task - PROSE IS NEVER INSTRUCTIONS;
  * the comment's AUTHOR must be on an allowlist, otherwise nothing in it is acted on;
  * the envelope must carry every required field;
  * command-shaped text anywhere in the envelope is a REFUSAL, never something to execute;
  * actions listed as hard-prohibited can never be authorized by an envelope at all, no matter
    who wrote it - they need an owner decision outside this channel;
  * everything else is delegated to task_guard, so scope, branch, approval and duplicate rules
    have exactly one implementation.

Fail-closed: any ambiguity (missing tag, malformed JSON, unknown author) refuses. There is no
path from a comment to an executed command.

Stdlib only. Run: python3 scripts/worker/task_envelope.py --comment <json> [--json]
The comment JSON is data only: {"author": {"login": "..."}, "body": "..."}
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from task_guard import ApprovalStore, Ledger, Refused, TrustedDispatchContext, validate  # noqa: E402

DEFAULT_POLICY = os.path.join(HERE, "watcher-policy.json")
DEFAULT_IDENTITY = os.path.join(HERE, "identity.json")


class EnvelopeRefused(Exception):
    def __init__(self, code: str, detail: str):
        super().__init__(f"{code}: {detail}")
        self.code, self.detail = code, detail


def extract_envelope(body: str, tag: str) -> dict:
    """Pull the single fenced task block out of a comment body. Prose is ignored entirely."""
    if not body or not body.strip():
        raise EnvelopeRefused("NO_ENVELOPE", "comment body is empty")
    blocks = re.findall(r"```" + re.escape(tag) + r"[ \t]*\r?\n(.*?)```", body, re.S)
    if not blocks:
        raise EnvelopeRefused("NO_ENVELOPE",
                              f"no ```{tag} block found - a comment without a structured envelope "
                              f"is prose and is never executed")
    if len(blocks) > 1:
        raise EnvelopeRefused("AMBIGUOUS_ENVELOPE",
                              f"{len(blocks)} ```{tag} blocks - exactly one is required")
    try:
        env = json.loads(blocks[0])
    except ValueError as exc:
        raise EnvelopeRefused("MALFORMED_ENVELOPE", f"envelope is not valid JSON: {exc}") from exc
    if not isinstance(env, dict):
        raise EnvelopeRefused("MALFORMED_ENVELOPE", "envelope must be a JSON object")
    return env


def _touches_protected(task) -> bool:
    """Conservative: any requested scope naming a protected target counts as privileged."""
    for sc in (task.get("allowed_scope") or []):
        low = str(sc).lower()
        if any(p in low for p in ("deploy/oracle", "oracle", "devices/", "handset")):
            return True
    return False


def admit(
    comment: dict,
    policy: dict,
    identity: dict,
    ledger: Ledger,
    approvals_store: ApprovalStore | None = None,
) -> dict:
    """Returns the admission decision. Raises EnvelopeRefused for anything that must not proceed."""
    author = ((comment.get("author") or {}).get("login") or "").strip()
    if not author:
        raise EnvelopeRefused("NO_AUTHOR", "comment has no author login")
    if author not in policy["allowed_authors"]:
        raise EnvelopeRefused("AUTHOR_NOT_ALLOWED",
                              f"'{author}' is not on the task-author allowlist; nothing in this "
                              f"comment is actionable")

    env = extract_envelope(comment.get("body", ""), policy["envelope_tag"])

    # Resolve the task FIRST: the request fields live inside it, not at the envelope root.
    # Reading them from the envelope root silently skipped the hard-prohibited block below.
    task = env["task"] if isinstance(env.get("task"), dict) else env
    if not isinstance(task, dict):
        raise EnvelopeRefused("MALFORMED_ENVELOPE", "envelope carries no task object")

    # Explicit alias migration: map legacy keys into canonical fields only when full required security fields exist
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
        if "source" not in task and "source" in env:
            task["source"] = env["source"]

    # `requires_approval` is a REQUEST and is the escalation path that must be hard-blocked.
    # `prohibited_actions` is the task's own safety DECLARATION - declaring a dangerous action
    # as forbidden is good hygiene, so it is recorded, never treated as a request.
    requested = set(task.get("requires_approval") or [])
    hard = requested & set(policy["hard_prohibited"])
    if hard:
        raise EnvelopeRefused("HARD_PROHIBITED",
                              f"{sorted(hard)} can never be authorized through a task envelope, "
                              f"regardless of author or approval record")

    # REPAIR 2: an offline comment dict is DATA. It cannot assert that GitHub verified anything, so
    # provenance must arrive out-of-band from the API layer (_verified_provenance), never be inferred
    # from comment.author.login. Privileged requests fail closed without it; benign requests still get a
    # context, but one whose auth_method does NOT claim verification.
    _prov = comment.get("_verified_provenance") or {}
    provenance_ok = bool(_prov.get("api_origin") is True and _prov.get("verified_author") == author)
    _privileged = bool(task.get("requires_approval")) or _touches_protected(task)
    if _privileged and not provenance_ok:
        raise EnvelopeRefused(
            "PROVENANCE_UNVERIFIED",
            "privileged task requires authenticated GitHub API-origin provenance bound to the actor; "
            "caller-supplied comment JSON (including comment.author.login) cannot authorize dispatch")
    dispatch_context = TrustedDispatchContext(
        actor_id=author,
        actor_roles=("collaborator",),
        auth_method=("github_issue_comment_verified_author" if provenance_ok
                     else "github_issue_comment_unverified_author"),
        auth_subject=(str(_prov.get("subject")) if provenance_ok else comment.get("url", "issue:comment")),
        dispatcher_id="hg_watcher",
        issued_at=time.time(),
        request_id=f"req-{os.urandom(8).hex()}",
    )

    task['author'] = author
    try:
        verdict = validate(
            task,
            identity,
            ledger,
            dispatch_context=dispatch_context,
            approvals_store=approvals_store,
        )
    except Refused as exc:
        raise EnvelopeRefused(exc.code, exc.detail) from exc

    verdict["declared_prohibited"] = sorted(set(task.get("prohibited_actions") or []))
    verdict["author"] = author
    verdict["source"] = env.get("source", comment.get("url", "issue:14"))
    return verdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--comment", help="comment JSON (data only; never executed)")
    ap.add_argument("--policy", default=DEFAULT_POLICY)
    ap.add_argument("--identity", default=DEFAULT_IDENTITY)
    ap.add_argument("--ledger", default=DEFAULT_LEDGER)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if not a.comment:
        ap.error("--comment is required")

    policy = json.load(open(a.policy))
    identity = json.load(open(a.identity))
    ledger = Ledger(a.ledger)
    comment = json.load(open(a.comment))
    try:
        verdict = admit(comment, policy, identity, ledger)
    except EnvelopeRefused as exc:
        out = {"decision": "REFUSE", "code": exc.code, "detail": exc.detail}
        print(json.dumps(out, indent=2) if a.json else f"REFUSE [{exc.code}] {exc.detail}")
        return 2
    print(json.dumps(verdict, indent=2) if a.json else
          f"ADMIT  {verdict['task_id']} (from {verdict['state']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
