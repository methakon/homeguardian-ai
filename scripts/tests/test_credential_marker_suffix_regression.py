#!/usr/bin/env python3
"""Regression fixtures - the credential-marker suffix must be bounded.

DEFECT (fail-open, blocker)
---------------------------
``scripts/check_release_package.py``'s ``CREDENTIAL_MARKER_RE`` classified a
value as a "negative-test label" (and downgraded it FAIL -> REVIEW) whenever its
literal text merely *started* with one of the marker tokens, because every
suffix wildcard was unbounded::

    fake(?:[-_][a-z0-9_-]*)?

A real credential whose value happens to be called ``fake-<32 hex>`` therefore
became a REVIEW - and under ``--severity-min FAIL`` that REVIEW is filtered out,
so the finding disappeared entirely and the audit exited 0.

FIX
---
Bound the suffix to a 1-8 character word (``[-_][a-z][a-z0-9]{0,7}``).  A label
shrank to a word; a credential did not.

EXPECTED EXIT CODES (also in the test names)
--------------------------------------------
* ``..._exit_1``  - a FAIL credential finding was reported -> EXIT_FAIL (1).
* ``..._exit_2``  - only a REVIEW (downgraded label) was reported -> EXIT_REVIEW
  (2).
* ``..._exit_0``  - nothing passed the policy's severity floor -> EXIT_OK (0).

NOTE ON VALUE LENGTH: the scanner's ``secret-assignment`` pattern only matches a
value of 12 or more characters (``[A-Za-z0-9_\\-\\.\\+/]{12,}``).  Short labels
such as ``fake-x`` therefore never reach the marker decision at all; they are
asserted at the regex level, and the end-to-end cases use label-shaped values the
scanner actually sees (``not-the-secret``, ``sample-abcdefgh``).

Non-vacuity: every case asserts the finding EXISTS with the expected severity, so
a run that stopped scanning cannot pass; and both policies are exercised so a
"filtered out" result cannot masquerade as a pass.

Run:
    python3 scripts/tests/test_credential_marker_suffix_regression.py -v
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS.parent))
sys.path.insert(0, str(TESTS))

import check_release_package as crp  # noqa: E402
import test_check_release_package as base  # noqa: E402

#: A 32-character hex suffix - the shape of a real key, not of a label.
HEX32 = "0123456789abcdef0123456789abcdef"

#: Labels the owner named as required to stay downgraded, plus one more that the
#: shipping package carries.  All are at least 12 characters so the scanner's
#: secret-assignment pattern actually matches them.
MARKER_LABELS = ("not-the-secret", "sample-abcdefgh", "invalid-operator",
                 "dummy-testing", "wrong-valuexx")

#: Short labels: below the scanner's 12-character floor, so they are asserted at
#: the regex level only.
SHORT_LABELS = ("fake-x", "dummy-test", "not-the-secret", "example")


class BoundedMarkerSuffixRegression(base.FixtureCase):
    """A value is a label only when it *is* one, not when it starts with one."""

    def package_with(self, value: str):
        """A release package whose only credential-shaped line holds ``value``."""
        return self.release(extra_files={
            "tests/fixtures/marker_probe.txt": 'secret = "%s"\n' % value,
        })

    def audit_cli(self, rel, *extra):
        """Run the real CLI so the assertion is on a process exit code."""
        code, out, _err = base.run_cli(
            str(rel.outdir), "--package-script", str(rel.script), "--json", *extra)
        return code, json.loads(out)

    @staticmethod
    def credential_findings(report):
        return [f for f in report["findings"] if f["rule"] == "HG-PKG-LEAK-003"]

    def credential_severity(self, rel, *extra):
        code, report = self.audit_cli(rel, *extra)
        found = self.credential_findings(report)
        self.assertEqual(
            len(found), 1,
            "the scanner must see exactly one credential value: {}".format(
                json.dumps([(f["rule"], f["file"]) for f in report["findings"]])),
        )
        return code, found[0]["severity"], report

    # -- (1) the blocker: a hex-suffixed "fake" value is a credential ---------

    def test_fake_with_32_hex_suffix_is_fail_exit_1(self) -> None:
        """``fake-<32 hex>`` must be FAIL, not a downgraded label (exit 1)."""
        rel = self.package_with("fake-" + HEX32)
        code, severity, report = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)
        self.assertEqual(report["counts"], {"FAIL": 1, "REVIEW": 0})

    def test_fake_with_32_hex_suffix_stays_fail_under_severity_min_fail_exit_1(
            self) -> None:
        """The same value must NOT vanish under ``--severity-min FAIL``."""
        rel = self.package_with("fake-" + HEX32)
        code, report = self.audit_cli(rel, "--severity-min", "FAIL")
        found = self.credential_findings(report)
        self.assertEqual(len(found), 1, "the finding was filtered away")
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_dummy_with_32_hex_suffix_is_fail_exit_1(self) -> None:
        rel = self.package_with("dummy-" + HEX32)
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_sample_with_32_hex_suffix_is_fail_exit_1(self) -> None:
        rel = self.package_with("sample-" + HEX32)
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_invalid_with_32_hex_suffix_is_fail_exit_1(self) -> None:
        rel = self.package_with("invalid-" + HEX32)
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_wrong_with_32_hex_suffix_is_fail_exit_1(self) -> None:
        rel = self.package_with("wrong-" + HEX32)
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_uppercase_marker_token_with_hex_suffix_is_fail_exit_1(self) -> None:
        """The marker regex is case-insensitive; the fix must not change that."""
        rel = self.package_with("FAKE-" + HEX32)
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual(severity, "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (2) the preservation set: real labels stay REVIEW -------------------

    def test_short_named_labels_still_match_the_marker_regex(self) -> None:
        """``fake-x``/``dummy-test``/``not-the-secret``/``example`` stay markers."""
        for value in SHORT_LABELS:
            with self.subTest(value=value):
                self.assertIsNotNone(crp.CREDENTIAL_MARKER_RE.match(value))

    def test_label_shaped_values_stay_review_exit_2(self) -> None:
        """A word-shaped suffix keeps its auto-downgrade, end to end (exit 2)."""
        for value in MARKER_LABELS:
            with self.subTest(value=value):
                rel = self.package_with(value)
                code, severity, report = self.credential_severity(rel)
                self.assertEqual(severity, "REVIEW")
                self.assertEqual(code, crp.EXIT_REVIEW)
                # A downgrade is still a reported finding, never a suppression.
                self.assertEqual(report["counts"], {"FAIL": 0, "REVIEW": 1})

    def test_a_review_label_is_filtered_only_by_an_explicit_policy_exit_0(
            self) -> None:
        """``--severity-min FAIL`` narrowing is a policy choice, not a suppression.

        Non-vacuity: the same package is REVIEW (exit 2) under the default policy
        and only *then* is it expected to disappear under the stricter one.
        """
        rel = self.package_with("not-the-secret")
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual((code, severity), (crp.EXIT_REVIEW, "REVIEW"))

        code, report = self.audit_cli(rel, "--severity-min", "FAIL")
        self.assertEqual(self.credential_findings(report), [])
        self.assertEqual(code, crp.EXIT_OK)

    # -- (3) the bound itself is observable and documented -------------------

    def test_the_suffix_bound_is_eight_characters_exit_1(self) -> None:
        """An 8-character word is a label; a 9-character one is not.

        This pins the exact boundary the fix chose.  A multi-word negative-test
        label whose suffix is longer than the bound (for example
        ``invalid-operator-secret``, suffix length 15) consequently classifies as
        a literal and FAILs - a deliberate narrowing of the auto-downgrade,
        recorded here so it cannot pass unnoticed.
        """
        self.assertIsNotNone(crp.CREDENTIAL_MARKER_RE.match("fake-abcdefgh"))
        self.assertIsNone(crp.CREDENTIAL_MARKER_RE.match("fake-abcdefghi"))

        rel = self.package_with("fake-abcdefgh")
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual((code, severity), (crp.EXIT_REVIEW, "REVIEW"))

        rel = self.package_with("fake-abcdefghi")
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual((code, severity), (crp.EXIT_FAIL, "FAIL"))

        rel = self.package_with("invalid-operator-secret")
        code, severity, _ = self.credential_severity(rel)
        self.assertEqual((code, severity), (crp.EXIT_FAIL, "FAIL"))

    # -- (4) pattern-level non-vacuity ---------------------------------------

    def test_marker_regex_suffix_is_bounded(self) -> None:
        """The unbounded-suffix regression, asserted on the compiled pattern."""
        pattern = crp.CREDENTIAL_MARKER_RE.pattern
        self.assertNotIn("[-_][a-z0-9_-]*", pattern,
                         "an unbounded marker suffix remains on the pattern")
        self.assertIn("{0,7}", pattern)
        self.assertTrue(pattern.startswith("(?i)^(?:"), pattern)
        self.assertTrue(pattern.endswith(")$"), pattern)
        for value in ("fake-" + HEX32, "dummy-" + HEX32, "sample-x" + HEX32,
                      "wrong-" + HEX32, "invalid-" + HEX32):
            with self.subTest(value=value):
                self.assertIsNone(crp.CREDENTIAL_MARKER_RE.match(value))
        for value in SHORT_LABELS:
            with self.subTest(value=value):
                self.assertIsNotNone(crp.CREDENTIAL_MARKER_RE.match(value))

    def test_the_value_is_never_printed_in_either_severity(self) -> None:
        for value in ("fake-" + HEX32, "not-the-secret"):
            with self.subTest(value=value):
                rel = self.package_with(value)
                _code, out, err = base.run_cli(
                    str(rel.outdir), "--package-script", str(rel.script), "--json")
                self.assertIn("value_sha256_8", out)
                self.assertNotIn(value, out)
                self.assertNotIn(value, err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
