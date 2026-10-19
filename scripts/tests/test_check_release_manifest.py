#!/usr/bin/env python3
"""Unit tests for scripts/check_release_manifest.py.

Fixtures are built under ``tempfile.mkdtemp()``; the suite never reads the real
repository, the real release output or the network.  Standard library only.

Run:
    python3 scripts/tests/test_check_release_manifest.py -v
    python3 -m unittest discover -s scripts/tests -t scripts -v

The regression fixtures for the release-QA defects are:

    * a stale recorded SECURITY revision      TestStaleSecurityRevisionBaseline
    * a correct baseline that passes with
      every check still on                    TestCorrectBaselinePassesWithoutLoosening
    * a deliberate removal resolved by
      re-recording the baseline               TestReviewedManifestRerecord
    * a wrong revision and corrupted /
      truncated / altered / short manifests   TestWrongRevisionAndCorruptedManifests
"""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))

import check_release_manifest as crm  # noqa: E402

CLI = SCRIPTS / "check_release_manifest.py"

PREP_A = "5d8cc3b39152a08d193aeea37f6cc93f00328708"
PREP_B = "c4df765041d7795a97ee9dad3bd22c722e0c403e"
SEC = "05e130395b1d3244bb38202c929902b6e1d93f28"


def package_name(sec=SEC, prep=PREP_A):
    return "homeguardian-ai-1.0.0-sec-{}10-prep-{}10".format(sec[:10], prep[:10])


TEXT_MANIFEST = """\
HomeGuardian AI — Release Package Manifest
===========================================
Status:                SECURITY APPROVAL PENDING
Version:               1.0.0
Security Baseline SHA: {sec} ({sec10})
Prep / Docs SHA:       {prep} ({prep10})
Runtime Divergence:    ZERO (src/, web_mcp/, CMake, config, demo verified identical)
Archive File:          {archive}
Size (bytes):          {size}
SHA-256:               {sha256}
SHA-512:               {sha512}
Total Files:           {files}
``PROJECT DOCS``
License:               Apache License 2.0 (see LICENSE and NOTICE)
"""

FIXTURE_FILES = {
    "README.md": "# Fixture\n\nA manifest-regression fixture.\n",
    "LICENSE": "Apache License\nVersion 2.0, January 2004\n",
    "src/main.cpp": "int main() { return 0; }\n",
    "src/core/registry.cpp": "// registry\n",
}


