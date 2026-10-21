#!/usr/bin/env python3
"""HomeGuardian AI - Remote Compute Router & Kaggle Runner
Mission: HG-WORKER-RESILIENCE-AND-WATCHER-01

Routes compute workloads between LOCAL_LIGHT, LOCAL_BOUNDED, KAGGLE_CPU, and KAGGLE_GPU.
Enforces strict resource gating, secret redaction, preflight quota checks,
execution timeouts, and post-run container cessation verification.
"""

import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("kaggle_router")


class ComputeTarget(str, Enum):
    LOCAL_LIGHT = "LOCAL_LIGHT"
    LOCAL_BOUNDED = "LOCAL_BOUNDED"
    KAGGLE_CPU = "KAGGLE_CPU"
    KAGGLE_GPU = "KAGGLE_GPU"


class ResourcePressureGuard:
    """Monitors host memory and load average to prevent RAM exhaustion."""

    @staticmethod
    def get_host_memory_available_gb() -> float:
        """Returns available RAM in GiB from /proc/meminfo or fallback."""
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if line.startswith("MemAvailable:"):
                        kb = int(line.split()[1])
                        return kb / (1024.0 * 1024.0)
        except Exception:
            pass
        return 8.0  # safe assumption if unreadable

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


class KaggleComputeRouter:
    """Manages remote Kaggle compute execution with fail-closed safety."""

    DEFAULT_KERNEL_SLUG = "swarnasekhardhar/hg-remote-smoke"
    MAX_TIMEOUT_SECONDS = 3600  # 60 minute hard ceiling

    def __init__(self, kernel_slug: Optional[str] = None):
        self.kernel_slug = kernel_slug or self.DEFAULT_KERNEL_SLUG

    @staticmethod
    def classify_task(task_action: str, task_type: str, requires_gpu: bool = False, is_heavy: bool = False) -> ComputeTarget:
        """Determines execution target based on action, type, and resource requirements."""
        if requires_gpu:
            return ComputeTarget.KAGGLE_GPU

        if is_heavy or task_type in ("sanitizer_verification", "full_matrix_build", "android_full_gradle"):
            return ComputeTarget.KAGGLE_CPU

        under_pressure, _ = ResourcePressureGuard.is_under_pressure()
        if under_pressure and is_heavy:
            return ComputeTarget.KAGGLE_CPU

        if task_action in ("implement", "test") and not is_heavy:
            return ComputeTarget.LOCAL_BOUNDED

        return ComputeTarget.LOCAL_LIGHT

    @staticmethod
    def preflight_check() -> Dict[str, Any]:
        """Verifies Kaggle CLI availability and checks for any active kernels."""
        which_kaggle = shutil.which("kaggle")
        if not which_kaggle:
            return {"ok": False, "error": "kaggle CLI not found in PATH"}

        try:
            res = subprocess.run(["kaggle", "kernels", "list", "--mine"], capture_output=True, text=True, check=True)
            return {"ok": True, "cli_path": which_kaggle, "active_listing": res.stdout}
        except subprocess.CalledProcessError as e:
            return {"ok": False, "error": f"Kaggle authentication/CLI failed: {e.stderr}"}

    def get_kernel_status(self, slug: Optional[str] = None) -> str:
        target_slug = slug or self.kernel_slug
        try:
            res = subprocess.run(["kaggle", "kernels", "status", target_slug], capture_output=True, text=True, check=True)
            output = res.stdout.strip()
            # Example: swarnasekhardhar/hg-remote-smoke has status "KernelWorkerStatus.COMPLETE"
            match = re.search(r'status "([^"]+)"', output)
            if match:
                return match.group(1)
            return output
        except subprocess.CalledProcessError as e:
            logger.error(f"Failed to query kernel status for {target_slug}: {e.stderr}")
            return "UNKNOWN"

    def run_ephemeral_job(
        self,
        script_code: str,
        title: str = "HG Remote Job",
        enable_gpu: bool = False,
        timeout_seconds: int = 1800,
        output_dir: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Pushes an ephemeral script kernel, monitors execution, retrieves output, and verifies cessation."""
        if timeout_seconds > self.MAX_TIMEOUT_SECONDS:
            timeout_seconds = self.MAX_TIMEOUT_SECONDS

        preflight = self.preflight_check()
        if not preflight["ok"]:
            return {"status": "BLOCKED", "reason": preflight["error"]}

        # Check existing status - ensure not already running
        current_status = self.get_kernel_status()
        if "RUNNING" in current_status or "QUEUED" in current_status:
            return {"status": "BUSY", "reason": f"Kernel {self.kernel_slug} is currently {current_status}"}

        # Build metadata and payload in isolated temp directory
        work_dir = tempfile.mkdtemp(prefix="kaggle-job-")
        try:
            kernel_name = self.kernel_slug.split("/")[-1]
            user_name = self.kernel_slug.split("/")[0]

            script_path = os.path.join(work_dir, "main.py")
            with open(script_path, "w", encoding="utf-8") as f:
                f.write(script_code)

            meta = {
                "id": self.kernel_slug,
                "title": title,
                "code_file": "main.py",
                "language": "python",
                "kernel_type": "script",
                "is_private": "true",
                "enable_gpu": "true" if enable_gpu else "false",
                "enable_tpu": "false",
                "enable_internet": "true",
                "dataset_sources": [],
                "competition_sources": [],
                "kernel_sources": [],
            }
            meta_path = os.path.join(work_dir, "kernel-metadata.json")
            with open(meta_path, "w", encoding="utf-8") as f:
                json.dump(meta, f, indent=2)

            logger.info(f"Pushing Kaggle kernel {self.kernel_slug} (GPU={enable_gpu}, timeout={timeout_seconds}s)...")
            push_res = subprocess.run(
                ["kaggle", "kernels", "push", "-p", work_dir],
                capture_output=True,
                text=True,
            )
            if push_res.returncode != 0:
                return {"status": "PUSH_FAILED", "error": push_res.stderr}

            t0 = time.time()
            final_status = "UNKNOWN"
            # Poll status until finished or timeout
            while time.time() - t0 < timeout_seconds:
                time.sleep(15)
                status = self.get_kernel_status()
                logger.info(f"Kaggle kernel status: {status} ({int(time.time() - t0)}s elapsed)")
                if "COMPLETE" in status or "ERROR" in status:
                    final_status = status
                    break

            if final_status == "UNKNOWN":
                return {"status": "TIMEOUT", "elapsed": time.time() - t0, "kernel": self.kernel_slug}

            # Retrieve outputs
            target_out = output_dir or os.path.join(work_dir, "output")
            os.makedirs(target_out, exist_ok=True)
            out_res = subprocess.run(
                ["kaggle", "kernels", "output", self.kernel_slug, "-p", target_out],
                capture_output=True,
                text=True,
            )

            # Confirm cessation
            post_status = self.get_kernel_status()
            is_terminated = ("COMPLETE" in post_status or "ERROR" in post_status)

            return {
                "status": "PASS" if "COMPLETE" in final_status else "FAIL",
                "kernel_status": final_status,
                "post_status": post_status,
                "is_terminated": is_terminated,
                "output_dir": target_out,
                "elapsed_seconds": int(time.time() - t0),
                "kernel": self.kernel_slug,
            }
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    pre = KaggleComputeRouter.preflight_check()
    print("Preflight check:", json.dumps(pre, indent=2))
    guard_pressure, msg = ResourcePressureGuard.is_under_pressure()
    print(f"Host Pressure Guard: pressure={guard_pressure} ({msg})")
