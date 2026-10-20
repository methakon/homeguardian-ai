#!/usr/bin/env python3
"""Regression fixtures - an unreadable packaged file must fail closed.

DEFECT (fail-closed gap)
------------------------
``scripts/check_release_package.py`` scanned packaged file contents with::

    try:
        raw = path.read_bytes()
    except OSError:
        continue

so a packaged file that could not be read (``chmod 000``, a broken link, an I/O
error) was silently skipped.  Every finding that file carried simply vanished:
a package with two credential-bearing files reported 2 FAILs when readable and
ZERO findings - a clean verdict, exit 0 - once they became unreadable, with no
note and no counter to say anything had been skipped.

FIX
---
Record the label and the OSError class, count them in
``stats["packaging"]["unreadable_skipped"]``, and emit an explicit
``HG-PKG-LEAK-008`` FAIL finding per unreadable file.  A file that cannot be read
cannot be certified, so an unscanned file can never pass as clean.

EXPECTED EXIT CODES (also in the test names)
--------------------------------------------
* ``..._exit_1``  - a FAIL finding was reported -> EXIT_FAIL (1).  Every case
  here is exit 1, in BOTH policies: the point is that no unreadable file may
  ever produce EXIT_OK (0).

Non-vacuity: the readable baseline of the same tree is asserted to produce NO
``HG-PKG-LEAK-008`` and to still report the credential finding each file carries,
so the new finding provably tracks readability and not something else.

Run:
    python3 scripts/tests/test_unreadable_package_file_regression.py -v
"""

from __future__ import annotations

import json
import os
import stat
import sys
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS.parent))
sys.path.insert(0, str(TESTS))

import check_release_package as crp  # noqa: E402
import test_check_release_package as base  # noqa: E402

#: Two packaged files that each carry a credential-shaped value (the scanner's
#: secret-assignment pattern needs 12 or more characters).
SECRET_A = 'const ' + 'sec' + 'ret = "hg-test-client-secret-77";\n'
SECRET_B = 'const ' + 'api_' + 'key = "hg-test-api-key-0001";\n'
PATHS = {"tests/fixtures/a.js": SECRET_A, "web/tests/b.js": SECRET_B}


