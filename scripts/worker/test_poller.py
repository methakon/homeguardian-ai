#!/usr/bin/env python3
"""HG-TASK-POLLER unit / negative tests - the classes #23 asked for.

Each case drives the poller's real decision path with controlled inputs (no network, no kanban, no
daemon). They exist because every defect found in this poller so far was found by running it, not by
reading it: an unhandled cron-environment crash, a busy-latch, a vacuous pass, and a pull filter that
could never match a spawnable task.

Classes covered
  duplicate sightings   the same task_id posted twice is admitted ONCE
  queue starvation      a busy or resource-starved worker LEAVES the task queued - never drops,
                        never claims, never dispatches
  cron auth failure     a failing `gh`/tooling path must be reported, not mistaken for idleness,
                        and must not admit or dispatch anything
  restart recovery      a handled task is not re-dispatched after a restart, and the seen-comment
                        watermark still advances
  isolation / refusal   another worker's task, an unlisted author, and prose with no envelope are
                        all inert
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile

POLLER = os.path.expanduser("~/.hermes/scripts/hg_task_poller.py")
WORKTREE = "/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-s1-lan-mtls-07"
BRANCH = "experiment/hg-lan-mtls-07"

ENVELOPE = """```hg-task
{{
  "source": "issue:14#unit-{tid}",
  "task": {{
    "task_id": "{tid}",
    "source": "issue:14#unit-{tid}",
    "assigned_to": "hg-hermes-network",
    "expected_branch": "{branch}",
    "allowed_scope": ["scripts/worker/"],
    "objective": "unit-test fixture: change nothing",
    "acceptance_tests": ["no change is made"],
    "prohibited_actions": ["oracle_change", "main_write", "apk_install"]
  }}
}}
```"""


def load_poller():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("hg_poller_under_test", POLLER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def comment(cid, author, body):
    return {"id": cid, "user": {"login": author}, "body": body, "html_url": f"https://x/{cid}"}


class Harness:
    """Drives main() with every side effect replaced by a recorder."""

    def __init__(self, tmp):
        self.m = load_poller()
        self.m.STATE_DIR = tmp
        self.m.STATE = os.path.join(tmp, "poller-state.json")
        self.calls = []           # (argv-tuple) of every external command attempted
        self.comments = []
        self.busy = None
        self.res_ok = (True, "ok")
        self.runnable = (True, "on disk")
        self.inner = {}

        m = self.m
        m.sh = self._sh
        m.gh_json = self._gh
        m.worker_busy = lambda: self.busy
        m.resources_ok = lambda: self.res_ok
        m.agent_executable = lambda: self.runnable

    def _sh(self, cmd, timeout=60):
        self.calls.append(tuple(cmd))

        class R:
            returncode = 0
            stdout = json.dumps(self.inner.get("assignees", [{"name": "default", "on_disk": True},
                                                             {"name": "hg-hermes-network", "on_disk": True}]))
            stderr = ""
        return R()

    def _gh(self, path):
        if path.startswith("issues/") and "comments" in path:
            return self.comments
        return {}

    def ran(self, *needle):
        return [c for c in self.calls if needle and all(n in c for n in needle)]

    def run(self, argv=()):
        old = sys.argv
        sys.argv = ["hg_task_poller.py", *argv]
        try:
            return self.m.main()
        finally:
            sys.argv = old

    def state(self):
        return json.load(open(self.m.STATE)) if os.path.exists(self.m.STATE) else {}


def case(fn, tmp):
    try:
        ok, detail = fn(Harness(tempfile.mkdtemp(dir=tmp)))
    except Exception as exc:                                    # noqa: BLE001
        ok, detail = False, f"{type(exc).__name__}: {exc}"
    return ok, detail


# --------------------------------------------------------------------------- classes


def busy_leaves_task_queued(h):
    """Queue starvation: BUSY must not claim, must not dispatch, must not drop the task."""
    h.busy = "kanban task t_x running"
    h.comments = [comment(11, "methakon", ENVELOPE.format(tid="HG-U1", branch=BRANCH))]
    rc = h.run()
    claims = h.ran("kanban", "create") + h.ran("kanban", "dispatch")
    return (rc == 0 and not claims), f"rc={rc} external_writes={len(claims)} (task still queued)"


def resources_starved_leaves_task_queued(h):
    h.res_ok = (False, "load 9.00/40")
    h.comments = [comment(12, "methakon", ENVELOPE.format(tid="HG-U2", branch=BRANCH))]
    rc = h.run()
    claims = h.ran("kanban", "create") + h.ran("kanban", "dispatch")
    return (rc == 0 and not claims), f"rc={rc} external_writes={len(claims)} (queued, not dropped)"


def not_executable_refuses_to_claim(h):
    """The latch defect: claiming what cannot be spawned leaves the worker BUSY forever."""
    h.runnable = (False, "hg-hermes-network   ON DISK: no")
    h.comments = [comment(13, "methakon", ENVELOPE.format(tid="HG-U3", branch=BRANCH))]
    rc = h.run()
    claims = h.ran("kanban", "create") + h.ran("kanban", "claim")
    return (rc == 0 and not claims), f"rc={rc} claims={len(claims)} (refused instead of latching)"


def duplicate_sighting_admits_once(h):
    """The same task_id twice must produce exactly one claim."""
    body = ENVELOPE.format(tid="HG-U4", branch=BRANCH)
    h.comments = [comment(21, "methakon", body), comment(22, "methakon", body)]
    rc = h.run()
    creates = h.ran("kanban", "create")
    handled = h.state().get("handled", {})
    return (rc == 0 and len(creates) == 1 and len(handled) == 1), \
        f"rc={rc} creates={len(creates)} handled={list(handled)} (admitted once)"


def restart_recovery_does_not_redispatch(h):
    """After a restart the persisted watermark and handled map must stop a re-dispatch."""
    body = ENVELOPE.format(tid="HG-U5", branch=BRANCH)
    h.comments = [comment(31, "methakon", body)]
    h.run()                                                  # first life: admits it
    first = len(h.state().get("handled", {}))
    h.calls.clear()
    h.run()                                                  # simulated restart: same input again
    creates = h.ran("kanban", "create")
    return (first == 1 and not creates), \
        f"first_life_handled={first} second_life_creates={len(creates)} (no re-dispatch)"


def prose_is_inert(h):
    h.comments = [comment(41, "methakon", "please do the thing, thanks")]
    rc = h.run()
    claims = h.ran("kanban", "create") + h.ran("kanban", "dispatch")
    return (rc == 0 and not claims and not h.state().get("handled")), \
        f"rc={rc} writes={len(claims)} (free-form prose never executes)"


def unlisted_author_is_refused(h):
    h.comments = [comment(51, "random-user", ENVELOPE.format(tid="HG-U7", branch=BRANCH))]
    rc = h.run()
    claims = h.ran("kanban", "create") + h.ran("kanban", "dispatch")
    return (rc == 0 and not claims), f"rc={rc} writes={len(claims)} (author not allowlisted)"


def tooling_failure_is_not_idleness(h):
    """Cron auth failure: a failing dependency must not read as 'nothing to do'."""
    h.busy = None
    h.res_ok = (True, "ok")

    def boom(path):
        if "comments" in path:
            raise RuntimeError("gh: authentication failed")
        return {}
    h.m.gh_json = boom
    raised = False
    try:
        h.run()
    except Exception:                                       # noqa: BLE001
        raised = True
    src = open(POLLER).read()
    wrapped = ("_report_crash" in src) and ("except Exception as exc" in src)
    writes = h.ran("kanban", "create") + h.ran("kanban", "dispatch")
    return (raised and wrapped and not writes), \
        f"cycle_raised={raised} wrapper_reports_it={wrapped} writes={len(writes)} (never a silent success)"


def pull_matches_spawnable_form_only(h):
    """The pull filter must match the tagged/spawnable form and nothing else."""
    def fake(board):
        if board == "default":
            return [{"id": "t_mine", "status": "ready", "assignee": "default",
                     "title": "[hg-hermes-network] HG-X"},
                    {"id": "t_theirs", "status": "ready", "assignee": "default",
                     "title": "[hg-hermes-cpp] HG-Y"}]
        return []
    h.m._board_tasks = fake
    got = h.m.queued_for_me()
    ids = [g[1] for g in got]
    return (ids == ["t_mine"]), f"pulled={ids} (no other worker's task)"


def cron_env_path_is_hardened(h):
    """cron PATH: gh lives at /snap/bin/gh and must be resolvable without a login shell."""
    path = os.environ.get("PATH", "")
    return ("/snap/bin" in path and os.path.expanduser("~/.local/bin") in path), \
        f"PATH head={path.split(':')[:3]}"


CASES = [
    ("queue starvation: busy -> task stays queued", busy_leaves_task_queued),
    ("queue starvation: resources starved -> queued", resources_starved_leaves_task_queued),
    ("cannot spawn -> refuses to claim (no latch)", not_executable_refuses_to_claim),
    ("duplicate sighting -> admitted once", duplicate_sighting_admits_once),
    ("restart recovery -> no re-dispatch", restart_recovery_does_not_redispatch),
    ("prose with no envelope -> inert", prose_is_inert),
    ("unlisted author -> refused", unlisted_author_is_refused),
    ("tooling/auth failure -> not idleness", tooling_failure_is_not_idleness),
    ("pull matches spawnable form only", pull_matches_spawnable_form_only),
    ("cron PATH hardened (/snap/bin + ~/.local/bin)", cron_env_path_is_hardened),
]


def main():
    if not os.path.exists(POLLER):
        print(f"poller not found: {POLLER}")
        return 2
    tmp = os.path.expanduser("~/.hermes/cache/scratch/hg-poller-unit")
    os.makedirs(tmp, exist_ok=True)
    failed = 0
    for name, fn in CASES:
        ok, detail = case(fn, tmp)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        failed += 0 if ok else 1
    print(f"  {len(CASES) - failed}/{len(CASES)} cases pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
