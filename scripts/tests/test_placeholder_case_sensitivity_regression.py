#!/usr/bin/env python3
"""Regression fixtures - the placeholder filter must not fail open.

DEFECT (fail-open, blocker)
---------------------------
``scripts/check_submission_claims.py`` compiled ``PLACEHOLDER_RE`` with a *global*
``re.IGNORECASE``.  One of its alternatives, ``[A-Z_]{4,}`` (CONSTANT_CASE), was
meant to recognise ``REPLACE_ME``/``TODO_TOKEN``-shaped constants.  Under a global
IGNORECASE flag that character class matches ANY purely alphabetic value of four
characters or more, because every ``[A-Z_]`` position also accepts ``[a-z]``.  The
scanner then classified the matched value as an obvious placeholder and *skipped
it entirely*, so credential assignments like pass-word = "supersecretvalue"
produced NO finding at all - not FAIL, not REVIEW, nothing.

FIX
---
The global flag is dropped; case-insensitivity is scoped to the CONSTANT_CASE
alternative (``(?-i:[A-Z_]{4,})``) and to the literal placeholder words
(``(?i:redacted|example|...|todo)``).

EXPECTED EXIT CODES (also in the test names)
--------------------------------------------
* ``..._exit_1``  - the scanner must return EXIT_FAIL (1): a real assignment was
  seen and reported.
* ``..._exit_0``  - the scanner must return EXIT_OK (0): only placeholders were
  present, so nothing is reported.

Non-vacuity: every case plants a control line and asserts on the *specific*
``HG-CRED-006`` finding, so a run that silently stopped scanning cannot pass.
This file is self-contained (only the standard library and a sibling fixture
module for the clean baseline tree).

Run:
    python3 scripts/tests/test_placeholder_case_sensitivity_regression.py -v
"""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
SCRIPTS = TESTS.parent
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(TESTS))

import check_submission_claims as csc  # noqa: E402

#: The suite already owns a fixture tree that must be silent (LICENSE, NOTICE,
#: README, package.json and the judge-facing docs, every claim explicitly
#: negated).  Reusing it here means the only variable under test is the
#: placeholder decision.
import test_check_submission_claims as base  # noqa: E402


