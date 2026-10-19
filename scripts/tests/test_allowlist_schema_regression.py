#!/usr/bin/env python3
"""Regression fixtures - the secret allow-list schema is validated, fail closed.

DEFECT (robustness)
-------------------
``scripts/check_release_package.py``'s ``load_secret_allowlist`` validated the
ENTRIES (all four match fields required, integer line) but never the DOCUMENT:
``SECRET_ALLOWLIST_FORMAT`` was defined and never compared, an unknown top-level
key was ignored, and an extra per-entry key was ignored.  A drifted allow-list
(``format: 99``, a stray key, a typo'd field name) therefore changed nothing that
was visible - the audit's own config silently disagreed with the schema it was
written against.

FIX
---
The format version must be present and equal ``SECRET_ALLOWLIST_FORMAT``; the
document may carry only ``("format", "entries")`` and an entry only
``("file", "line", "pattern_class", "value_sha256_8", "reason")``.  Any violation
REJECTS the whole file: no entry is loaded, every credential-shaped value stays
FAIL, and each violation is reported as a named ``HG-PKG-LEAK-009`` FAIL finding.

EXPECTED EXIT CODES (also in the test names)
--------------------------------------------
* ``..._exit_1``  - the package's own credential findings force FAIL (1); every
  case here is exit 1, and the discriminating assertions are ``applied``,
  ``HG-PKG-LEAK-009`` and the target finding's severity.
* ``..._exit_2``  - only REVIEW findings remain (2).

Non-vacuity: every rejection case is contrasted with the same package and the
same *entry* under a valid schema, which is asserted to be applied - so the
fixture proves the schema check is what rejected it, not the entry.

Run:
    python3 scripts/tests/test_allowlist_schema_regression.py -v
"""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS.parent))
sys.path.insert(0, str(TESTS))

import check_release_package as crp  # noqa: E402
import test_check_release_package as base  # noqa: E402

TARGET_FILE = "web_mcp/tests/auth.test.js"
TARGET_CLASS = "secret-assignment"
SCHEMA_RULE = "HG-PKG-LEAK-009"


