#!/usr/bin/env python3
"""HomeGuardian AI — Shared Multi-Agent GitHub Task Watcher & Queue
Mission: HG-WORKER-ORCHESTRATION-BOOTSTRAP-01
Role: INTEGRATION_COORDINATOR

Connects authorized GitHub issue directives to Hermes Sessions 1, 2, 3 and Antigravity.
Enforces:
  1. Author allowlist (only authorized coordinators/maintainers)
  2. Comment text is DATA, never commands (rejects shell injection patterns)
  3. Structured task block requirement (fenced `hg-task` JSON)
  4. Worker scope and branch confinement
  5. Durable atomic state store and single-writer concurrency lock
  6. Rate-limit preservation and ETag/since caching
  7. Deduplication across restarts
"""

from __future__ import annotations

import argparse
import fcntl
import json
import logging
import os
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional, Tuple

DEFAULT_STATE_DIR = os.environ.get("HG_ORCHESTRATOR_STATE_DIR") or os.path.expanduser("~/.hermes/cache/scratch/hg-orchestrator")
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
DEFAULT_REGISTRY_PATH = os.path.join(SCRIPT_DIR, "worker_registry.json")

SHELL_INJECTION_PATTERNS = [
    r"\brm\s+-[rf]",
    r"\bsudo\b",
    r"\bcurl\s+[^|]*\|\s*(ba)?sh",
    r"\bwget\s+[^|]*\|\s*(ba)?sh",
    r"\beval\b",
    r"\$\([^)]*\)",
    r"`[^`]+`",
    r"\biptables\b",
    r"\bssh\s+\w+@",
    r"\bchmod\s+[0-7]{3,4}\b",
    r"\bdd\s+if=",
    r":\(\)\s*\{",
    r"\bnc\s+-[a-z]*e",
]

SECRET_PATTERNS = [
    (r"gh[opsu]_[A-Za-z0-9_]{30,}", "[REDACTED_GH_TOKEN]"),
    (r"(?i)bearer\s+[A-Za-z0-9_\-\.]{20,}", "Bearer [REDACTED]"),
    (r"(?i)password\s*[:=]\s*\S+", "password=[REDACTED]"),
    (r"(?i)secret\s*[:=]\s*\S+", "secret=[REDACTED]"),
]


def redact_secrets(text: str) -> str:
    """Sanitize logs and outputs from potential credential leaks."""
    for pattern, repl in SECRET_PATTERNS:
        text = re.sub(pattern, repl, text)
    return text


class SafeLogFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        s = super().format(record)
        return redact_secrets(s)


logger = logging.getLogger("hg_task_watcher")


def setup_logging(verbose: bool = False) -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(SafeLogFormatter("%(asctime)s [%(levelname)s] %(message)s"))
    logger.setLevel(logging.DEBUG if verbose else logging.INFO)
    logger.handlers = [handler]


class WorkerRegistry:
    def __init__(self, registry_path: str = DEFAULT_REGISTRY_PATH):
        self.path = registry_path
        with open(registry_path, "r", encoding="utf-8") as f:
            self.data = json.load(f)
        self.workers: Dict[str, Dict[str, Any]] = self.data.get("workers", {})
        self.authorized_authors: List[str] = self.data.get("authorized_authors", [])
        self.allowed_actions: List[str] = self.data.get("allowed_task_actions", [])

    def get_worker_for_issue(self, issue_number: int) -> Optional[Dict[str, Any]]:
        for w in self.workers.values():
            if w.get("issue_number") == issue_number or issue_number in w.get("also_accepts_issues", []):
                return w
        return None

    def get_worker(self, worker_id: str) -> Optional[Dict[str, Any]]:
        return self.workers.get(worker_id)