def run_cli(*argv: str) -> tuple[int, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = csc.main(list(argv))
    return code, out.getvalue()


class PlaceholderCaseSensitivityRegression(unittest.TestCase):
    """PLACEHOLDER_RE must suppress placeholders only - never real values."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="hg-pholder-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    # -- helpers -------------------------------------------------------------

    def tree(self, notes: str, *, extra: dict | None = None) -> Path:
        """The clean baseline tree plus a doc holding ``notes``."""
        root = self.tmp / "tree"
        if root.exists():
            shutil.rmtree(root)
        root.mkdir()
        files = dict(base.CLEAN_FILES)
        files.update(extra or {})
        files["docs/PLACEHOLDER_PROBE.md"] = "# Probe\n\n" + notes + "\n"
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return root

    def scan(self, root: Path) -> tuple[int, dict]:
        code, out = run_cli(str(root), "--json")
        return code, json.loads(out)

    @staticmethod
    def cred006(report: dict) -> list[dict]:
        return [f for f in report["findings"] if f["rule"] == "HG-CRED-006"]

    # -- (0) the baseline really is silent, exit 0 ---------------------------

    def test_clean_baseline_tree_is_exit_0(self) -> None:
        """Guard: if the baseline is not silent this fixture proves nothing."""
        code, report = self.scan(self.tree("- Nothing to see here.\n"))
        self.assertEqual(
            report["findings"], [],
            "baseline tree is not clean: {}".format(json.dumps(report["findings"])),
        )
        self.assertEqual(code, csc.EXIT_OK)

    # -- (1) the fail-open: an all-alphabetic value must FAIL, exit 1 --------

    def test_all_lowercase_alpha_assignment_is_fail_exit_1(self) -> None:
        """``assignment = "supersecretvalue"`` must produce a FAIL (exit 1)."""
        root = self.tree('pass' + 'word = "supersecretvalue"\n')
        code, report = self.scan(root)
        found = self.cred006(report)
        self.assertEqual(len(found), 1, json.dumps(report["findings"]))
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertEqual(found[0]["value_length"], len("supersecretvalue"))
        self.assertEqual(code, csc.EXIT_FAIL)

    def test_mixed_case_alpha_assignment_is_fail_exit_1(self) -> None:
        """``assignment = "SuperSecretProdValue"`` must produce a FAIL (exit 1)."""
        root = self.tree('pass' + 'word = "SuperSecretProdValue"\n')
        code, report = self.scan(root)
        found = self.cred006(report)
        self.assertEqual(len(found), 1, json.dumps(report["findings"]))
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertEqual(found[0]["value_length"], len("SuperSecretProdValue"))
        self.assertEqual(code, csc.EXIT_FAIL)

    def test_all_alpha_token_assignment_is_fail_exit_1(self) -> None:
        """A different assignment key, same shape: must FAIL (exit 1)."""
        root = self.tree('auth_' + 'token = "abcdefghijklmnop"\n')
        code, report = self.scan(root)
        found = self.cred006(report)
        self.assertEqual(len(found), 1, json.dumps(report["findings"]))
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertEqual(code, csc.EXIT_FAIL)

    # -- (2) the plugin must not revert to suppressing a real value ----------

    def test_real_value_flagged_while_a_placeholder_beside_it_is_not_exit_1(
            self) -> None:
        """Non-vacuity: both values on adjacent lines, one real, one not."""
        root = self.tree(
            'pass' + 'word = "REPLACE_ME"\n'
            'pass' + 'word = "correct-horse-battery-9"\n'
        )
        code, report = self.scan(root)
        found = self.cred006(report)
        self.assertEqual(
            [(f["value_length"], f["severity"]) for f in found],
            [(len("correct-horse-battery-9"), "FAIL")],
            "the placeholder line must be silent AND the real line must FAIL",
        )
        self.assertEqual(code, csc.EXIT_FAIL)

    # -- (3) genuine placeholders stay silent, exit 0 ------------------------

    def test_placeholder_values_stay_silent_exit_0(self) -> None:
        """Every documented placeholder shape must remain suppressed (exit 0)."""
        placeholders = (
            'password = "REPLACE_ME"',          # CONSTANT_CASE constant
            'password = "TODO"',                # literal word, upper case
            'password = "todo"',                # literal word, lower case
            'password = "Example"',             # literal word, mixed case
            "password = ${DB_PASSWORD}",        # shell-style reference
            'password = "xxxxxxxx"',            # x-run
            "password = <vpn-address>",         # angle-bracket placeholder
            'password = "..."',                 # ellipsis
            'password = "None"',                # nullish literal
        )
        root = self.tree("\n".join(placeholders) + "\n")
        code, report = self.scan(root)
        found = self.cred006(report)
        self.assertEqual(
            found, [],
            "a placeholder was reported: {}".format(json.dumps(found)),
        )
        self.assertEqual(code, csc.EXIT_OK)

    def test_placeholder_re_does_not_carry_a_global_ignorecase(self) -> None:
        """The regression the whole fixture exists for, asserted directly."""
        import re as _re

        self.assertFalse(
            csc.PLACEHOLDER_RE.flags & _re.IGNORECASE,
            "a global IGNORECASE makes [A-Z_]{4,} match any alphabetic value",
        )
        for real in ("supersecretvalue", "SuperSecretProdValue", "abcdefgh"):
            with self.subTest(value=real):
                self.assertIsNone(csc.PLACEHOLDER_RE.match(real))
        for placeholder in ("REPLACE_ME", "TODO", "todo", "Example", "xxxxxxxx",
                            "<vpn-address>", "..."):
            with self.subTest(value=placeholder):
                self.assertIsNotNone(csc.PLACEHOLDER_RE.match(placeholder))


if __name__ == "__main__":
    unittest.main(verbosity=2)
