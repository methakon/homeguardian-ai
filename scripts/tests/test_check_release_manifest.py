#!/usr/bin/env python3
"""Unit tests for scripts/check_release_manifest.py.

Fixtures are built under ``tempfile.mkdtemp()``; the suite never reads the real
repository, the real release output or the network.  Standard library only.

Run:
    python3 scripts/tests/test_check_release_manifest.py -v
    python3 -m unittest discover -s scripts/tests -t scripts -v
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