class AllowlistSchemaRegression(base.FixtureCase):
    """An unknown-schema allow-list must be rejected loudly, not ignored."""

    def synthetic_package(self):
        return self.release(extra_files=dict(base.SYNTHETIC_TEST_SECRETS))

    def baseline_target(self, rel):
        """The credential finding the reviewed entry will name."""
        result = self.audit(rel.outdir, script=rel.script)
        for finding in result["findings"]:
            if (finding["rule"] == "HG-PKG-LEAK-003"
                    and finding["file"] == TARGET_FILE
                    and finding["pattern_class"] == TARGET_CLASS):
                return finding
        self.fail("no HG-PKG-LEAK-003 finding for {} / {}".format(
            TARGET_FILE, TARGET_CLASS))

    def entry_for(self, target):
        return {
            "file": target["file"],
            "line": target["line"],
            "pattern_class": target["pattern_class"],
            "value_sha256_8": target["value_sha256_8"],
            "reason": "negative-test password asserted by the JS test",
        }

    def allowlist(self, payload, name="allowlist-schema.json"):
        path = self.tmp / name
        path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return path

    def run_with(self, rel, allow):
        code, out, err = base.run_cli(
            str(rel.outdir), "--package-script", str(rel.script), "--json",
            "--secret-allowlist", str(allow))
        return code, json.loads(out), err

    @staticmethod
    def rule_findings(report, rule):
        return [f for f in report["findings"] if f["rule"] == rule]

    def severity_of_target(self, report):
        for finding in report["findings"]:
            if (finding["rule"] == "HG-PKG-LEAK-003"
                    and finding["file"] == TARGET_FILE
                    and finding["pattern_class"] == TARGET_CLASS):
                return finding["severity"]
        self.fail("the target finding disappeared: {}".format(json.dumps(
            [(f["rule"], f["file"]) for f in report["findings"]])))

    # -- (0) the contrast: a valid schema applies the entry, exit 1 ----------

    def test_valid_schema_applies_the_entry_exit_1(self) -> None:
        """Non-vacuity guard: the same entry under a valid schema IS applied."""
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        allow = self.allowlist({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": [self.entry_for(target)],
        })
        code, report, _err = self.run_with(rel, allow)

        self.assertEqual(self.rule_findings(report, SCHEMA_RULE), [])
        self.assertEqual(self.severity_of_target(report), "REVIEW")
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 1)
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 1)
        self.assertEqual(code, crp.EXIT_FAIL)  # the other 7 fixtures remain FAIL

    # -- (1) the three unvalidated gaps, each rejected -----------------------

    def test_format_99_is_rejected_exit_1(self) -> None:
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        allow = self.allowlist({
            "format": 99,
            "entries": [self.entry_for(target)],
        })
        code, report, err = self.run_with(rel, allow)

        problems = self.rule_findings(report, SCHEMA_RULE)
        self.assertEqual(len(problems), 1, json.dumps(report["findings"]))
        self.assertEqual(problems[0]["severity"], "FAIL")
        self.assertIn("format", problems[0]["message"])
        # Fail closed: the entry was NOT applied, so the value stays FAIL.
        self.assertEqual(self.severity_of_target(report), "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)
        # The finding names the offending config file.
        self.assertIn(allow.name, problems[0]["file"])

    def test_missing_format_is_rejected_exit_1(self) -> None:
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        allow = self.allowlist({"entries": [self.entry_for(target)]})
        code, report, _err = self.run_with(rel, allow)

        problems = self.rule_findings(report, SCHEMA_RULE)
        self.assertEqual(len(problems), 1, json.dumps(report["findings"]))
        self.assertIn("no 'format'", problems[0]["message"])
        self.assertEqual(self.severity_of_target(report), "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_unknown_top_level_key_is_rejected_exit_1(self) -> None:
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        allow = self.allowlist({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": [self.entry_for(target)],
            "unexpected": True,
        })
        code, report, _err = self.run_with(rel, allow)

        problems = self.rule_findings(report, SCHEMA_RULE)
        self.assertEqual(len(problems), 1, json.dumps(report["findings"]))
        self.assertIn("unknown top-level key", problems[0]["message"])
        self.assertIn("unexpected", problems[0]["message"])
        self.assertEqual(self.severity_of_target(report), "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_extra_entry_key_is_rejected_exit_1(self) -> None:
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        entry = self.entry_for(target)
        entry["value_sha256"] = "a-different-field-name"
        allow = self.allowlist({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": [entry],
        })
        code, report, _err = self.run_with(rel, allow)

        problems = self.rule_findings(report, SCHEMA_RULE)
        self.assertEqual(len(problems), 1, json.dumps(report["findings"]))
        self.assertIn("unknown key", problems[0]["message"])
        self.assertIn("value_sha256", problems[0]["message"])
        self.assertEqual(self.severity_of_target(report), "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (2) fail closed: rejection is exactly "no allow-list at all" --------

    def test_rejection_is_indistinguishable_from_no_allowlist_exit_1(self) -> None:
        """Every credential stays FAIL, and nothing is downgraded."""
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        without = self.audit(rel.outdir, script=rel.script)

        allow = self.allowlist({
            "format": 99,
            "entries": [self.entry_for(target), "not-even-an-object"],
            "extra_top": "yes",
        })
        code, report, _err = self.run_with(rel, allow)

        def credential_severities(result):
            return sorted((f["file"], f["severity"]) for f in result["findings"]
                          if f["rule"] == "HG-PKG-LEAK-003")

        self.assertEqual(credential_severities(report),
                         credential_severities(without))
        self.assertTrue(all(f["severity"] == "FAIL"
                            for f in self.rule_findings(report, "HG-PKG-LEAK-003")))
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(code, crp.EXIT_FAIL)
        # Both file-level violations are named, one finding each.
        self.assertEqual(len(self.rule_findings(report, SCHEMA_RULE)), 2)

    # -- (3) the loader contract is unchanged for keys-only callers ----------

    def test_loader_still_returns_a_two_value_contract(self) -> None:
        """A caller that wants only the keys is unaffected by the validation."""
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        allow = self.allowlist({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": [self.entry_for(target)],
        })
        keys, notes = crp.load_secret_allowlist(allow)
        self.assertEqual(len(keys), 1)
        self.assertTrue(any("reviewed entr" in note for note in notes))

        bad = self.allowlist({"format": 99, "entries": [self.entry_for(target)]},
                             name="bad.json")
        keys, notes = crp.load_secret_allowlist(bad)
        self.assertEqual(keys, {})
        self.assertTrue(any("REJECTED" in note for note in notes))

        problems: list[str] = []
        keys, _notes = crp.load_secret_allowlist(bad, problems)
        self.assertEqual(keys, {})
        self.assertEqual(len(problems), 1)
        self.assertIn("format", problems[0])

    def test_a_malformed_entry_is_still_rejected_by_the_entry_rules(self) -> None:
        """Pre-existing behaviour kept: a bad entry never widens suppression."""
        rel = self.synthetic_package()
        target = self.baseline_target(rel)
        broken = self.entry_for(target)
        del broken["line"]
        allow = self.allowlist({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": [broken],
        })
        code, report, _err = self.run_with(rel, allow)
        self.assertEqual(self.rule_findings(report, SCHEMA_RULE), [])
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 0)
        self.assertEqual(self.severity_of_target(report), "FAIL")
        self.assertEqual(code, crp.EXIT_FAIL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
