#!/usr/bin/env python3
"""Regression test suite for scripts/verify_judge_release.sh.

Verifies failure propagation, non-zero exit codes on failure, strict gating
against logging-only masking, and proper output formatting for automated judges.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = SCRIPTS_DIR.parent
VERIFY_SCRIPT = SCRIPTS_DIR / "verify_judge_release.sh"

ANSI_ESCAPE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")


def strip_ansi(text: str) -> str:
    return ANSI_ESCAPE.sub("", text)


class TestVerifyJudgeReleaseScript(unittest.TestCase):
    """Test suite for verify_judge_release.sh harness."""

    def test_verify_judge_release_passes_on_clean_candidate(self):
        """Clean candidate must pass all 27 checks and 9 gates with exit code 0."""
        result = subprocess.run(
            ["bash", str(VERIFY_SCRIPT)],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60,
        )
        clean_stdout = strip_ansi(result.stdout)
        clean_stderr = strip_ansi(result.stderr)

        self.assertEqual(
            result.returncode,
            0,
            f"Expected exit 0, got {result.returncode}.\nSTDOUT:\n{clean_stdout}\nSTDERR:\n{clean_stderr}",
        )
        self.assertIn("VERDICT:                PASS", clean_stdout)
        self.assertIn("TOTAL_CHECKS_PASSED:    27", clean_stdout)
        self.assertIn("TOTAL_CHECKS_FAILED:    0", clean_stdout)
        self.assertIn("BUILD_PREREQUISITES:    PASS", clean_stdout)
        self.assertIn("NATIVE_MCP_PROTOCOL:    PASS", clean_stdout)
        self.assertIn("MCP_HANDSHAKE:          PASS", clean_stdout)
        self.assertIn("READ_ONLY_TOOLS:        PASS", clean_stdout)
        self.assertIn("MUTATING_TOOLS_EXCLUDED:PASS", clean_stdout)
        self.assertIn("SECURITY_BOUNDS:        PASS", clean_stdout)
        self.assertIn("OUTBOUND_COORDINATOR:   PASS", clean_stdout)
        self.assertIn("CAMERA_SAFETY_BOUNDARY: PASS", clean_stdout)
        self.assertIn("CLEAN_PROCESS_SHUTDOWN: PASS", clean_stdout)

    def test_logging_only_failure_cannot_be_hidden(self):
        """Calling log_fail without explicit exit must still produce exit code 1 and VERDICT: FAIL."""
        script_content = VERIFY_SCRIPT.read_text(encoding="utf-8")
        # Inject a log_fail call right before print_summary without an explicit exit
        injection_point = "print_summary\nFINAL_RC=$?"
        self.assertIn(injection_point, script_content)
        modified_content = script_content.replace(
            injection_point,
            'log_fail "Simulated failure without explicit exit"\n' + injection_point,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            test_script = Path(tmpdir) / "verify_injected_fail.sh"
            test_script.write_text(modified_content, encoding="utf-8")
            test_script.chmod(0o755)

            env = {**os.environ, "HG_REPO_ROOT": str(REPO_ROOT)}
            result = subprocess.run(
                ["bash", str(test_script)],
                cwd=str(REPO_ROOT),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=60,
            )
            clean_stdout = strip_ansi(result.stdout)
            clean_stderr = strip_ansi(result.stderr)

            self.assertEqual(
                result.returncode,
                1,
                f"Expected exit 1 when log_fail is called, got {result.returncode}.\nSTDOUT:\n{clean_stdout}\nSTDERR:\n{clean_stderr}",
            )
            self.assertIn("VERDICT:                FAIL", clean_stdout)
            self.assertIn("Simulated failure without explicit exit", clean_stderr)
            self.assertIn("TOTAL_CHECKS_FAILED:    1", clean_stdout)

    def test_gate_failure_propagates_nonzero_exit_and_fail_verdict(self):
        """Simulating a gate failure must fail execution and report VERDICT: FAIL."""
        script_content = VERIFY_SCRIPT.read_text(encoding="utf-8")
        # Invert camera safety check so it triggers failure
        target = 'if [[ "$CONFIG_CAPTURE_ENABLED" == "false" ]]; then'
        self.assertIn(target, script_content)
        modified_content = script_content.replace(
            target,
            'if [[ "$CONFIG_CAPTURE_ENABLED" == "INVERTED_MOCK_FAILURE" ]]; then',
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            test_script = Path(tmpdir) / "verify_gate_fail.sh"
            test_script.write_text(modified_content, encoding="utf-8")
            test_script.chmod(0o755)

            env = {**os.environ, "HG_REPO_ROOT": str(REPO_ROOT)}
            result = subprocess.run(
                ["bash", str(test_script)],
                cwd=str(REPO_ROOT),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=60,
            )
            clean_stdout = strip_ansi(result.stdout)
            clean_stderr = strip_ansi(result.stderr)

            self.assertEqual(
                result.returncode,
                1,
                f"Expected exit 1 on gate failure, got {result.returncode}.\nSTDOUT:\n{clean_stdout}\nSTDERR:\n{clean_stderr}",
            )
            self.assertIn("VERDICT:                FAIL", clean_stdout)
            self.assertIn("CAMERA_SAFETY_BOUNDARY: FAIL", clean_stdout)
            self.assertIn("[VERIFICATION FAILED]", clean_stdout + clean_stderr)

    def test_missing_prerequisite_fails_early_with_summary(self):
        """Missing prerequisite command must fail preflight, report summary, and produce exit code 1."""
        script_content = VERIFY_SCRIPT.read_text(encoding="utf-8")
        target = 'check_cmd "curl"'
        self.assertIn(target, script_content)
        modified_content = script_content.replace(
            target,
            'check_cmd "nonexistent_fake_tool_xyz_123" "Dummy prerequisite test"',
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            test_script = Path(tmpdir) / "verify_missing_prereq.sh"
            test_script.write_text(modified_content, encoding="utf-8")
            test_script.chmod(0o755)

            env = {**os.environ, "HG_REPO_ROOT": str(REPO_ROOT)}
            result = subprocess.run(
                ["bash", str(test_script)],
                cwd=str(REPO_ROOT),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=15,
            )
            clean_stdout = strip_ansi(result.stdout)
            clean_stderr = strip_ansi(result.stderr)

            self.assertEqual(
                result.returncode,
                1,
                f"Expected exit 1 on missing prereq, got {result.returncode}.\nSTDOUT:\n{clean_stdout}\nSTDERR:\n{clean_stderr}",
            )
            self.assertIn("BUILD_PREREQUISITES:    FAIL", clean_stdout)
            self.assertIn("VERDICT:                FAIL", clean_stdout)
            self.assertIn("[VERIFICATION FAILED]", clean_stdout + clean_stderr)


if __name__ == "__main__":
    unittest.main()