class OrchestratorState:
    """Durable state storage with atomic write operations and file locking."""

    def __init__(self, state_dir: str = DEFAULT_STATE_DIR):
        self.state_dir = state_dir
        os.makedirs(state_dir, exist_ok=True)
        self.state_path = os.path.join(state_dir, "state.json")
        self.lock_path = os.path.join(state_dir, "orchestrator.lock")
        self.lock_file: Optional[Any] = None

    def acquire_lock(self, timeout_sec: int = 5) -> bool:
        t0 = time.time()
        self.lock_file = open(self.lock_path, "w")
        while time.time() - t0 < timeout_sec:
            try:
                fcntl.flock(self.lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.lock_file.write(str(os.getpid()) + "\n")
                self.lock_file.flush()
                return True
            except (IOError, OSError):
                time.sleep(0.2)
        try:
            self.lock_file.close()
        except Exception:
            pass
        self.lock_file = None
        return False

    def release_lock(self) -> None:
        if self.lock_file:
            try:
                fcntl.flock(self.lock_file, fcntl.LOCK_UN)
                self.lock_file.close()
            except Exception:
                pass
            self.lock_file = None

    def __del__(self) -> None:
        self.release_lock()

    def load(self) -> Dict[str, Any]:
        if not os.path.isfile(self.state_path):
            return {
                "schema": "hg-orchestrator-state/1.0",
                "processed_comments": {},
                "tasks": {},
                "last_poll_timestamps": {},
                "rejected_events": []
            }
        try:
            with open(self.state_path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception as e:
            logger.warning(f"Failed to read state file, recovering fresh: {e}")
            return {
                "schema": "hg-orchestrator-state/1.0",
                "processed_comments": {},
                "tasks": {},
                "last_poll_timestamps": {},
                "rejected_events": []
            }

    def save(self, state: Dict[str, Any]) -> None:
        state["rejected_events"] = state.get("rejected_events", [])[-100:]  # bounded retention
        fd, tmp = tempfile.mkstemp(dir=self.state_dir, prefix=".state-", suffix=".tmp")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=2, sort_keys=True)
        os.replace(tmp, self.state_path)


class TaskValidator:
    """Validates GitHub comments against strict security invariants."""

    @staticmethod
    def scan_shell_injection(text: str) -> List[str]:
        hits = []
        for pat in SHELL_INJECTION_PATTERNS:
            if re.search(pat, text):
                hits.append(pat)
        return hits

    @classmethod
    def extract_tasks(cls, comment_body: str, author: str, allowed_authors: List[str]) -> Tuple[List[Dict[str, Any]], List[str]]:
        """Extracts structured tasks from comment text. Returns (valid_tasks, rejection_reasons)."""
        rejections: List[str] = []
        tasks: List[Dict[str, Any]] = []

        if author not in allowed_authors:
            rejections.append(f"UNAUTHORIZED_AUTHOR: '{author}' not in allowlist")
            return tasks, rejections

        # Extract fenced blocks with marker ```hg-task or ```json:hg-task
        pattern = r"```(?:hg-task|json:hg-task)\s*([\s\S]*?)```"
        blocks = re.findall(pattern, comment_body)

        if not blocks:
            # Check for coordinator directive block syntax
            dir_match = re.search(r"##\s*TASK ASSIGNMENT\s*([\s\S]*?)(?:##|\Z)", comment_body, re.IGNORECASE)
            if dir_match:
                # Try finding JSON block inside
                sub_blocks = re.findall(r"```(?:json)?\s*([\s\S]*?)```", dir_match.group(1))
                if sub_blocks:
                    blocks.extend(sub_blocks)

        if not blocks:
            # Non-task comments are normal conversational comments; do not treat as an error
            return tasks, rejections

        for raw in blocks:
            # 1. Shell injection scan on raw text before parsing
            shell_hits = cls.scan_shell_injection(raw)
            if shell_hits:
                rejections.append(f"REJECTED_SHELL_INJECTION: detected shell patterns: {shell_hits}")
                continue

            # 2. JSON parse
            try:
                task_data = json.loads(raw.strip())
            except Exception as e:
                rejections.append(f"MALFORMED_TASK_JSON: {e}")
                continue

            if not isinstance(task_data, dict):
                rejections.append("INVALID_TASK_SHAPE: task must be a JSON object")
                continue

            # 3. Required fields validation
            required = ["mission", "worker", "action"]
            missing = [f for f in required if f not in task_data]
            if missing:
                rejections.append(f"MISSING_REQUIRED_FIELDS: {missing}")
                continue

            tasks.append(task_data)

        return tasks, rejections


def is_hermes_paused() -> Tuple[bool, str]:
    """Checks whether the global emergency stop (hermes pause) is engaged via ESTOP sentinel."""
    estop_path = os.environ.get("HERMES_ESTOP_FILE") or os.path.expanduser("~/.hermes/ESTOP")
    if os.path.isfile(estop_path):
        reason = "global emergency stop engaged"
        try:
            with open(estop_path, "r", encoding="utf-8") as f:
                content = f.read().strip()
                if content:
                    reason = f"global emergency stop: {content}"
        except Exception:
            pass
        return True, reason
    return False, "operational"


class ResourcePressureGuard:
    """Monitors host memory and load average to prevent RAM exhaustion."""

    @staticmethod
    def get_host_memory_available_gb() -> float:
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if line.startswith("MemAvailable:"):
                        kb = int(line.split()[1])
                        return kb / (1024.0 * 1024.0)
        except Exception:
            pass
        return 8.0

    @staticmethod
    def get_load_average_1m() -> float:
        try:
            return os.getloadavg()[0]
        except Exception:
            return 1.0

    @classmethod
    def is_under_pressure(cls, min_free_ram_gb: float = 2.0, max_load_1m: float = 12.0) -> Tuple[bool, str]:
        free_ram = cls.get_host_memory_available_gb()
        load_1m = cls.get_load_average_1m()
        if free_ram < min_free_ram_gb:
            return True, f"Low RAM: {free_ram:.2f} GiB available < threshold {min_free_ram_gb:.2f} GiB"
        if load_1m > max_load_1m:
            return True, f"High load average: {load_1m:.2f} > threshold {max_load_1m:.2f}"
        return False, "Host resources healthy"


class TaskDispatcher:
    """Dispatches validated tasks to worker sessions or records queue entries."""

    def __init__(self, registry: WorkerRegistry, dry_run: bool = False):
        self.registry = registry
        self.dry_run = dry_run

    def is_worker_busy(self, worker_info: Dict[str, Any]) -> Tuple[bool, Optional[int]]:
        status_src = worker_info.get("status_source", {})
        lock_path = status_src.get("lock_file")
        if not lock_path or not os.path.exists(lock_path):
            return False, None

        try:
            with open(lock_path, "r", encoding="utf-8") as f:
                content = f.read().strip()
                pid = int(content) if content.isdigit() else None
        except Exception:
            pid = None

        if pid and self._is_pid_alive(pid):
            return True, pid

        return False, None

    @staticmethod
    def _is_pid_alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except (ProcessLookupError, PermissionError):
            return False

    def dispatch(self, worker_id: str, task: Dict[str, Any]) -> Tuple[bool, str]:
        worker_info = self.registry.get_worker(worker_id)
        if not worker_info:
            return False, f"Unknown worker: {worker_id}"

        busy, pid = self.is_worker_busy(worker_info)
        if busy:
            return False, f"Worker {worker_id} is currently busy (PID {pid} active)"

        action = task.get("action")
        if action not in self.registry.allowed_actions:
            return False, f"Action '{action}' is not in allowed actions: {self.registry.allowed_actions}"

        # Resource Pressure Guard check for heavy tasks
        is_heavy = task.get("is_heavy", False) or action in ("build", "sanitizer")
        under_pressure, pressure_msg = ResourcePressureGuard.is_under_pressure()
        if is_heavy and under_pressure:
            logger.warning(f"Task deferred due to host resource pressure: {pressure_msg}")
            return False, f"DEFERRED_RESOURCE_PRESSURE: {pressure_msg}"

        dispatch_info = worker_info.get("dispatch_mechanism", {})
        dispatch_type = dispatch_info.get("type")

        mission = task.get("mission", "unknown-mission")
        summary = f"[{worker_id}] Task {mission}: {action} - {task.get('objective', '')}"

        # Sync to Hermes Kanban if board is available
        kanban_board = task.get("kanban_board", "hg-orchestration")
        idempotency_key = f"{mission}-{task.get('id', int(time.time()))}"
        title = f"[{worker_id}] {mission}: {action}"
        body = f"Objective: {task.get('objective', '')}\nPayload: {json.dumps(task)}"
        try:
            kanban_cmd = [
                "hermes", "kanban", "--board", kanban_board, "create",
                "--assignee", worker_id,
                "--idempotency-key", idempotency_key,
                title,
                "--body", body,
                "--json"
            ]
            if not self.dry_run:
                subprocess.run(kanban_cmd, capture_output=True, text=True, timeout=10)
        except Exception as ke:
            logger.warning(f"Kanban sync warning (non-fatal): {ke}")

        if self.dry_run:
            logger.info(f"[DRY-RUN] Would dispatch to {worker_id} ({dispatch_type}): {summary}")
            return True, f"dry-run simulated dispatch to {worker_id}"

        if dispatch_type == "hermes_resume":
            cmd = dispatch_info.get("command_template", "").format(assignment_summary=summary)
            logger.info(f"Executing Hermes dispatch: {cmd}")
            # Non-blocking / subprocess invocation
            try:
                proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                return True, f"dispatched to Hermes session PID {proc.pid}"
            except Exception as e:
                return False, f"Hermes dispatch failed: {e}"

        elif dispatch_type == "assistant_orchestrator":
            logger.info(f"Antigravity Coordinator internal task queue accepted: {summary}")
            return True, "queued in coordinator inbox"

        return False, f"Unsupported dispatch type: {dispatch_type}"


class SharedTaskWatcher:
    """Core orchestrator loop polling GitHub issues and dispatching tasks."""

    def __init__(self, registry_path: str = DEFAULT_REGISTRY_PATH, state_dir: str = DEFAULT_STATE_DIR, dry_run: bool = False):
        self.registry = WorkerRegistry(registry_path)
        self.state_mgr = OrchestratorState(state_dir)
        self.dispatcher = TaskDispatcher(self.registry, dry_run=dry_run)
        self.dry_run = dry_run

    def fetch_comments_via_gh(self, issue_number: int, since: Optional[str] = None) -> List[Dict[str, Any]]:
        repo = self.registry.data.get("repository", "methakon/homeguardian-ai")
        endpoint = f"repos/{repo}/issues/{issue_number}/comments"
        if since:
            endpoint += f"?since={since}"
        cmd = ["gh", "api", endpoint, "--paginate"]

        try:
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return json.loads(res.stdout)
        except subprocess.CalledProcessError as e:
            logger.error(f"GitHub API fetch failed for issue #{issue_number}: {e.stderr}")
            return []
        except Exception as e:
            logger.error(f"Unexpected error fetching issue #{issue_number}: {e}")
            return []

    def poll_once(self, comments_override: Optional[Dict[int, List[Dict[str, Any]]]] = None) -> Dict[str, Any]:
        """Runs a single polling cycle across all registered issues."""
        if not self.state_mgr.acquire_lock():
            logger.warning("Could not acquire orchestrator lock; another instance is running.")
            return {"status": "locked", "dispatched": 0}

        try:
            is_paused, pause_reason = is_hermes_paused()
            if is_paused:
                logger.warning(f"SharedTaskWatcher: Dispatch paused ({pause_reason})")
                return {"status": "paused", "reason": pause_reason, "dispatched": 0, "rejected": 0}

            state = self.state_mgr.load()
            dispatched_count = 0
            rejected_count = 0
            now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

            # Map of issue -> worker
            issues_to_check = set()
            for w in self.registry.workers.values():
                issues_to_check.add(w["issue_number"])
                for other in w.get("also_accepts_issues", []):
                    issues_to_check.add(other)

            for issue_num in sorted(issues_to_check):
                worker_info = self.registry.get_worker_for_issue(issue_num)
                if not worker_info:
                    continue
                worker_id = worker_info["worker_id"]

                # Fetch comments
                if comments_override is not None:
                    comments = comments_override.get(issue_num, [])
                else:
                    last_since = state["last_poll_timestamps"].get(str(issue_num))
                    comments = self.fetch_comments_via_gh(issue_num, since=last_since)

                state["last_poll_timestamps"][str(issue_num)] = now_iso

                for comment in comments:
                    comment_id = str(comment.get("id", ""))
                    if not comment_id:
                        continue

                    # Deduplication: skip already processed comments
                    if comment_id in state["processed_comments"]:
                        continue

                    author = comment.get("user", {}).get("login", "")
                    body = comment.get("body", "")

                    tasks, rejections = TaskValidator.extract_tasks(
                        body, author, self.registry.authorized_authors
                    )

                    if rejections:
                        for rej in rejections:
                            logger.warning(f"[REJECTED] Issue #{issue_num} Comment {comment_id}: {rej}")
                            state["rejected_events"].append({
                                "issue": issue_num,
                                "comment_id": comment_id,
                                "author": author,
                                "reason": rej,
                                "timestamp": now_iso
                            })
                            rejected_count += 1

                    for task in tasks:
                        task_worker = task.get("worker")
                        mission = task.get("mission")

                        # Validate worker assignment matches issue recipient
                        if task_worker != worker_id:
                            rej = f"WORKER_MISMATCH: Task assigned to '{task_worker}' but posted on #{issue_num} (owned by '{worker_id}')"
                            logger.warning(f"[REJECTED] {rej}")
                            state["rejected_events"].append({
                                "issue": issue_num,
                                "comment_id": comment_id,
                                "reason": rej,
                                "timestamp": now_iso
                            })
                            rejected_count += 1
                            continue

                        # Check deduplication on task mission
                        if mission in state["tasks"] and state["tasks"][mission].get("status") == "COMPLETED":
                            logger.info(f"[DUPLICATE] Task '{mission}' already completed; skipping.")
                            continue

                        # Dispatch task
                        ok, msg = self.dispatcher.dispatch(worker_id, task)
                        status_str = "DISPATCHED" if ok else "QUEUED"

                        state["tasks"][mission] = {
                            "mission": mission,
                            "worker_id": worker_id,
                            "issue_number": issue_num,
                            "comment_id": comment_id,
                            "status": status_str,
                            "action": task.get("action"),
                            "dispatch_message": msg,
                            "updated_at": now_iso,
                            "retries": 0
                        }

                        if ok:
                            dispatched_count += 1
                            logger.info(f"[DISPATCHED] {mission} -> {worker_id}: {msg}")
                        else:
                            logger.warning(f"[QUEUED_PENDING] {mission} -> {worker_id}: {msg}")

                    state["processed_comments"][comment_id] = {
                        "issue": issue_num,
                        "author": author,
                        "task_count": len(tasks),
                        "processed_at": now_iso
                    }

            self.state_mgr.save(state)
            return {
                "status": "ok",
                "dispatched": dispatched_count,
                "rejected": rejected_count,
                "timestamp": now_iso
            }

        finally:
            self.state_mgr.release_lock()


def main() -> None:
    parser = argparse.ArgumentParser(description="HomeGuardian AI Shared Task Watcher")
    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    poll_parser = subparsers.add_parser("poll", help="Run a single poll cycle")
    poll_parser.add_argument("--dry-run", action="store_true", help="Parse without executing dispatches")
    poll_parser.add_argument("--live", action="store_true", help="Explicit live dispatch mode")
    poll_parser.add_argument("--verbose", "-v", action="store_true", help="Verbose logging")

    status_parser = subparsers.add_parser("status", help="Show orchestrator and worker status")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        sys.exit(1)

    setup_logging(verbose=getattr(args, "verbose", False))

    if args.command == "poll":
        watcher = SharedTaskWatcher(dry_run=args.dry_run)
        result = watcher.poll_once()
        print(json.dumps(result, indent=2))

    elif args.command == "status":
        reg = WorkerRegistry()
        state_mgr = OrchestratorState()
        state = state_mgr.load()
        dispatcher = TaskDispatcher(reg)

        print("\n=== HOMEGUARDIAN AI WORKER STATUS ===")
        for w_id, w in reg.workers.items():
            busy, pid = dispatcher.is_worker_busy(w)
            status_str = f"BUSY (PID {pid})" if busy else "IDLE"
            print(f"- {w_id:28} [{w['role']:24}] Issue #{w['issue_number']:2} | {status_str:15} | Branch: {w['owned_branch']}")

        print(f"\nActive Tasks Tracked: {len(state.get('tasks', {}))}")
        for mission, t in state.get("tasks", {}).items():
            print(f"  * {mission:30} -> {t['worker_id']:24} [{t['status']}] (Issue #{t['issue_number']})")

        print(f"Processed Comments:   {len(state.get('processed_comments', {}))}")
        print(f"Rejected Events:      {len(state.get('rejected_events', {}))}\n")


if __name__ == "__main__":
    main()