class UnreadablePackageFileRegression(base.FixtureCase):
    """A packaged file the audit cannot read must be reported, never skipped."""

    def package_tree(self):
        """A directory-input package (no archive) that is clean but for the two."""
        return self.release(extra_files=dict(PATHS))

    def make_unreadable(self, tree: Path, rel: str) -> Path:
        path = tree / rel
        self.addCleanup(os.chmod, path, 0o644)
        os.chmod(path, 0)
        return path

    def audit_cli(self, target, *extra):
        code, out, _err = base.run_cli(str(target), "--json", *extra)
        return code, json.loads(out)

    @staticmethod
    def findings_for_rule(report, rule):
        return [f for f in report["findings"] if f["rule"] == rule]

    @staticmethod
    def packaging_stats(report):
        return report["stats"].get("packaging", {})

    # -- (0) baseline: readable, the credentials are the only findings --------

    def test_readable_tree_reports_each_credential_and_no_unreadable_exit_1(
            self) -> None:
        """Non-vacuity guard: with readable files there is nothing to report."""
        rel = self.package_tree()
        code, report = self.audit_cli(rel.tree)
        self.assertEqual(self.findings_for_rule(report, "HG-PKG-LEAK-008"), [])
        creds = self.findings_for_rule(report, "HG-PKG-LEAK-003")
        self.assertEqual(len(creds), 2, json.dumps(
            [(f["rule"], f["file"]) for f in report["findings"]]))
        self.assertTrue(all(f["severity"] == "FAIL" for f in creds))
        self.assertEqual(self.packaging_stats(report)["unreadable_skipped"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (1) one unreadable file: a named finding, the rest still scanned -----

    def test_one_unreadable_file_is_a_named_finding_exit_1(self) -> None:
        rel = self.package_tree()
        self.make_unreadable(rel.tree, "tests/fixtures/a.js")
        code, report = self.audit_cli(rel.tree)

        unreadable = self.findings_for_rule(report, "HG-PKG-LEAK-008")
        self.assertEqual(len(unreadable), 1, json.dumps(
            [(f["rule"], f["file"]) for f in report["findings"]]))
        self.assertEqual(unreadable[0]["severity"], "FAIL")
        self.assertEqual(unreadable[0]["file"], "tests/fixtures/a.js")
        self.assertIn("PermissionError", unreadable[0]["message"])
        self.assertIn("could not be read", unreadable[0]["message"])

        # The readable file is still scanned and still reported.
        creds = self.findings_for_rule(report, "HG-PKG-LEAK-003")
        self.assertEqual([f["file"] for f in creds], ["web/tests/b.js"])
        self.assertEqual(creds[0]["severity"], "FAIL")

        self.assertEqual(self.packaging_stats(report)["unreadable_skipped"], 1)
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (2) the original defect: every credential file unreadable -----------

    def test_all_credential_files_unreadable_is_never_a_clean_verdict_exit_1(
            self) -> None:
        """The ``findings drop 2 -> 0`` regression: the audit must still fail."""
        rel = self.package_tree()
        _baseline_code, baseline = self.audit_cli(rel.tree)
        baseline_fail = baseline["counts"]["FAIL"]
        self.assertEqual(
            len(self.findings_for_rule(baseline, "HG-PKG-LEAK-008")), 0)

        for path in PATHS:
            self.make_unreadable(rel.tree, path)
        code, report = self.audit_cli(rel.tree)

        unreadable = self.findings_for_rule(report, "HG-PKG-LEAK-008")
        self.assertEqual(len(unreadable), 2, json.dumps(
            [(f["rule"], f["file"]) for f in report["findings"]]))
        self.assertEqual({f["file"] for f in unreadable}, set(PATHS))
        self.assertTrue(all(f["severity"] == "FAIL" for f in unreadable))
        # The FAIL total must not fall.  The two credential FAILs are replaced by
        # two unreadable-file FAILs; pre-fix the count silently dropped by two
        # and both policies could return a clean verdict.
        self.assertGreaterEqual(report["counts"]["FAIL"], baseline_fail)
        self.assertEqual(
            len(self.findings_for_rule(report, "HG-PKG-LEAK-003")), 0,
            "the credential findings should be gone: the files could not be read")
        self.assertEqual(self.packaging_stats(report)["unreadable_skipped"], 2)
        self.assertNotEqual(code, crp.EXIT_OK)
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (3) both policies: an unreadable file cannot be filtered away -------

    def test_unreadable_file_still_fails_under_severity_min_fail_exit_1(
            self) -> None:
        rel = self.package_tree()
        for path in PATHS:
            self.make_unreadable(rel.tree, path)
        code, report = self.audit_cli(rel.tree, "--severity-min", "FAIL")

        unreadable = self.findings_for_rule(report, "HG-PKG-LEAK-008")
        self.assertEqual(len(unreadable), 2, "an unreadable file was filtered away")
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (4) the counter is present and is what it says ----------------------

    def test_the_unreadable_counters_agree_with_the_findings(self) -> None:
        rel = self.package_tree()
        self.make_unreadable(rel.tree, "web/tests/b.js")
        _code, report = self.audit_cli(rel.tree)
        stats = self.packaging_stats(report)
        self.assertIn("unreadable_skipped", stats)
        self.assertEqual(stats["unreadable_skipped"], 1)
        self.assertEqual(
            stats["unreadable_skipped"],
            len(self.findings_for_rule(report, "HG-PKG-LEAK-008")),
        )
        self.assertEqual(stats["unreadable_files"], [["web/tests/b.js",
                                                      "PermissionError"]])

    # -- (5) the finding tracks readability, not the file --------------------

    def test_restoring_readability_removes_the_finding_exit_1(self) -> None:
        rel = self.package_tree()
        path = self.make_unreadable(rel.tree, "tests/fixtures/a.js")
        before = self.audit_cli(rel.tree)[1]
        self.assertTrue(self.findings_for_rule(before, "HG-PKG-LEAK-008"))

        os.chmod(path, 0o644)
        code, after = self.audit_cli(rel.tree)
        self.assertEqual(self.findings_for_rule(after, "HG-PKG-LEAK-008"), [])
        # ...and the file's own credential is scanned again.
        creds = self.findings_for_rule(after, "HG-PKG-LEAK-003")
        self.assertEqual({f["file"] for f in creds}, set(PATHS))
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_unreadable_release_manifest_is_an_internal_error_exit_3(self) -> None:
        """A produced metadata file is a scan target; unreadable, it still fails.

        The release manifest is read on a path outside the content scan, so an
        unreadable one aborts the audit as an internal error (EXIT_INTERNAL, 3)
        with the path named on stderr.  Either way the run is not a pass, which
        is the property this fixture exists to pin.
        """
        rel = self.release(extra_files=dict(PATHS))
        manifest = next(rel.outdir.glob("*-manifest.txt"))
        self.addCleanup(os.chmod, manifest, stat.S_IRUSR | stat.S_IWUSR)
        os.chmod(manifest, 0)

        code, out, err = base.run_cli(str(rel.outdir), "--json")
        self.assertEqual(code, crp.EXIT_INTERNAL)
        self.assertNotEqual(code, crp.EXIT_OK)
        self.assertEqual(out, "")
        self.assertIn(manifest.name, err)
        self.assertIn("Permission denied", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