def run_cli(*args):
    proc = subprocess.run([sys.executable, str(CLI), *args],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


class FixtureCase(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="hgmf-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def tree(self, files=None, name="tree"):
        root = Path(tempfile.mkdtemp(prefix="tree-", dir=str(self.tmp)))
        payload = dict(FIXTURE_FILES if files is None else files)
        for rel, text in payload.items():
            if text is None:
                continue
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return root

    def archive_of(self, tree, name):
        outdir = Path(tempfile.mkdtemp(prefix="out-", dir=str(self.tmp)))
        archive = outdir / "{}-source.tar.gz".format(name)
        with tarfile.open(archive, "w:gz") as handle:
            handle.add(tree, arcname=name)
        return archive

    def release(self, *, files=None, prep=PREP_A, sec=SEC, with_text_manifest=True,
                mutate_tar=None):
        """A package directory holding an archive and (optionally) a text manifest."""
        name = package_name(sec, prep)
        tree = self.tree(files)
        if mutate_tar is not None:
            mutate_tar(tree)
        archive = self.archive_of(tree, name)
        outdir = archive.parent
        if with_text_manifest:
            import hashlib

            def digest(algo):
                d = hashlib.new(algo)
                with archive.open("rb") as handle:
                    for chunk in iter(lambda: handle.read(1 << 20), b""):
                        d.update(chunk)
                return d.hexdigest()

            count = sum(1 for rel in (files or FIXTURE_FILES)
                        if (files or FIXTURE_FILES)[rel] is not None)
            text = TEXT_MANIFEST.format(
                sec=sec, sec10=sec[:10], prep=prep, prep10=prep[:10],
                archive=archive.name, size=archive.stat().st_size,
                sha256=digest("sha256"), sha512=digest("sha512"), files=count,
            ).replace("``PROJECT DOCS``\n", "")
            (outdir / "{}-manifest.txt".format(name)).write_text(
                text, encoding="utf-8")
        return types.SimpleNamespace(outdir=outdir, archive=archive, tree=tree,
                                    name=name)

    def baseline_path(self, package, filename="baseline.json"):
        baseline = crm.record_manifest(str(package))
        path = self.tmp / filename
        path.write_text(json.dumps(baseline, indent=2, sort_keys=True),
                        encoding="utf-8")
        return path, baseline

    def rules(self, result):
        return [f["rule"] for f in result["findings"]]

    def findings_for(self, result, rule):
        return [f for f in result["findings"] if f["rule"] == rule]


# ---------------------------------------------------------------------------
# Recording and the clean round-trip
# ---------------------------------------------------------------------------


class TestRecordAndVerifyClean(FixtureCase):
    def test_record_then_verify_is_clean(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        self.assertEqual(baseline["program"], "check_release_manifest")
        self.assertEqual(baseline["format"], crm.BASELINE_FORMAT)
        self.assertEqual(len(baseline["files"]), len(FIXTURE_FILES))
        self.assertEqual(
            [entry["path"] for entry in baseline["files"]],
            sorted(FIXTURE_FILES),
        )
        for entry in baseline["files"]:
            self.assertEqual(len(entry["sha256"]), 64)
            self.assertGreaterEqual(entry["size"], 0)

        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(result["counts"], {"FAIL": 0, "REVIEW": 0})
        self.assertEqual(result["total"], 0)
        self.assertEqual(result["checks"]["files"], "PASS")
        self.assertEqual(result["checks"]["digests"], "PASS")
        self.assertEqual(result["checks"]["revision"], "PASS")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_OK)

    def test_record_records_the_source_revision_and_archive_digests(self):
        rel = self.release()
        _path, baseline = self.baseline_path(rel.archive)
        self.assertEqual(baseline["source"]["security_baseline"], SEC)
        self.assertEqual(baseline["source"]["prep"], PREP_A)
        self.assertEqual(baseline["archive"]["sha256"],
                         crm._sha256_file(rel.archive))
        self.assertEqual(baseline["archive"]["name"], rel.archive.name)

    def test_directory_and_tarball_inputs_agree(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        from_tar = crm.verify_manifest(str(rel.archive), baseline_path)
        from_dir = crm.verify_manifest(str(rel.outdir), baseline_path)
        self.assertEqual(from_tar["counts"], from_dir["counts"])
        self.assertTrue(
            all(c >= 0 for c in from_dir["counts"].values()), from_dir["counts"])
        self.assertEqual([f["rule"] for f in from_tar["findings"]],
                         [f["rule"] for f in from_dir["findings"]])
        self.assertEqual(from_tar["checks"], from_dir["checks"])

    def test_record_from_a_bare_tree_has_no_archive_digests(self):
        tree = self.tree()
        baseline = crm.record_manifest(str(tree))
        self.assertIsNone(baseline["archive"])
        self.assertEqual(baseline["package"]["origin"], "directory")
        self.assertEqual(len(baseline["files"]), len(FIXTURE_FILES))

    def test_json_round_trip_is_stable(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(json.loads(json.dumps(result)), result)
        for key in ("program", "format", "package", "manifest",
                    "manifest_format", "origin", "package_root", "package_name",
                    "summary", "source", "archive", "checks", "counts", "total",
                    "findings", "notes"):
            self.assertIn(key, result)


# ---------------------------------------------------------------------------
# Missing files
# ---------------------------------------------------------------------------


class TestMissingFiles(FixtureCase):
    def test_recorded_file_absent_is_fail(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)

        broken = dict(FIXTURE_FILES)
        del broken["src/core/registry.cpp"]
        damaged = self.release(files=broken, prep=PREP_A)

        result = crm.verify_manifest(str(damaged.archive), baseline_path)
        missing = self.findings_for(result, "HG-MF-001")
        self.assertEqual([f["path"] for f in missing],
                         ["src/core/registry.cpp"])
        self.assertEqual(missing[0]["severity"], "FAIL")
        self.assertEqual(result["summary"]["missing"], 1)
        self.assertEqual(result["checks"]["files"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_missing_file_is_also_unexpected_in_the_other_direction(self):
        """A file in the package that the baseline lacks is REVIEW, not FAIL."""
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)

        grown = dict(FIXTURE_FILES)
        grown["docs/NEW.md"] = "# new\n"
        extended = self.release(files=grown, prep=PREP_A)

        result = crm.verify_manifest(str(extended.archive), baseline_path,
                                     check_archive_digests=False)
        added = self.findings_for(result, "HG-MF-002")
        self.assertEqual([f["path"] for f in added], ["docs/NEW.md"])
        self.assertEqual(added[0]["severity"], "REVIEW")
        self.assertEqual(result["summary"]["unexpected"], 1)
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_REVIEW)

    def test_unexpected_severity_can_be_raised(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        grown = dict(FIXTURE_FILES)
        grown["docs/NEW.md"] = "# new\n"
        extended = self.release(files=grown, prep=PREP_A)

        result = crm.verify_manifest(str(extended.archive), baseline_path,
                                     unexpected_severity="FAIL")
        added = self.findings_for(result, "HG-MF-002")
        self.assertEqual(added[0]["severity"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_missing_and_unexpected_do_not_double_report_the_count(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        changed_set = dict(FIXTURE_FILES)
        del changed_set["src/core/registry.cpp"]
        changed_set["docs/NEW.md"] = "# new\n"
        altered = self.release(files=changed_set, prep=PREP_A)
        result = crm.verify_manifest(str(altered.archive), baseline_path)
        self.assertEqual(rules_count(result, "HG-MF-008"), 0,
                         "the file-count rule must not restate the per-file "
                         "findings")


def rules_count(result, rule):
    return sum(1 for f in result["findings"] if f["rule"] == rule)


# ---------------------------------------------------------------------------
# Changed files and checksums
# ---------------------------------------------------------------------------


class TestChangedContent(FixtureCase):
    def test_edited_file_same_size_is_fail(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        edited = dict(FIXTURE_FILES)
        original = "int main() { return 0; }\n"
        edited["src/main.cpp"] = "int main() { return 1; }\n"
        self.assertEqual(len(original), len(edited["src/main.cpp"]),
                         "this fixture must keep the size identical")
        mutated = self.release(files=edited, prep=PREP_A)

        result = crm.verify_manifest(str(mutated.archive), baseline_path)
        changed = self.findings_for(result, "HG-MF-003")
        self.assertEqual([f["path"] for f in changed], ["src/main.cpp"])
        self.assertEqual(changed[0]["severity"], "FAIL")
        self.assertIn("size is unchanged", changed[0]["message"])
        self.assertEqual(changed[0]["recorded_size"], changed[0]["actual_size"])
        self.assertNotEqual(changed[0]["recorded_sha256"],
                            changed[0]["actual_sha256"])
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_resized_file_is_fail(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        resized = dict(FIXTURE_FILES)
        resized["src/core/registry.cpp"] = "// registry\n// a longer body\n"
        mutated = self.release(files=resized, prep=PREP_A)
        result = crm.verify_manifest(str(mutated.archive), baseline_path)
        changed = self.findings_for(result, "HG-MF-003")
        self.assertEqual([f["path"] for f in changed], ["src/core/registry.cpp"])
        self.assertIn("byte(s)", changed[0]["message"])
        self.assertNotEqual(changed[0]["recorded_size"], changed[0]["actual_size"])

    def test_reworded_archive_digest_is_fail(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["archive"]["sha256"] = "0" * 64
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        digests = self.findings_for(result, "HG-MF-006")
        self.assertEqual(len(digests), 1, self.rules(result))
        self.assertEqual(digests[0]["severity"], "FAIL")
        self.assertEqual(result["checks"]["digests"], "FAIL")

    def test_sha512_mismatch_alone_is_detected(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["archive"]["sha512"] = "1" * 128
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        digests = self.findings_for(result, "HG-MF-006")
        self.assertEqual(len(digests), 1, self.rules(result))
        self.assertIn("SHA-512", digests[0]["message"])

    def test_archive_digest_check_can_be_skipped(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["archive"]["sha256"] = "0" * 64
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), baseline_path,
                                     check_archive_digests=False)
        self.assertNotIn("HG-MF-006", self.rules(result))


# ---------------------------------------------------------------------------
# Source revision
# ---------------------------------------------------------------------------


class TestSourceRevision(FixtureCase):
    def test_different_source_revision_is_fail(self):
        """The artefact is cut from a different prep revision than recorded."""
        original = self.release(prep=PREP_A)
        baseline_path, _ = self.baseline_path(original.archive)
        other = self.release(prep=PREP_B)
        result = crm.verify_manifest(str(other.archive), baseline_path)
        revision = self.findings_for(result, "HG-MF-004")
        self.assertEqual(len(revision), 1, self.rules(result))
        self.assertEqual(revision[0]["severity"], "FAIL")
        self.assertIn(PREP_A[:12], revision[0]["message"])
        self.assertIn(PREP_B[:12], revision[0]["message"])
        self.assertEqual(result["checks"]["revision"], "FAIL")

    def test_recorded_revision_edited_is_fail(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["source"]["revision"] = PREP_B
        baseline["source"]["prep"] = PREP_B
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(len(self.findings_for(result, "HG-MF-004")), 1,
                         self.rules(result))

    def test_revision_recorded_on_only_one_side_is_review(self):
        tree = self.tree()
        baseline = crm.record_manifest(str(tree))
        baseline["source"] = {"revision": None, "security_baseline": None,
                              "prep": None, "name_prefixes": {}}
        baseline_path = self.tmp / "norev.json"
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(tree), baseline_path)
        self.assertIn("HG-MF-005", self.rules(result))
        self.assertEqual(self.findings_for(result, "HG-MF-005")[0]["severity"],
                         "REVIEW")

    def test_short_recorded_revision_matches_a_full_actual_revision(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["source"]["revision"] = PREP_A[:10]
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertNotIn("HG-MF-004", self.rules(result))
        self.assertEqual(result["checks"]["revision"], "PASS")

    def test_identical_revision_is_pass(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(result["checks"]["revision"], "PASS")
        self.assertEqual(result["source"]["recorded_security"], SEC)
        self.assertEqual(result["source"]["actual_security"], SEC)


# ---------------------------------------------------------------------------
# The human-readable manifest form: honest, limited coverage
# ---------------------------------------------------------------------------


class TestTextManifestForm(FixtureCase):
    def test_text_manifest_is_read_and_limits_are_stated(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        result = crm.verify_manifest(str(rel.archive), manifest)
        self.assertEqual(result["manifest_format"], "text")
        self.assertEqual(result["checks"]["files"], "REVIEW")
        self.assertNotEqual(result["checks"]["text_mode_limits"], "none")
        self.assertTrue(any("human-readable form" in n for n in result["notes"]))
        self.assertEqual(result["checks"]["digests"], "PASS")
        self.assertEqual(result["checks"]["revision"], "PASS")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_REVIEW)

    def test_text_manifest_does_not_flood_unexpected_findings(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        result = crm.verify_manifest(str(rel.archive), manifest)
        self.assertEqual([f for f in result["findings"]
                          if f["rule"] == "HG-MF-002"], [],
                         "a text manifest records no per-file data, so it must "
                         "not report every packaged file as unexpected")

    def test_text_manifest_detects_a_file_count_change(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        text = manifest.read_text(encoding="utf-8")
        manifest.write_text(
            text.replace("Total Files:           4", "Total Files:           99"),
            encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), manifest)
        self.assertIn("HG-MF-008", self.rules(result))
        self.assertEqual(self.findings_for(result, "HG-MF-008")[0]["severity"],
                         "REVIEW")

    def test_text_manifest_detects_a_changed_archive_digest(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        text = manifest.read_text(encoding="utf-8")
        import re

        manifest.write_text(
            re.sub(r"^(SHA-256:\s+)[0-9a-f]{64}", r"\g<1>" + "0" * 64,
                   text, flags=re.M),
            encoding="utf-8")
        result = crm.verify_manifest(str(rel.archive), manifest)
        self.assertIn("HG-MF-006", self.rules(result))

    def test_text_manifest_selected_explicitly(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        code, out, err = run_cli(str(rel.archive), "--manifest", str(manifest),
                                 "--manifest-format", "text", "--json")
        self.assertEqual(err, "")
        self.assertEqual(json.loads(out)["manifest_format"], "text")

    def test_format_mismatch_is_an_input_error(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        code, _out, err = run_cli(str(rel.archive), "--manifest", str(manifest),
                                  "--manifest-format", "json")
        self.assertEqual(code, crm.EXIT_INTERNAL)
        self.assertIn("not JSON", err)


# ---------------------------------------------------------------------------
# Bad input, CLI behaviour, importable API
# ---------------------------------------------------------------------------


class TestInputsAndCli(FixtureCase):
    def test_unreadable_manifest_raises(self):
        rel = self.release()
        with self.assertRaises(crm.ManifestInputError):
            crm.verify_manifest(str(rel.archive), str(self.tmp / "nope.json"))

    def test_json_baseline_with_zero_files_raises(self):
        path = self.tmp / "empty.json"
        path.write_text(json.dumps({"format": 1, "files": []}), encoding="utf-8")
        with self.assertRaises(crm.ManifestInputError):
            crm.read_baseline(path)

    def test_unrecognisable_manifest_raises(self):
        path = self.tmp / "junk.txt"
        path.write_text("nothing useful here\n", encoding="utf-8")
        with self.assertRaises(crm.ManifestInputError):
            crm.read_baseline(path)

    def test_bad_package_raises(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        with self.assertRaises(crm.ManifestInputError):
            crm.verify_manifest(str(self.tmp / "missing"), baseline_path)

    def test_cli_exit_codes(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        code, _out, err = run_cli(str(rel.archive), "--manifest",
                                  str(baseline_path))
        self.assertEqual(code, crm.EXIT_OK)
        self.assertEqual(err, "")

        grown = dict(FIXTURE_FILES)
        grown["docs/NEW.md"] = "# new\n"
        extended = self.release(files=grown, prep=PREP_A)
        code, _out, _err = run_cli(str(extended.archive), "--manifest",
                                   str(baseline_path), "--no-archive-digests")
        self.assertEqual(code, crm.EXIT_REVIEW)

        code, _out, _err = run_cli(str(self.tmp / "gone"), "--manifest",
                                   str(baseline_path))
        self.assertEqual(code, crm.EXIT_INTERNAL)

    def test_cli_finds_the_text_manifest_beside_the_package(self):
        rel = self.release()
        code, out, err = run_cli(str(rel.outdir), "--json")
        self.assertEqual(err, "")
        self.assertEqual(code, crm.EXIT_REVIEW)
        self.assertEqual(json.loads(out)["manifest_format"], "text")

    def test_cli_without_a_manifest_is_an_input_error(self):
        tree = self.tree()
        code, _out, err = run_cli(str(tree))
        self.assertEqual(code, crm.EXIT_INTERNAL)
        self.assertIn("no --manifest", err)

    def test_quiet_prints_nothing(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        code, out, err = run_cli(str(rel.archive), "--manifest",
                                 str(baseline_path), "--quiet")
        self.assertEqual(code, crm.EXIT_OK)
        self.assertEqual(out, "")
        self.assertEqual(err, "")

    def test_record_writes_a_baseline_file(self):
        rel = self.release()
        target = self.tmp / "recorded.json"
        code, _out, err = run_cli(str(rel.archive), "--record", "-o",
                                  str(target))
        self.assertEqual(code, crm.EXIT_OK)
        self.assertEqual(err, "")
        baseline = json.loads(target.read_text(encoding="utf-8"))
        self.assertEqual(len(baseline["files"]), len(FIXTURE_FILES))
        result = crm.verify_manifest(str(rel.archive), target)
        self.assertEqual(result["counts"], {"FAIL": 0, "REVIEW": 0})

    def test_import_is_side_effect_free(self):
        proc = subprocess.run(
            [sys.executable, "-c",
             "import sys; sys.path.insert(0, {!r}); import check_release_manifest "
             "as m; sys.stdout.write('IMPORTED ' + m.PROGRAM)".format(str(SCRIPTS))],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "IMPORTED check_release_manifest")
        self.assertEqual(proc.stderr, "")

    def test_verify_manifest_does_not_print(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(buffer):
            result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(buffer.getvalue(), "")
        self.assertEqual(result["total"], 0)

    def test_entry_point_signatures(self):
        import inspect

        self.assertEqual(
            list(inspect.signature(crm.verify_manifest).parameters),
            ["package_path", "manifest_path", "unexpected_severity",
             "check_archive_digests"])
        self.assertEqual(list(inspect.signature(crm.record_manifest).parameters),
                         ["package_path", "archive_digests"])

    def test_bad_unexpected_severity_raises(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        with self.assertRaises(crm.ManifestInputError):
            crm.verify_manifest(str(rel.archive), baseline_path,
                                unexpected_severity="INFO")

    def test_report_never_contains_file_contents(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        blob = json.dumps(result)
        for text in FIXTURE_FILES.values():
            self.assertNotIn(text.strip().splitlines()[0], blob)


# ---------------------------------------------------------------------------
# Fixture 1 - a stale recorded SECURITY revision
# ---------------------------------------------------------------------------

#: The real defect: the packaging script's hardcoded security SHA had drifted
#: from the candidate's runtime trees by 5241 diff lines, so its drift gate
#: exited 1.  Once a package has been cut anyway, the recorded baseline is what
#: has to catch it: the security revision the baseline records must be the one
#: the shipped package was actually cut from.
STALE_SEC = "1111111111111111111111111111111111111111"
SHIPPED_SEC = "2222222222222222222222222222222222222222"


class TestStaleSecurityRevisionBaseline(FixtureCase):
    """A package cut from a different security revision than the baseline records."""

    def release_security_only(self, sec, label="security-only"):
        """A package whose ONLY revision record is its security revision.

        The archive/manifest name carries no ``sec-.../prep-...`` pair and the
        text manifest records no "Prep / Docs SHA" field, so the security
        revision is the single revision the artefact and its record describe -
        which is the field the packaging script's own drift gate is built
        around.
        """
        rel = self.release(sec=sec, prep=PREP_A)
        name = "homeguardian-ai-1.0.0-{}".format(label)
        (rel.outdir / "{}-manifest.txt".format(rel.name)).rename(
            rel.outdir / "{}-manifest.txt".format(name))
        rel.archive.rename(rel.outdir / "{}-source.tar.gz".format(name))
        rel.archive = rel.outdir / "{}-source.tar.gz".format(name)
        rel.name = name
        manifest = rel.outdir / "{}-manifest.txt".format(name)
        manifest.write_text(
            "\n".join(line for line in
                      manifest.read_text(encoding="utf-8").splitlines()
                      if not line.startswith("Prep / Docs SHA:")) + "\n",
            encoding="utf-8")
        return rel

    def test_a_stale_recorded_security_revision_is_fail_and_names_both(self):
        recorded = self.release_security_only(STALE_SEC)
        baseline_path, baseline = self.baseline_path(recorded.archive)
        self.assertEqual(baseline["source"]["security_baseline"], STALE_SEC)
        self.assertEqual(baseline["source"]["revision"], STALE_SEC,
                         "the security revision must be the revision this "
                         "baseline records")

        shipped = self.release_security_only(SHIPPED_SEC)
        result = crm.verify_manifest(str(shipped.archive), baseline_path)
        revision = self.findings_for(result, "HG-MF-004")
        self.assertEqual(len(revision), 1, self.rules(result))
        self.assertEqual(revision[0]["severity"], "FAIL")
        # Both sides are named: the expected (recorded) and the found (shipped).
        self.assertIn(STALE_SEC[:12], revision[0]["message"])
        self.assertIn(SHIPPED_SEC[:12], revision[0]["message"])
        self.assertEqual(result["source"]["recorded_revision"], STALE_SEC)
        self.assertEqual(result["source"]["actual_revision"], SHIPPED_SEC)
        self.assertEqual(result["source"]["recorded_security"], STALE_SEC)
        self.assertEqual(result["source"]["actual_security"], SHIPPED_SEC)
        self.assertEqual(result["checks"]["revision"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_the_same_security_revision_passes(self):
        """Control: the FAIL above is the drift, not the fixture's shape."""
        rel = self.release_security_only(STALE_SEC)
        baseline_path, _ = self.baseline_path(rel.archive)
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertNotIn("HG-MF-004", self.rules(result))
        self.assertEqual(result["counts"], {"FAIL": 0, "REVIEW": 0})
        self.assertEqual(result["checks"]["revision"], "PASS")
        self.assertEqual(result["source"]["recorded_security"], STALE_SEC)
        self.assertEqual(result["source"]["actual_security"], STALE_SEC)


# ---------------------------------------------------------------------------
# Fixture 2 - a correct baseline passes, with nothing switched off
# ---------------------------------------------------------------------------


class TestCorrectBaselinePassesWithoutLoosening(FixtureCase):
    def test_a_correct_baseline_passes_with_every_check_still_on(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)

        # The documented defaults - no flag, no switch, no override.
        result = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(result["counts"], {"FAIL": 0, "REVIEW": 0})
        self.assertEqual(result["total"], 0)
        self.assertEqual(result["checks"]["files"], "PASS")
        self.assertEqual(result["checks"]["digests"], "PASS")
        self.assertEqual(result["checks"]["revision"], "PASS")
        self.assertEqual(result["manifest_format"], "json")
        self.assertEqual(result["checks"]["text_mode_limits"], "none",
                         "per-file checks must not have been skipped")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_OK)

        # The baseline drift check RAN, with non-zero comparison counts: every
        # recorded file was present and was hashed against the record.
        summary = result["summary"]
        self.assertGreater(summary["recorded_files"], 0)
        self.assertEqual(summary["recorded_files"], len(FIXTURE_FILES))
        self.assertEqual(len(baseline["files"]), summary["recorded_files"])
        self.assertEqual(summary["present_files"], summary["recorded_files"])
        self.assertEqual(summary["missing"], 0)
        self.assertEqual(summary["unexpected"], 0)
        self.assertEqual(summary["changed"], 0)

        # A revision "PASS" is only ever produced AFTER a successful comparison
        # of two non-empty operands (a comparison that could not be made reads
        # REVIEW, and one that disagreed reads FAIL).
        self.assertTrue(result["source"]["recorded_revision"])
        self.assertTrue(result["source"]["actual_revision"])
        self.assertEqual(result["source"]["recorded_revision"],
                         result["source"]["actual_revision"])
        # The same holds for the archive digests.
        self.assertTrue(result["archive"]["recorded_sha256"])
        self.assertEqual(result["archive"]["recorded_sha256"],
                         result["archive"]["actual_sha256"])
        self.assertTrue(result["archive"]["recorded_sha512"])
        self.assertEqual(result["archive"]["recorded_sha512"],
                         result["archive"]["actual_sha512"])
        self.assertTrue(baseline["archive"]["sha256"])

        # Nothing was loosened to reach that PASS: the defaults of the two
        # switches that can widen the check are the strict ones.
        import inspect

        parameters = inspect.signature(crm.verify_manifest).parameters
        self.assertIs(parameters["check_archive_digests"].default, True)
        self.assertEqual(parameters["unexpected_severity"].default, "REVIEW")
        self.assertNotIn("severity-min", result.get("checks", {}))

    def test_one_wrong_input_flips_the_same_call_to_fail(self):
        """Falsification: the PASS above is earned by the inputs, not by a default."""
        rel = self.release(prep=PREP_A)
        baseline_path, _ = self.baseline_path(rel.archive)
        clean = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(clean["checks"]["revision"], "PASS")

        # Same call, same defaults, same fixture - only the revision differs.
        other = self.release(prep=PREP_B)
        drifted = crm.verify_manifest(str(other.archive), baseline_path)
        self.assertIn("HG-MF-004", self.rules(drifted))
        self.assertEqual(self.findings_for(drifted, "HG-MF-004")[0]["severity"],
                         "FAIL")
        self.assertEqual(drifted["checks"]["revision"], "FAIL")
        self.assertEqual(crm.exit_code_for(drifted), crm.EXIT_FAIL)


# ---------------------------------------------------------------------------
# Fixture 4 - a deliberate removal resolved by re-recording the baseline
# ---------------------------------------------------------------------------

REMOVED_ON_PURPOSE = "src/core/registry.cpp"


class TestReviewedManifestRerecord(FixtureCase):
    def test_a_deliberate_removal_is_resolved_by_re_recording_the_baseline(self):
        original = self.release()
        old_baseline_path, _ = self.baseline_path(original.archive)

        # The file was deliberately deleted from the candidate.
        reduced = dict(FIXTURE_FILES)
        del reduced[REMOVED_ON_PURPOSE]
        candidate = self.release(files=reduced)

        # (a) it FAILs against the old baseline
        stale = crm.verify_manifest(str(candidate.archive), old_baseline_path)
        missing = self.findings_for(stale, "HG-MF-001")
        self.assertEqual([f["path"] for f in missing], [REMOVED_ON_PURPOSE])
        self.assertEqual(missing[0]["severity"], "FAIL")
        self.assertEqual(stale["summary"]["missing"], 1)
        self.assertEqual(stale["checks"]["files"], "FAIL")
        self.assertEqual(crm.exit_code_for(stale), crm.EXIT_FAIL)

        # (b) the supported --record path re-records the baseline (a reviewed,
        # committed change), rather than the check being weakened.
        new_baseline_path = self.tmp / "rerecorded.json"
        code, _out, err = run_cli(str(candidate.archive), "--record", "-o",
                                  str(new_baseline_path))
        self.assertEqual(code, crm.EXIT_OK)
        self.assertEqual(err, "")
        recorded = json.loads(new_baseline_path.read_text(encoding="utf-8"))
        self.assertEqual([entry["path"] for entry in recorded["files"]],
                         sorted(reduced))
        self.assertEqual(len(recorded["files"]), len(reduced))
        for entry in recorded["files"]:
            self.assertEqual(len(entry["sha256"]), 64,
                             "the re-record must still hash every file")
        self.assertNotIn(REMOVED_ON_PURPOSE,
                         [entry["path"] for entry in recorded["files"]])

        # (c) it PASSes against the new baseline
        rerecorded = crm.verify_manifest(str(candidate.archive),
                                         new_baseline_path)
        self.assertEqual(rerecorded["counts"], {"FAIL": 0, "REVIEW": 0})
        self.assertEqual(rerecorded["total"], 0)
        self.assertEqual(rerecorded["checks"]["files"], "PASS")
        self.assertEqual(rerecorded["checks"]["revision"], "PASS")
        self.assertEqual(crm.exit_code_for(rerecorded), crm.EXIT_OK)
        self.assertNotIn("HG-MF-001", self.rules(rerecorded))

        # (d) the re-record did NOT blind the tool: a DIFFERENT unexpected file
        # is still caught...
        sneaked = dict(reduced)
        sneaked["docs/UNREVIEWED.md"] = "# added after the baseline was recorded\n"
        arrived = self.release(files=sneaked)
        after = crm.verify_manifest(str(arrived.archive), new_baseline_path)
        unexpected = self.findings_for(after, "HG-MF-002")
        self.assertEqual([f["path"] for f in unexpected], ["docs/UNREVIEWED.md"])
        self.assertEqual(crm.exit_code_for(after), crm.EXIT_FAIL)

        # ... and so is an edited file at the same size.
        edited = dict(reduced)
        body = FIXTURE_FILES["src/main.cpp"]
        edited["src/main.cpp"] = body.replace("return 0", "return 1")
        self.assertEqual(len(edited["src/main.cpp"]), len(body))
        changed_package = self.release(files=edited)
        changed = crm.verify_manifest(str(changed_package.archive),
                                      new_baseline_path)
        self.assertEqual(
            [f["path"] for f in self.findings_for(changed, "HG-MF-003")],
            ["src/main.cpp"])
        self.assertNotIn("HG-MF-001", self.rules(changed))
        self.assertEqual(crm.exit_code_for(changed), crm.EXIT_FAIL)


# ---------------------------------------------------------------------------
# Fixture 7 - a wrong revision and corrupted / altered / short manifests
# ---------------------------------------------------------------------------


class TestWrongRevisionAndCorruptedManifests(FixtureCase):
    def test_a_mismatched_recorded_revision_is_fail(self):
        rel = self.release(prep=PREP_A)
        baseline_path, baseline = self.baseline_path(rel.archive)
        baseline["source"]["revision"] = PREP_B
        baseline["source"]["prep"] = PREP_B
        baseline_path.write_text(json.dumps(baseline, indent=2), encoding="utf-8")

        result = crm.verify_manifest(str(rel.archive), baseline_path)
        revision = self.findings_for(result, "HG-MF-004")
        self.assertEqual(len(revision), 1, self.rules(result))
        self.assertEqual(revision[0]["severity"], "FAIL")
        self.assertIn(PREP_B[:12], revision[0]["message"])
        self.assertIn(PREP_A[:12], revision[0]["message"])
        self.assertEqual(result["source"]["recorded_revision"], PREP_B)
        self.assertEqual(result["source"]["actual_revision"], PREP_A)
        self.assertEqual(result["checks"]["revision"], "FAIL")
        # Non-vacuous: the revision is the ONLY thing wrong with this package.
        self.assertNotIn("HG-MF-001", self.rules(result))
        self.assertNotIn("HG-MF-002", self.rules(result))
        self.assertNotIn("HG-MF-003", self.rules(result))
        self.assertNotIn("HG-MF-006", self.rules(result))
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_a_truncated_manifest_is_an_error_not_a_pass(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)
        text = baseline_path.read_text(encoding="utf-8")
        baseline_path.write_text(text[: len(text) // 2], encoding="utf-8")
        with self.assertRaises(crm.ManifestInputError):
            crm.verify_manifest(str(rel.archive), baseline_path)
        code, _out, err = run_cli(str(rel.archive), "--manifest",
                                  str(baseline_path))
        self.assertEqual(code, crm.EXIT_INTERNAL)
        self.assertIn("error:", err)

    def test_a_manifest_that_lost_its_file_list_is_an_error(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        del baseline["files"]
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
        with self.assertRaises(crm.ManifestInputError):
            crm.verify_manifest(str(rel.archive), baseline_path)

    def test_a_recorded_digest_truncated_by_corruption_is_fail(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        entry = next(item for item in baseline["files"]
                     if item["path"] == "src/main.cpp")
        full = entry["sha256"]
        entry["sha256"] = full[:12]          # a corrupted/truncated record
        baseline_path.write_text(json.dumps(baseline, indent=2),
                                 encoding="utf-8")

        result = crm.verify_manifest(str(rel.archive), baseline_path)
        changed = self.findings_for(result, "HG-MF-003")
        self.assertEqual([f["path"] for f in changed], ["src/main.cpp"])
        self.assertEqual(changed[0]["severity"], "FAIL")
        self.assertEqual(changed[0]["recorded_sha256"], full[:12])
        self.assertEqual(changed[0]["actual_sha256"], full)
        self.assertEqual(result["checks"]["files"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_an_altered_recorded_archive_digest_is_fail(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        truth = baseline["archive"]["sha256"]
        baseline["archive"]["sha256"] = "0" * 64
        baseline_path.write_text(json.dumps(baseline, indent=2),
                                 encoding="utf-8")

        result = crm.verify_manifest(str(rel.archive), baseline_path)
        digests = self.findings_for(result, "HG-MF-006")
        self.assertEqual(len(digests), 1, self.rules(result))
        self.assertEqual(digests[0]["severity"], "FAIL")
        self.assertEqual(digests[0]["recorded_sha256"], "0" * 64)
        self.assertEqual(digests[0]["actual_sha256"], truth)
        self.assertIn(truth[:16], digests[0]["evidence"]
                      if "evidence" in digests[0] else digests[0]["message"])
        self.assertEqual(result["checks"]["digests"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)

    def test_a_manifest_missing_an_entry_the_package_carries_is_fail(self):
        rel = self.release()
        baseline_path, baseline = self.baseline_path(rel.archive)
        dropped = baseline["files"].pop(0)     # a file the package DOES carry
        baseline["package"]["file_count"] = len(baseline["files"])
        baseline_path.write_text(json.dumps(baseline, indent=2),
                                 encoding="utf-8")

        default = crm.verify_manifest(str(rel.archive), baseline_path)
        self.assertEqual(self.findings_for(default, "HG-MF-002")[0]["severity"],
                         "REVIEW")
        # The supported severity switch raises it to FAIL for a release gate.
        gate = crm.verify_manifest(str(rel.archive), baseline_path,
                                  unexpected_severity="FAIL")
        added = self.findings_for(gate, "HG-MF-002")
        self.assertEqual([f["path"] for f in added], [dropped["path"]])
        self.assertEqual(added[0]["severity"], "FAIL")
        self.assertEqual(added[0]["actual_sha256"], dropped["sha256"])
        self.assertEqual(crm.exit_code_for(gate), crm.EXIT_FAIL)

    def test_a_same_size_content_edit_is_caught_by_the_sha256(self):
        rel = self.release()
        baseline_path, _ = self.baseline_path(rel.archive)

        body = FIXTURE_FILES["src/main.cpp"]
        edited_body = body.replace("return 0", "return 1")
        self.assertEqual(
            len(edited_body), len(body),
            "the fixture must keep the size identical, so a size-only "
            "comparison would miss this edit")
        edited = dict(FIXTURE_FILES)
        edited["src/main.cpp"] = edited_body
        mutated = self.release(files=edited)

        result = crm.verify_manifest(str(mutated.archive), baseline_path)
        changed = self.findings_for(result, "HG-MF-003")
        self.assertEqual([f["path"] for f in changed], ["src/main.cpp"])
        self.assertEqual(changed[0]["severity"], "FAIL")
        self.assertEqual(changed[0]["recorded_size"], changed[0]["actual_size"])
        self.assertEqual(changed[0]["recorded_size"], len(body))
        self.assertNotEqual(changed[0]["recorded_sha256"],
                            changed[0]["actual_sha256"])
        self.assertIn("size is unchanged", changed[0]["message"])
        self.assertNotIn("HG-MF-001", self.rules(result))
        self.assertNotIn("HG-MF-002", self.rules(result))
        self.assertEqual(result["checks"]["files"], "FAIL")
        self.assertEqual(crm.exit_code_for(result), crm.EXIT_FAIL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
