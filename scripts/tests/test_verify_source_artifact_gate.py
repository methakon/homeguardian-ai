#!/usr/bin/env python3
"""Unit tests for scripts/verify_source_artifact_gate.py.

Every fixture is built under ``tempfile.mkdtemp()``: the suite never reads the
real repository, the real release output or the network, so it is deterministic
and safe to run anywhere.  Only the standard library is used.

Run:
    python3 -m unittest discover -s scripts/tests -t scripts
    python3 scripts/tests/test_verify_source_artifact_gate.py -v

What this suite pins
--------------------
The gate has six admission checks (A-F).  For EACH check there is a SUCCESS
fixture and a FAILURE fixture, and every fixture is non-vacuous: it asserts the
check's own status AND its evidence, so a check that silently stopped running -
or that read as a pass on an artifact it should refuse - fails the suite.  The
success fixture of each pair is the same builder call with exactly ONE input
changed to produce the failure fixture, which is what proves a single wrong
input flips the result rather than the check being a constant.

    1.  a clean artifact passes all six          TestCleanArtifact
    2.  A  licence-present                       TestCheckALicence
    3.  B  notice-present                        TestCheckBNotice
    4.  C  source-revision-recorded              TestCheckCRevision
    5.  D  manifest-digest-consistency           TestCheckDManifest
    6.  E  no-unexpected-critical-files          TestCheckECriticalFiles
    7.  F  secret-findings-explained             TestCheckFSecrets

plus the contract tests:

    8.  exit 2 (REVIEW) is NOT a pass            TestExitContract
    9.  an unreadable artifact is exit 3         TestExitContract
    10. a matched value never reaches the report TestRedactionDiscipline
    11. the real reviewed fixture entry
        (operator-secret-xyz, sha256[:8] c9387d23)
        is what the allow-list check keys on  TestCheckFSecrets
    12. a heuristic FAIL->REVIEW downgrade with no
        reviewed allow-list entry FAILS the gate
        (the part-1 bypass shape)            TestCheckFSecrets
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import verify_source_artifact_gate as gate  # noqa: E402

#: The real synthetic credential fixture the release carries in
#: tests/core/test_native_http_security.cpp line 35:
#:     std::string secret = "operator-secret-xyz";
#: This is the value the reviewed allow-list entry names.
REAL_FIXTURE_VALUE = "operator-secret-xyz"
#: The certified value sha256[:8] of the line above.
REAL_FIXTURE_SHA8 = "c9387d23"

#: A 48-char high-entropy value that begins with a magic prefix.  Under the
#: unbounded marker regex this read as a negative-test label (REVIEW) - the
#: shape of the part-1 defect, where the prefix made a real secret quiet.  The
#: marker suffix is now BOUNDED, so the shipped checker classifies this value as
#: a literal: it is a genuine FAIL and no longer needs the gate to rescue it.
PREFIXED_HIGH_ENTROPY = "dummy-" + ("Kf9Qm2Zr7LpVxTb4NwYs6HdJe3AgCu8Ma1QzRt5Bv0Ln"
                                    "Qw4Er7Ty1Ui3Op5As8Df2Gh6Jk9Lz")[:48]
BARE_HIGH_ENTROPY = ("Zq7Wm3Er9Ty1Ui5Op8As2Df6Gh4Jk0Lz9Xc7Vb5Nm3Qw1Er8Ty"
                     "6Ui4Op2As9D")[:48]

#: A word-shaped negative-test label the (bounded) marker regex still treats as a
#: label: "dummy-" plus a 7-character word.  With no reviewed allow-list entry it
#: is exactly the value the gate must refuse to call explained.
MARKER_LABEL_VALUE = "dummy-testval"

REVISION = "aac0efd7fe01f06f24ca7fd4e51653f9dfa1b314"
LICENCE_TEXT = (
    "                                 Apache License\n"
    "                           Version 2.0, January 2004\n"
    "                        http://www.apache.org/licenses/\n"
    "\n"
    "Licensed under the Apache License, Version 2.0.\n"
)
# Carries an attribution marker so the checker's HG-PKG-LIC-004 stays quiet.
NOTICE_TEXT = (
    "HomeGuardian AI\n"
    "\n"
    "This product includes software developed by third parties. Component\n"
    "attribution is recorded here; see https://example.invalid/notices.\n"
)

ARCHIVE_NAME = "homeguardian-ai-1.0.0-source.tar.gz"
TOPLEVEL = "homeguardian-ai-1.0.0-sec-1234567890-prep-abcdef1234-source"


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def base_tree():
    """A minimal, clean packaged source tree."""
    return {
        "LICENSE": LICENCE_TEXT,
        "NOTICE": NOTICE_TEXT,
        "REVISION": REVISION + "\n",
        "README.md": "# HomeGuardian AI\n\nSource artifact.\n",
        "src/plugin.js": "export const VERSION = '1.0.0';\n",
    }


class GateFixtureCase(unittest.TestCase):
    """Base class: temp dirs, a package builder and status helpers."""

    maxDiff = None

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="hggate-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    # -- fixture construction ------------------------------------------------

    def build(self, files=None, *, checksums=True, wrong_checksum=False,
              manifest=None, name=ARCHIVE_NAME, toplevel=TOPLEVEL,
              archive=True):
        """Build an artifact output directory and return a namespace.

        ``outdir`` holds the archive and its companions; ``tree`` is the
        packaged tree; ``artifact`` is what the gate is pointed at (the archive
        by default, so a sibling manifest is not auto-discovered unless the
        caller writes one).
        """
        outdir = Path(tempfile.mkdtemp(prefix="out-", dir=str(self.tmp)))
        tree = outdir / "tree"
        payload = base_tree() if files is None else dict(files)
        for rel, text in payload.items():
            target = tree / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

        archive_path = None
        if archive:
            archive_path = outdir / name
            with tarfile.open(archive_path, "w:gz") as handle:
                handle.add(tree, arcname=toplevel)

        if checksums and archive_path is not None:
            digest = _sha256_bytes(archive_path.read_bytes())
            if wrong_checksum:
                digest = "0" * 64
            (outdir / (archive_path.name + ".sha256")).write_text(
                "{}  {}\n".format(digest, archive_path.name), encoding="utf-8")

        if manifest is not None:
            (outdir / "{}-manifest.txt".format(name)).write_text(
                manifest, encoding="utf-8")

        return types.SimpleNamespace(
            outdir=outdir, tree=tree, archive=archive_path,
            artifact=str(archive_path or outdir), name=name)

    def allowlist(self, entries, name="release-secret-allowlist.json"):
        path = self.tmp / name
        path.write_text(json.dumps({"format": 1, "entries": entries}, indent=2),
                        encoding="utf-8")
        return path

    # -- gate + assertions ---------------------------------------------------

    def gate(self, artifact, **kwargs):
        return gate.verify_source_artifact(artifact, **kwargs)

    def status_of(self, result, check):
        return result["checks"][check]["status"]

    def assertAllChecks(self, result, expected, checks="ABCDEF"):
        actual = {c: self.status_of(result, c) for c in checks}
        self.assertEqual(
            actual, {c: expected for c in checks},
            "check statuses were {}\nfindings: {}".format(
                json.dumps(actual),
                json.dumps([{k: f.get(k) for k in
                             ("rule", "severity", "check", "message")}
                            for f in result["findings"]], indent=2)),
        )


# --------------------------------------------------------------------------
# 1. a clean artifact passes all six
# --------------------------------------------------------------------------

class TestCleanArtifact(GateFixtureCase):

    def test_clean_artifact_passes_all_six_checks(self):
        fixture = self.build()
        result = self.gate(fixture.artifact)
        self.assertAllChecks(result, gate.SEV_OK)
        self.assertEqual(result["status"], gate.SEV_OK)
        self.assertEqual(gate.exit_code_for(result), 0)
        # Non-vacuity: the checks that report evidence must have it.
        self.assertIn("HG-PKG-LIC-001",
                      result["checks"]["A"]["evidence"]["confirmed_by"])
        self.assertIn("HG-PKG-LIC-002",
                      result["checks"]["B"]["evidence"]["confirmed_by"])
        self.assertEqual(
            result["checks"]["C"]["evidence"]["full_sha_records"], [REVISION])
        self.assertEqual(result["checks"]["D"]["evidence"]["checksums_verified"],
                         ["sha256"])
        self.assertGreater(result["checks"]["E"]["evidence"]["entries"], 0)
        self.assertGreater(result["checks"]["E"]["evidence"]["lines_scanned"], 0)
        self.assertEqual(result["checks"]["F"]["evidence"]
                         ["credential_value_findings"], 0)
        self.assertEqual(result["total"], 0)

    def test_directory_output_is_equivalent_to_the_archive(self):
        fixture = self.build()
        from_archive = self.gate(fixture.artifact)
        from_dir = self.gate(str(fixture.outdir))
        self.assertEqual(gate.exit_code_for(from_archive),
                         gate.exit_code_for(from_dir))
        self.assertAllChecks(from_dir, gate.SEV_OK)


# --------------------------------------------------------------------------
# 2. A - licence present
# --------------------------------------------------------------------------

class TestCheckALicence(GateFixtureCase):

    def test_success_and_single_mutation_failure(self):
        files = base_tree()

        ok = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(ok, "A"), gate.SEV_OK)
        # The archive's extracted tree is gone before the gate runs, so the
        # positive confirmation is the checker's own check 1 raising no finding.
        self.assertIn("HG-PKG-LIC-001",
                      ok["checks"]["A"]["evidence"]["confirmed_by"])

        without = dict(files)
        del without["LICENSE"]
        bad = self.gate(self.build(files=without).artifact)
        self.assertEqual(self.status_of(bad, "A"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-PKG-LIC-001"
                            for f in bad["checks"]["A"]["findings"]))
        # The mutation flipped A and only A's verdict for this pair.
        self.assertNotEqual(self.status_of(ok, "A"), self.status_of(bad, "A"))

    def test_directory_input_confirms_the_entry_on_disk(self):
        fixture = self.build()
        result = self.gate(str(fixture.tree))
        self.assertEqual(self.status_of(result, "A"), gate.SEV_OK)
        self.assertEqual(result["checks"]["A"]["evidence"]["licence_entry"],
                         "LICENSE")
        self.assertTrue(result["checks"]["A"]["evidence"]["independently_walked"])

    def test_presence_is_not_passed_without_a_check_that_ran(self):
        # A structurally broken audit result must not read as a licence pass.
        result = self.gate(self.build().artifact)
        self.assertIn("HG-PKG-LIC-001",
                      result["checks"]["A"]["evidence"]["confirmed_by"])

    def test_licence_not_at_package_root_fails(self):
        # The checker requires LICENSE at the packaged root: a nested licence is
        # still missing as far as the release is concerned.
        files = base_tree()
        del files["LICENSE"]
        files["docs/LICENSE"] = LICENCE_TEXT
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "A"), gate.SEV_FAIL)


# --------------------------------------------------------------------------
# 3. B - notice present
# --------------------------------------------------------------------------

class TestCheckBNotice(GateFixtureCase):

    def test_success_and_single_mutation_failure(self):
        files = base_tree()

        ok = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(ok, "B"), gate.SEV_OK)
        self.assertTrue(ok["checks"]["B"]["evidence"]["confirmed_by"])

        without = dict(files)
        del without["NOTICE"]
        bad = self.gate(self.build(files=without).artifact)
        self.assertEqual(self.status_of(bad, "B"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-PKG-LIC-002"
                            for f in bad["checks"]["B"]["findings"]))

    def test_directory_input_confirms_the_notice_entry_on_disk(self):
        fixture = self.build()
        result = self.gate(str(fixture.tree))
        self.assertEqual(self.status_of(result, "B"), gate.SEV_OK)
        self.assertEqual(result["checks"]["B"]["evidence"]["notice_entry"],
                         "NOTICE")
        self.assertTrue(result["checks"]["B"]["evidence"]["attribution_content"])

    def test_notice_without_attribution_is_review_not_pass(self):
        # A required file that exists but says nothing about third parties is
        # not a FAIL and is emphatically not a pass.
        files = base_tree()
        files["NOTICE"] = "HomeGuardian AI\n"
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "B"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)
        self.assertFalse(result["checks"]["B"]["evidence"]["attribution_content"])


# --------------------------------------------------------------------------
# 4. C - source revision recorded
# --------------------------------------------------------------------------

class TestCheckCRevision(GateFixtureCase):

    def test_success_and_single_mutation_failure(self):
        ok = self.gate(self.build().artifact)
        self.assertEqual(self.status_of(ok, "C"), gate.SEV_OK)
        self.assertEqual(ok["checks"]["C"]["evidence"]["full_sha_record_count"], 1)

        without = base_tree()
        del without["REVISION"]
        bad = self.gate(self.build(files=without).artifact)
        self.assertEqual(self.status_of(bad, "C"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-PKG-REV-001"
                            for f in bad["checks"]["C"]["findings"]))

    def test_truncated_revision_only_is_review(self):
        files = base_tree()
        files["REVISION"] = "aac0efd\n"
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "C"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)
        self.assertEqual(result["checks"]["C"]["evidence"]["full_sha_records"], [])

    def test_archive_name_prefixes_are_not_a_full_revision(self):
        # A 10-hex prefix in the name is the packaging's own shorthand; the gate
        # must still refuse it as the only revision record.
        files = base_tree()
        del files["REVISION"]
        result = self.gate(self.build(
            files=files,
            name="homeguardian-ai-1.0.0-sec-aac0efd7fe-prep-9f3c1b2a45-source"
                 ".tar.gz").artifact)
        self.assertEqual(self.status_of(result, "C"), gate.SEV_REVIEW)
        self.assertEqual(result["checks"]["C"]["evidence"]["full_sha_records"], [])


# --------------------------------------------------------------------------
# 5. D - manifest digest consistency
# --------------------------------------------------------------------------

class TestCheckDManifest(GateFixtureCase):

    def test_success_and_single_mutation_failure(self):
        ok = self.gate(self.build().artifact)
        self.assertEqual(self.status_of(ok, "D"), gate.SEV_OK)
        self.assertEqual(ok["checks"]["D"]["evidence"]
                         ["recorded_checksum_findings"], 0)

        bad = self.gate(self.build(wrong_checksum=True).artifact)
        self.assertEqual(self.status_of(bad, "D"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-PKG-REV-004"
                            for f in bad["checks"]["D"]["findings"]))

    def test_no_digest_record_at_all_is_review_not_pass(self):
        result = self.gate(self.build(checksums=False).artifact)
        self.assertEqual(self.status_of(result, "D"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)
        self.assertEqual(result["checks"]["D"]["evidence"]["recorded_checksum_"
                                                          "findings"], 0)

    def test_recorded_baseline_that_no_longer_matches_fails(self):
        import check_release_manifest as crm

        baseline_source = self.build()
        baseline = crm.record_manifest(baseline_source.artifact)
        baseline_path = self.tmp / "baseline.json"
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")

        ok = self.gate(baseline_source.artifact, manifest=str(baseline_path))
        self.assertEqual(self.status_of(ok, "D"), gate.SEV_OK)
        self.assertIsNotNone(ok["checks"]["D"]["evidence"]["baseline"])

        # One extra file in the package: the baseline no longer describes it.
        files = base_tree()
        files["added-after-baseline.txt"] = "not in the recorded manifest\n"
        bad = self.gate(self.build(files=files).artifact,
                        manifest=str(baseline_path))
        self.assertEqual(self.status_of(bad, "D"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-MF-002"
                            for f in bad["checks"]["D"]["findings"]))


# --------------------------------------------------------------------------
# 6. E - no unexpected critical files
# --------------------------------------------------------------------------

class TestCheckECriticalFiles(GateFixtureCase):

    def test_success_and_single_mutation_failure(self):
        ok = self.gate(self.build().artifact)
        self.assertEqual(self.status_of(ok, "E"), gate.SEV_OK)
        self.assertGreater(ok["checks"]["E"]["evidence"]["lines_scanned"], 0)

        files = base_tree()
        files[".env"] = "# empty, but a key file must never be packaged\n"
        bad = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(bad, "E"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(bad), 1)
        self.assertTrue(any(f["rule"] == "HG-PKG-LEAK-001"
                            for f in bad["checks"]["E"]["findings"]))

    def test_escalation_of_review_class_critical_files_to_fail(self):
        # The underlying audit calls these REVIEW; a source hand-over refuses
        # them, and that escalation is the reason the gate exists.
        cases = {
            "build output": ("build/libplugin.o", "binary-output\n"),
            "node_modules": ("node_modules/left-pad/index.js", "module.exports=1;\n"),
            "local state": ("data/app.db", "sqlite-state\n"),
            "captured media": ("captures/front-door.mp4", "video-bytes\n"),
        }
        for label, (rel, content) in cases.items():
            with self.subTest(kind=label):
                files = base_tree()
                files[rel] = content
                result = self.gate(self.build(files=files).artifact)
                self.assertEqual(self.status_of(result, "E"), gate.SEV_FAIL,
                                 "{} should FAIL the source gate".format(label))
                self.assertEqual(gate.exit_code_for(result), 1)

    def test_scan_that_did_not_run_is_review_not_pass(self):
        # Point the gate at an empty directory: the packaging scan classifies
        # nothing, so "no unexpected critical files" was not actually checked.
        empty = self.tmp / "empty"
        empty.mkdir()
        result = self.gate(str(empty))
        self.assertEqual(self.status_of(result, "E"), gate.SEV_REVIEW)
        self.assertTrue(any(f["rule"] == "HG-GATE-E-002"
                            for f in result["checks"]["E"]["findings"]))
        # ...and it is certainly not a pass (the missing LICENSE is why the
        # overall verdict here is FAIL rather than REVIEW).
        self.assertNotEqual(gate.exit_code_for(result), 0)
        self.assertEqual(self.status_of(result, "A"), gate.SEV_FAIL)


# --------------------------------------------------------------------------
# 7. F - secret findings explained
# --------------------------------------------------------------------------

class TestCheckFSecrets(GateFixtureCase):

    def secret_files(self, value):
        return {"tests/negative_fixture.js":
                'const apiKey = "{}";\n'.format(value)}

    def test_real_fixture_value_is_the_reviewed_entry(self):
        # Self-check the fixture: the certified value and digest, and that the
        # shipped marker regex is NOT what waives it.
        import check_release_package as crp

        self.assertEqual(crp._sha256_8(REAL_FIXTURE_VALUE), REAL_FIXTURE_SHA8)
        self.assertFalse(crp.CREDENTIAL_MARKER_RE.match(REAL_FIXTURE_VALUE))

    def test_success_via_reviewed_allowlist_is_review_not_pass(self):
        files = base_tree()
        files.update(self.secret_files(REAL_FIXTURE_VALUE))
        fixture = self.build(files=files)
        allow = self.allowlist([{
            "file": "tests/negative_fixture.js",
            "line": 1,
            "pattern_class": "secret-assignment",
            "value_sha256_8": REAL_FIXTURE_SHA8,
            "reason": "negative-test label asserted by this fixture; reviewed",
        }])

        result = self.gate(fixture.artifact, secret_allowlist=str(allow))
        self.assertEqual(self.status_of(result, "F"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)
        evidence = result["checks"]["F"]["evidence"]
        self.assertEqual(evidence["allowlist_entries"], 1)
        self.assertEqual(evidence["allowlist_matched"], 1)
        self.assertEqual(evidence["allowlist_dangling"], 0)
        self.assertEqual(evidence["allowlisted"], 1)
        self.assertEqual(evidence["genuine"], 0)
        self.assertEqual(evidence["shaped_total"],
                         evidence["credential_value_findings"])

    def test_same_fixture_without_allowlist_fails(self):
        files = base_tree()
        files.update(self.secret_files(REAL_FIXTURE_VALUE))
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(result), 1)
        self.assertEqual(result["checks"]["F"]["evidence"]["genuine"], 1)

    def test_dangling_allowlist_entry_fails(self):
        # The entry names a value that is not in the artifact: it can only be
        # covering something that changed, so it must not sit unexamined.
        files = base_tree()
        files.update(self.secret_files(REAL_FIXTURE_VALUE))
        fixture = self.build(files=files)
        allow = self.allowlist([{
            "file": "tests/negative_fixture.js",
            "line": 1,
            "pattern_class": "secret-assignment",
            "value_sha256_8": "deadbeef",
        }])
        result = self.gate(fixture.artifact, secret_allowlist=str(allow))
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertEqual(result["checks"]["F"]["evidence"]["allowlist_dangling"],
                         1)
        self.assertTrue(any(f["rule"] == "HG-GATE-F-003"
                            for f in result["checks"]["F"]["findings"]))

    def test_changed_value_at_the_same_file_and_line_fails(self):
        # The allow-list is keyed by the value's digest, so editing the value -
        # which is what a real leak looks like - returns the finding to FAIL.
        files = base_tree()
        files.update(self.secret_files(BARE_HIGH_ENTROPY))
        fixture = self.build(files=files)
        allow = self.allowlist([{
            "file": "tests/negative_fixture.js",
            "line": 1,
            "pattern_class": "secret-assignment",
            "value_sha256_8": REAL_FIXTURE_SHA8,
        }])
        result = self.gate(fixture.artifact, secret_allowlist=str(allow))
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(result), 1)

    def test_unexplained_heuristic_downgrade_fails(self):
        # A value the shipped marker regex genuinely downgrades - a word-shaped
        # negative-test label - with no reviewed allow-list entry.  The checker
        # reports it as a REVIEW, and the gate must refuse to treat that as
        # explained.
        files = base_tree()
        files.update(self.secret_files(MARKER_LABEL_VALUE))
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(result), 1)
        evidence = result["checks"]["F"]["evidence"]
        self.assertEqual(evidence["unexplained_downgrades"], 1)
        self.assertEqual(evidence["genuine"], 0)
        self.assertTrue(any(f["rule"] == "HG-GATE-F-002"
                            for f in result["checks"]["F"]["findings"]))

    def test_prefixed_high_entropy_is_now_a_genuine_fail(self):
        # THE PART-1 SHAPE, AFTER THE MARKER SUFFIX WAS BOUNDED: a high-entropy
        # value carrying a magic prefix used to be excused as a label and needed
        # this gate to catch it.  The checker now classifies it as a literal, so
        # the gate sees a plain genuine FAIL - strictly stronger, with no
        # downgrade left to explain.
        import check_release_package as crp

        self.assertIsNone(crp.CREDENTIAL_MARKER_RE.match(PREFIXED_HIGH_ENTROPY))
        files = base_tree()
        files.update(self.secret_files(PREFIXED_HIGH_ENTROPY))
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertEqual(gate.exit_code_for(result), 1)
        evidence = result["checks"]["F"]["evidence"]
        self.assertEqual(evidence["unexplained_downgrades"], 0)
        self.assertEqual(evidence["genuine"], 1)

    def test_same_shape_is_waivable_only_by_a_reviewed_entry(self):
        # ...and the escape hatch is data, not a rule change: the identical
        # artifact passes to REVIEW once, and only once, a reviewed entry names
        # that exact value at that exact location.
        files = base_tree()
        files.update(self.secret_files(PREFIXED_HIGH_ENTROPY))
        fixture = self.build(files=files)
        import check_release_package as crp

        allow = self.allowlist([{
            "file": "tests/negative_fixture.js",
            "line": 1,
            "pattern_class": "secret-assignment",
            "value_sha256_8": crp._sha256_8(PREFIXED_HIGH_ENTROPY),
            "reason": "reviewed synthetic fixture",
        }])
        result = self.gate(fixture.artifact, secret_allowlist=str(allow))
        self.assertEqual(self.status_of(result, "F"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)
        self.assertEqual(result["checks"]["F"]["evidence"]
                         ["unexplained_downgrades"], 0)

    def test_a_swallowed_finding_fails_reconciliation(self):
        # Inject a shape counter that does not reconcile with the findings that
        # were reported: a credential finding that reaches no report must never
        # read as a pass.
        files = base_tree()
        files.update(self.secret_files(REAL_FIXTURE_VALUE))
        fixture = self.build(files=files)
        original = gate.crp.audit_package

        def doctored(*args, **kwargs):
            result = original(*args, **kwargs)
            shapes = result["stats"]["packaging"]["credential_value_shapes"]
            shapes["literal"] = shapes.get("literal", 0) + 1
            return result

        gate.crp.audit_package = doctored
        try:
            result = self.gate(fixture.artifact)
        finally:
            gate.crp.audit_package = original
        self.assertEqual(self.status_of(result, "F"), gate.SEV_FAIL)
        self.assertTrue(any(f["rule"] == "HG-GATE-F-004"
                            for f in result["checks"]["F"]["findings"]))


# --------------------------------------------------------------------------
# 8-9. the exit-code contract
# --------------------------------------------------------------------------

class TestExitContract(GateFixtureCase):

    def test_exit_codes_are_the_documented_contract(self):
        self.assertEqual(gate.exit_code_for({"status": gate.SEV_OK}), 0)
        self.assertEqual(gate.exit_code_for({"status": gate.SEV_FAIL}), 1)
        self.assertEqual(gate.exit_code_for({"status": gate.SEV_REVIEW}), 2)
        self.assertEqual(gate.exit_code_for({}), 3)

    def test_review_is_not_a_pass(self):
        files = base_tree()
        files["NOTICE"] = "HomeGuardian AI\n"
        result = self.gate(self.build(files=files).artifact)
        self.assertEqual(result["status"], gate.SEV_REVIEW)
        self.assertNotEqual(gate.exit_code_for(result), 0)
        self.assertEqual(gate.exit_code_for(result), 2)

    def test_missing_artifact_is_exit_3_not_a_pass(self):
        with self.assertRaises(gate.GateInputError):
            self.gate(str(self.tmp / "does-not-exist.tar.gz"))
        code = gate.main([str(self.tmp / "does-not-exist.tar.gz"), "--quiet"])
        self.assertEqual(code, 3)

    def test_cli_exit_matches_the_api_on_a_clean_artifact(self):
        fixture = self.build()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = gate.main([fixture.artifact, "--json"])
        self.assertEqual(code, 0)
        payload = json.loads(buffer.getvalue())
        self.assertEqual(payload["status"], gate.SEV_OK)
        self.assertEqual(payload["program"], "verify_source_artifact_gate")


# --------------------------------------------------------------------------
# 10. redaction discipline
# --------------------------------------------------------------------------

class TestRedactionDiscipline(GateFixtureCase):

    def test_no_planted_value_reaches_the_report(self):
        files = base_tree()
        files.update({
            "tests/bare_fixture.js":
                'const apiKey = "{}";\n'.format(BARE_HIGH_ENTROPY),
            "tests/negative_fixture.js":
                'const apiKey = "{}";\n'.format(REAL_FIXTURE_VALUE),
        })
        result = self.gate(self.build(files=files).artifact)
        blob = json.dumps(result, sort_keys=True)
        for value in (BARE_HIGH_ENTROPY, REAL_FIXTURE_VALUE,
                      PREFIXED_HIGH_ENTROPY):
            self.assertNotIn(value, blob)
        # The redacted identifying detail is present instead.
        credential_findings = [f for f in result["findings"]
                               if f.get("pattern_class") is not None]
        self.assertTrue(credential_findings)
        for finding in credential_findings:
            self.assertEqual(finding["pattern_class"], "secret-assignment")
            self.assertIsInstance(finding["value_length"], int)
            self.assertRegex(finding["value_sha256_8"], r"^[0-9a-f]{8}$")

    def test_rendered_text_does_not_leak_the_value(self):
        files = base_tree()
        files.update({"tests/bare_fixture.js":
                      'const apiKey = "{}";\n'.format(BARE_HIGH_ENTROPY)})
        result = self.gate(self.build(files=files).artifact)
        rendered = gate.render_text(result)
        self.assertNotIn(BARE_HIGH_ENTROPY, rendered)
        self.assertIn("sha256[:8]", rendered)


# --------------------------------------------------------------------------
# backwards-compatible import surface
# --------------------------------------------------------------------------

class TestModuleSurface(GateFixtureCase):

    def test_public_helpers_exist_and_are_importable(self):
        for name in ("verify_source_artifact", "exit_code_for", "render_text",
                     "build_parser", "main", "GATE_CHECKS", "RULE_TO_CHECK"):
            self.assertTrue(hasattr(gate, name), name)
        self.assertEqual([key for key, _ in gate.GATE_CHECKS],
                         ["A", "B", "C", "D", "E", "F"])

    def test_verify_directory_without_archive_or_checksum_is_review(self):
        tree = self.tmp / "bare-tree"
        tree.mkdir()
        (tree / "LICENSE").write_text(LICENCE_TEXT, encoding="utf-8")
        (tree / "NOTICE").write_text(NOTICE_TEXT, encoding="utf-8")
        (tree / "REVISION").write_text(REVISION + "\n", encoding="utf-8")
        result = self.gate(str(tree))
        self.assertEqual(self.status_of(result, "A"), gate.SEV_OK)
        self.assertEqual(self.status_of(result, "B"), gate.SEV_OK)
        self.assertEqual(self.status_of(result, "C"), gate.SEV_OK)
        self.assertEqual(self.status_of(result, "D"), gate.SEV_REVIEW)
        self.assertEqual(gate.exit_code_for(result), 2)


if __name__ == "__main__":  # pragma: no cover - test entry point
    unittest.main(verbosity=2)
