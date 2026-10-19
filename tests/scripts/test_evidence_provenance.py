#!/usr/bin/env python3
"""Tests for scripts/verify_evidence_provenance.py.

The validator is what stands between a submission document and a claim it cannot support, so these
tests exercise it against purpose-built git repositories rather than mocks — a real ``git init``, real
commits, real branches. Every case the task asked for is covered, plus the two failure classes the
tool exists for (a release document citing evidence that is not in the release, and a digest that does
not match).

Run: python3 -m unittest discover -s tests/scripts -v
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VALIDATOR = os.path.join(REPO_ROOT, "scripts", "verify_evidence_provenance.py")

# Content the validator reads to decide whether a declared evidence type is supportable.
REAL_LOG = (
    "=====================================================================\n"
    " REAL camera capture -> bounded scalars -> one finding per frame\n"
    " device=/dev/video0  requested=640x480  frames=90\n"
    "=====================================================================\n"
    "    provenance         : real (a physical device frame)\n"
    "  MOTION-VERDICT       : NO sustained increase observed\n"
)
SYNTHETIC_TEST = (
    "//   There is no camera here either: the input is a deterministic synthetic step.\n"
    "        (void)session.ingest(frame.data(), frame.size(), Provenance::synthetic, now);\n"
)
TRANSCRIBED_LOG = (
    "# capture log\n"
    "# these values were transcribed from the capture's own stdout\n"
    "but this file is a transcription, not the original stream - treat it as such.\n"
    "  peak motion / ratio  : 0.1252 / 2.70\n"
    "  MOTION-VERDICT       : a sustained increase WAS observed\n"
)


def git(root: str, *args: str) -> str:
    proc = subprocess.run(["git", "-C", root, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, check=False)
    if proc.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {proc.stdout}")
    return proc.stdout.strip()


class TempRepo:
    """A throwaway git repository with two branches: ``release`` and ``external``."""

    def __init__(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="hg-evidence-test-")
        self.root = self._tmp.name
        git(self.root, "init", "-q", "-b", "release")
        git(self.root, "config", "user.email", "test@example.invalid")
        git(self.root, "config", "user.name", "Evidence Test")
        git(self.root, "config", "commit.gpgsign", "false")

    def write(self, path: str, content: str) -> str:
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(content)
        return path

    def commit(self, message: str = "commit") -> str:
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", message)
        return git(self.root, "rev-parse", "HEAD")

    def branch(self, name: str) -> None:
        git(self.root, "checkout", "-q", "-b", name)

    def checkout(self, name: str) -> None:
        git(self.root, "checkout", "-q", name)

    def manifest(self, artifacts: list[dict], refs: dict | None = None) -> str:
        doc = {
            "schema": "hg-evidence-manifest/1",
            "refs": refs or {"release": "release", "external": "external"},
            "artifacts": artifacts,
        }
        return self.write("manifest.json", json.dumps(doc, indent=2))

    def run_validator(self, manifest_path: str, *extra: str) -> tuple[int, dict]:
        out = os.path.join(self.root, "report.json")
        proc = subprocess.run(
            [sys.executable, VALIDATOR, "--root", self.root, "--manifest",
             os.path.join(self.root, manifest_path), "--json-out", out, "--quiet", *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
        if os.path.exists(out):
            with open(out, encoding="utf-8") as handle:
                report = json.load(handle)
        else:
            report = {}
        return proc.returncode, report

    def close(self) -> None:
        self._tmp.cleanup()


class EvidenceProvenanceValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempRepo()

    def tearDown(self) -> None:
        self.repo.close()

    # --- required case 1: a valid committed artifact -------------------------------------------
    def test_valid_committed_artifact_passes(self) -> None:
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit("add evidence")
        manifest = self.repo.manifest([{
            "id": "real-log", "path": "docs/evidence/CAPTURE.log", "revision": "release",
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))
        entry = report["artifacts"][0]
        self.assertTrue(entry["exists"])
        self.assertEqual(entry["evidence_type"], "real")
        self.assertTrue(entry["classification_verified"])
        self.assertEqual(len(entry["sha256"]), 64)
        self.assertTrue(entry["in_release"])

    # --- required case 2: a missing artifact ----------------------------------------------------
    def test_missing_artifact_fails(self) -> None:
        self.repo.write("docs/evidence/OTHER.log", REAL_LOG)
        self.repo.commit("add something else")
        manifest = self.repo.manifest([{
            "id": "absent", "path": "docs/evidence/NEVER_WRITTEN.log", "revision": "release",
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["missing_artifact"])
        self.assertFalse(report["artifacts"][0]["exists"])
        self.assertIsNone(report["artifacts"][0]["sha256"])

    # --- required case 3: a wrong git revision --------------------------------------------------
    def test_unresolvable_revision_fails(self) -> None:
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "bad-rev", "path": "docs/evidence/CAPTURE.log",
            "revision": "no-such-branch", "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["unresolved_revision"])

    def test_file_absent_at_the_declared_earlier_revision_fails(self) -> None:
        """A real revision, but the artifact was only added later: still a failed reference."""
        self.repo.write("README.md", "release notes\n")
        first = self.repo.commit("before the evidence existed")
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit("add evidence")
        manifest = self.repo.manifest([{
            "id": "too-early", "path": "docs/evidence/CAPTURE.log", "revision": first,
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["missing_artifact"])
        self.assertIn(first[:8], report["findings"][0]["detail"])

    # --- required case 4: a valid external-branch reference --------------------------------------
    def test_valid_external_branch_reference_passes(self) -> None:
        self.repo.write("docs/notes.md", "release notes\n")
        self.repo.commit("release side")
        self.repo.branch("external")
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.write("docs/evidence/README.md", "see docs/evidence/CAPTURE.log\n")
        self.repo.commit("external evidence")
        manifest = self.repo.manifest([{
            "id": "external-log", "path": "docs/evidence/CAPTURE.log", "revision": "external",
            "scope": "external", "evidence_type": "real",
            "referenced_by": [{"path": "docs/evidence/README.md", "revision": "external"}],
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))
        entry = report["artifacts"][0]
        self.assertTrue(entry["in_external"])
        self.assertFalse(entry["in_release"])
        self.assertTrue(entry["referenced_by"][0]["reference_confirmed"])
        # The release document does not mention it, so nothing in the release depends on it.
        self.assertEqual(report["summary"]["external_cited_by_release"], 0)

    # --- required case 5: an incorrect real/synthetic classification -----------------------------
    def test_real_artifact_carrying_synthetic_provenance_is_rejected(self) -> None:
        self.repo.write("docs/evidence/MIXED.log", REAL_LOG +
                        '    "provenance": "synthetic",\n')
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "misclassified", "path": "docs/evidence/MIXED.log", "revision": "release",
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertIn("classification_contradicted", [f["kind"] for f in report["findings"]])
        self.assertFalse(report["artifacts"][0]["classification_verified"])

    def test_synthetic_artifact_without_a_synthetic_marker_is_rejected(self) -> None:
        self.repo.write("tests/test_thing.cpp", "int main() { return 0; }\n")
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "unverifiable", "path": "tests/test_thing.cpp", "revision": "release",
            "scope": "release", "evidence_type": "synthetic",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["marker_missing"])

    def test_synthetic_artifact_with_its_marker_is_accepted(self) -> None:
        self.repo.write("tests/test_camera.cpp", SYNTHETIC_TEST)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "fixture", "path": "tests/test_camera.cpp", "revision": "release",
            "scope": "release", "evidence_type": "synthetic",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))
        self.assertTrue(report["artifacts"][0]["classification_verified"])

    def test_transcribed_artifact_must_say_so(self) -> None:
        self.repo.write("docs/evidence/LOG.md", TRANSCRIBED_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "transcribed", "path": "docs/evidence/LOG.md", "revision": "release",
            "scope": "release", "evidence_type": "transcribed",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))

    # --- the case that motivated the tool --------------------------------------------------------
    def test_release_document_citing_absent_external_evidence_fails(self) -> None:
        self.repo.write("docs/SUBMISSION.md", "see docs/evidence/CAPTURE.log for the numbers\n")
        release = self.repo.commit("release doc that cites the evidence")
        self.repo.branch("external")
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit("external evidence")
        manifest = self.repo.manifest([{
            "id": "external-log", "path": "docs/evidence/CAPTURE.log", "revision": "external",
            "scope": "external", "evidence_type": "real",
            "referenced_by": [{"path": "docs/SUBMISSION.md", "revision": release}],
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]],
                         ["claim_not_supported_in_release"])
        self.assertEqual(report["summary"]["external_cited_by_release"], 1)

        rc_tolerant, report_tolerant = self.repo.run_validator(
            manifest, "--allow-external-references")
        self.assertEqual(rc_tolerant, 0, report_tolerant.get("findings"))
        entry = report_tolerant["artifacts"][0]
        self.assertTrue(entry["exists"])
        self.assertFalse(entry["in_release"])
        self.assertTrue(entry["referenced_by"][0]["reference_confirmed"])

    def test_reference_token_must_actually_appear_in_the_citing_document(self) -> None:
        """A document that does not mention the artifact is not a citation, so it cannot dangle."""
        self.repo.write("docs/SUBMISSION.md", "no mention of any evidence file here\n")
        release = self.repo.commit()
        self.repo.branch("external")
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "external-log", "path": "docs/evidence/CAPTURE.log", "revision": "external",
            "scope": "external", "evidence_type": "real",
            "referenced_by": [{"path": "docs/SUBMISSION.md", "revision": release}],
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))
        self.assertFalse(report["artifacts"][0]["referenced_by"][0]["reference_confirmed"])

    def test_referencing_revision_absent_from_the_clone_warns_unless_strict(self) -> None:
        """A ref this clone does not have means the cross-check could not run - not that it passed."""
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "log", "path": "docs/evidence/CAPTURE.log", "revision": "release",
            "scope": "release", "evidence_type": "real",
            "referenced_by": [{"path": "docs/SUBMISSION.md",
                               "revision": "origin/a-branch-this-clone-never-fetched"}],
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, "an unfetched ref is not a disproved reference")
        self.assertEqual(report["summary"]["references_unchecked"], 1)
        self.assertEqual(report["summary"]["failures"], 0)

        rc_strict, report_strict = self.repo.run_validator(manifest, "--strict-refs")
        self.assertEqual(rc_strict, 1)
        self.assertIn("unresolved_revision",
                      [f["kind"] for f in report_strict["findings"]])

    # --- digests and degenerate artifacts --------------------------------------------------------
    def test_declared_digest_mismatch_fails(self) -> None:
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "digest", "path": "docs/evidence/CAPTURE.log", "revision": "release",
            "scope": "release", "evidence_type": "real", "sha256": "0" * 64,
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["digest_mismatch"])

    def test_empty_artifact_fails(self) -> None:
        self.repo.write("docs/evidence/EMPTY.log", "")
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "empty", "path": "docs/evidence/EMPTY.log", "revision": "release",
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertEqual([f["kind"] for f in report["findings"]], ["empty_artifact"])

    def test_declared_release_scope_that_is_actually_absent_fails(self) -> None:
        self.repo.write("docs/notes.md", "hello\n")
        self.repo.commit()
        self.repo.branch("external")
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.commit()
        manifest = self.repo.manifest([{
            "id": "wishful", "path": "docs/evidence/CAPTURE.log", "revision": "external",
            "scope": "release", "evidence_type": "real",
        }])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 1)
        self.assertIn("scope_mismatch", [f["kind"] for f in report["findings"]])

    def test_report_is_machine_readable_and_counts_evidence_types(self) -> None:
        self.repo.write("docs/evidence/CAPTURE.log", REAL_LOG)
        self.repo.write("docs/evidence/LOG.md", TRANSCRIBED_LOG)
        self.repo.write("tests/test_camera.cpp", SYNTHETIC_TEST)
        self.repo.commit()
        manifest = self.repo.manifest([
            {"id": "r", "path": "docs/evidence/CAPTURE.log", "revision": "release",
             "scope": "release", "evidence_type": "real"},
            {"id": "t", "path": "docs/evidence/LOG.md", "revision": "release",
             "scope": "release", "evidence_type": "transcribed"},
            {"id": "s", "path": "tests/test_camera.cpp", "revision": "release",
             "scope": "release", "evidence_type": "synthetic"},
        ])
        rc, report = self.repo.run_validator(manifest)
        self.assertEqual(rc, 0, report.get("findings"))
        self.assertEqual(report["schema"], "hg-evidence-report/1")
        self.assertEqual(report["summary"]["real"], 1)
        self.assertEqual(report["summary"]["synthetic"], 1)
        self.assertEqual(report["summary"]["transcribed"], 1)
        self.assertEqual(report["summary"]["release_contained"], 3)
        self.assertEqual(report["summary"]["external"], 0)
        for entry in report["artifacts"]:
            self.assertEqual(len(entry["sha256"]), 64)
            self.assertTrue(entry["classification_verified"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
