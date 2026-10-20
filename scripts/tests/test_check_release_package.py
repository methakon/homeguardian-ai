#!/usr/bin/env python3
"""Unit tests for scripts/check_release_package.py.

Every fixture is built under ``tempfile.mkdtemp()``: the suite never reads the
real repository, the real release output or the network, so it is deterministic
and safe to run anywhere.  Only the standard library is used.

Run:
    python3 scripts/tests/test_check_release_package.py -v
    python3 -m unittest discover -s scripts/tests -t scripts -v

The eight mutation fixtures the release-package audit is required to cover are:

    1. complete valid package            TestCompleteValidPackage
    2. missing LICENSE or NOTICE         TestMissingLicenceOrNotice
    3. missing runtime dep attribution   TestDependencyAttributionCoverage
    4. licence mismatch                  TestManifestLicencePolicy
    5. unpinned dependency               TestUnpinnedDependency
    6. missing source revision           TestSourceRevisionRecorded
    7. secret-bearing file               TestSecretsAndBuildOutput
    8. unexpected build artifacts        TestSecretsAndBuildOutput

plus the secret-redaction proof in :class:`TestSecretRedaction`, which asserts a
planted credential never reaches stdout, stderr or the JSON report, and that the
internal guard aborts (exit 3) rather than emit a matched value.

The regression fixtures added for the release-QA defects are:

    9.  a mandatory file the candidate dropped        TestMandatoryFilesRegression
    10. runtime attribution omissions (42 packages,
        4 nested components, an unnamed MPL component)
                                                     TestAttributionOmissionsRegression
    11. a reviewed allow-list for synthetic credential
        fixtures in TEST files - which downgrades a
        named finding to REVIEW and can never hide a
        different value                           TestSyntheticSecretAllowlist
"""

from __future__ import annotations

import base64
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))

import check_release_package as crp  # noqa: E402
import check_submission_claims as csc  # noqa: E402

CLI = SCRIPTS / "check_release_package.py"

#: A well-formed but deliberately non-existent commit id.  Nothing in the fixture
#: set asserts that it exists in a repository; check 5 only reports a REVIEW for
#: an unresolvable revision when a repository was named explicitly.
FULL_SHA = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f80912"
OTHER_SHA = "0f1e2d3c4b5a69788796a5b4c3d2e1f00918a7b6"

#: The shape scripts/package_release.sh produces.
PACKAGE_NAME = "homeguardian-ai-1.0.0-sec-05e130395b-prep-5d8cc3b391"

APACHE_LICENSE = """\
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
"""

README = """\
# Fixture Project

A fixture README used by the release-package audit tests. It describes a local
demo and makes no claims about anything that is not implemented.

## Licence

This fixture is licensed under the Apache License, Version 2.0.
"""

NOTICE = """\
HomeGuardian AI

This product includes software developed by third parties. The bundled
components and the licences they are used under are listed below.

Bundled third-party components:

 - leftpad 1.0.0 -- MIT License. https://github.com/leftpad/leftpad
 - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib

The project itself is licensed under the Apache License, Version 2.0.
"""

PACKAGE_JSON = """\
{
  "name": "homeguardian-web-mcp",
  "version": "1.0.0",
  "license": "Apache-2.0",
  "private": true,
  "dependencies": {
    "leftpad": "^1.0.0",
    "tslib": "^2.6.2"
  }
}
"""

LOCKFILE = {
    "name": "homeguardian-web-mcp",
    "version": "1.0.0",
    "lockfileVersion": 3,
    "packages": {
        "": {
            "name": "homeguardian-web-mcp",
            "version": "1.0.0",
            "license": "Apache-2.0",
            "dependencies": {"leftpad": "^1.0.0", "tslib": "^2.6.2"},
        },
        "node_modules/leftpad": {
            "version": "1.0.0",
            "license": "MIT",
            "resolved": "https://registry.npmjs.org/leftpad/-/leftpad-1.0.0.tgz",
            "integrity": "sha512-fixture-leftpad-integrity",
        },
        "node_modules/tslib": {
            "version": "2.6.2",
            "license": "0BSD",
            "resolved": "https://registry.npmjs.org/tslib/-/tslib-2.6.2.tgz",
            "integrity": "sha512-fixture-tslib-integrity",
        },
    },
}

DEFAULT_MANDATORY = (
    "README.md",
    "LICENSE",
    "NOTICE",
    "src/main.cpp",
    "package.json",
    "package-lock.json",
    "deploy/coordinator/service.js",
    "scripts/verify_judge_release.sh",
)

#: Fixture stand-in for scripts/package_release.sh.  The mandatory list and the
#: leak-check token list are parsed out of it verbatim, exactly as the checker
#: parses the real script.  ``{mandatory}`` is one quoted entry per line.
PACKAGE_SCRIPT_TEMPLATE = """\
#!/usr/bin/env bash
# Fixture stand-in for scripts/package_release.sh.
set -euo pipefail

MANDATORY_FILES=(
{mandatory}
)

echo "[Step 4] Checking for forbidden artifacts in package..."
FORBIDDEN=$(find . -name ".git" -o -name "*.db" -o -name "*.log" -o -name "node_modules" -o -name "*.secret" -o -name "id_rsa*" -o -name "*.key")
MANIFEST="$PACKAGE_NAME-manifest.txt"
echo "$FORBIDDEN" > /dev/null
"""

MANIFEST_TEMPLATE = """\
HomeGuardian AI release manifest (fixture)
Package: {name}
Version: 1.0.0
License: {licence}
Source SHA: {sha}
Files: {files}
"""

#: The fixture stand-in's forbidden list must equal the checker's copy, or the
#: drift rule HG-PKG-LEAK-006 fires.  Asserted once so a future edit to either
#: list is caught here rather than in a release.
EXPECTED_FORBIDDEN = (".git", "*.db", "*.log", "node_modules", "*.secret",
                      "id_rsa*", "*.key")


def base_files(lockfile=None, notice=None, manifest_extra=None, extra=None,
               drop=()):
    """The complete, valid fixture package's files, before mutation."""
    files = {
        "README.md": README,
        "LICENSE": APACHE_LICENSE,
        "NOTICE": NOTICE,
        "src/main.cpp": "int main() { return 0; }\n",
        "deploy/coordinator/service.js":
            "// outbound bridge coordinator (fixture)\nmodule.exports = {};\n",
        "scripts/verify_judge_release.sh": "#!/usr/bin/env bash\necho ok\n",
        "package.json": PACKAGE_JSON,
        "package-lock.json":
            json.dumps(lockfile if lockfile is not None else LOCKFILE,
                       indent=2) + "\n",
    }
    if notice is not None:
        files["NOTICE"] = notice
    if manifest_extra:
        files.update(manifest_extra)
    if extra:
        files.update(extra)
    for rel in drop:
        files.pop(rel, None)
    return files


def materialise(root, files):
    """Write ``files`` (rel -> text) under ``root``; a None value is omitted."""
    for rel, text in files.items():
        if text is None:
            continue
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def run_cli(*args):
    proc = subprocess.run([sys.executable, str(CLI), *args],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def rules_of(result):
    return [f["rule"] for f in result["findings"]]


def findings_for(result, rule):
    return [f for f in result["findings"] if f["rule"] == rule]


def status_of(result, number):
    return result["checks"][str(number)]["status"]


class FixtureCase(unittest.TestCase):
    """Base class: temp dirs, fixture packages and scripted rule sources."""

    maxDiff = None

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="hgpkg-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    # -- fixture construction ------------------------------------------------

    def packaging_script(self, mandatory=DEFAULT_MANDATORY, name="pkgrel.sh"):
        script = self.tmp / name
        body = "\n".join('    "{}"'.format(entry) for entry in mandatory)
        script.write_text(PACKAGE_SCRIPT_TEMPLATE.format(mandatory=body),
                          encoding="utf-8")
        return script

    def release(self, files=None, *, mandatory=DEFAULT_MANDATORY, licence="Apache-2.0",
                sha=FULL_SHA, manifest=True, checksums=True, archive=True,
                lockfile=None, name=PACKAGE_NAME, extra_files=None, drop=(),
                script=None, notice=None):
        """Build a release output directory and return a namespace describing it.

        Returns an object with ``outdir`` (the packaging output directory),
        ``archive`` (the tarball, or None), ``tree`` (the packaged tree, for
        direct reference), ``script`` (the fixture packaging script) and
        ``name``.
        """
        outdir = Path(tempfile.mkdtemp(prefix="out-", dir=str(self.tmp)))
        tree = outdir / "tree"
        tree.mkdir(parents=True, exist_ok=True)
        payload = (base_files(lockfile=lockfile) if files is None else dict(files))
        if extra_files:
            payload.update(extra_files)
        if notice is not None:
            payload["NOTICE"] = notice
        for rel in drop:
            payload.pop(rel, None)
        materialise(tree, payload)

        script = script or self.packaging_script(mandatory=mandatory)

        archive_path = None
        if archive:
            archive_path = outdir / "{}-source.tar.gz".format(name)
            with tarfile.open(archive_path, "w:gz") as handle:
                handle.add(tree, arcname=name)

        if manifest:
            text = MANIFEST_TEMPLATE.format(
                name=name, licence=licence, sha=sha, files=len(payload),
            )
            (outdir / "{}-manifest.txt".format(name)).write_text(
                text, encoding="utf-8")

        if checksums and archive_path is not None:
            import hashlib

            for algo in ("sha256", "sha512"):
                digest = hashlib.new(algo)
                with archive_path.open("rb") as handle:
                    for chunk in iter(lambda: handle.read(1 << 20), b""):
                        digest.update(chunk)
                (outdir / (archive_path.name + "." + algo)).write_text(
                    "{}  {}\n".format(digest.hexdigest(), archive_path.name),
                    encoding="utf-8")

        return types.SimpleNamespace(
            outdir=outdir, archive=archive_path, tree=tree, script=script,
            name=name,
        )

    # -- assertions ----------------------------------------------------------

    def audit(self, target, *, script=None, **kwargs):
        kwargs.setdefault("package_script", script)
        return crp.audit_package(str(target), **kwargs)

    def assertClean(self, result):
        self.assertEqual(
            result["counts"], {"FAIL": 0, "REVIEW": 0},
            "expected a clean audit, got: {}".format(
                json.dumps([{k: f[k] for k in ("rule", "severity", "file", "message")}
                            for f in result["findings"]], indent=2)),
        )


# ---------------------------------------------------------------------------
# Fixture 1 - a complete, valid package
# ---------------------------------------------------------------------------


class TestCompleteValidPackage(FixtureCase):
    def test_directory_input_is_clean(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        self.assertClean(result)
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_OK)
        self.assertEqual(result["origin"], "tarball")
        self.assertEqual(result["prefix"], rel.name)
        self.assertEqual(result["expected_licence"], "Apache-2.0")
        for number in range(1, 7):
            self.assertEqual(status_of(result, number), "PASS",
                             "check {} not PASS: {}".format(number, result["checks"]))

    def test_tarball_input_matches_directory_input(self):
        rel = self.release()
        from_dir = self.audit(rel.outdir, script=rel.script)
        from_tar = self.audit(rel.archive, script=rel.script)
        self.assertClean(from_tar)
        self.assertEqual(from_tar["origin"], "tarball")
        self.assertEqual(
            [f["rule"] for f in from_dir["findings"]],
            [f["rule"] for f in from_tar["findings"]],
        )
        self.assertEqual(from_dir["counts"], from_tar["counts"])

    def test_extracted_tree_is_a_directory_input(self):
        """A bare extracted tree is auditable; the absent manifest is REVIEW.

        The tree carries no manifest and no revision file, so check 5 is FAIL
        (there is genuinely no revision record inside it) while check 3 is only
        REVIEW - a package with no manifest beside it cannot have its licence
        claim compared, and saying FAIL there would be a false positive.
        """
        rel = self.release()
        result = self.audit(rel.tree, script=rel.script)
        self.assertEqual(result["origin"], "directory")
        self.assertIsNone(result["prefix"])
        manifest_findings = findings_for(result, "HG-PKG-LIC-021")
        self.assertEqual(len(manifest_findings), 1, rules_of(result))
        self.assertEqual(manifest_findings[0]["severity"], "REVIEW")
        self.assertEqual(status_of(result, 3), "REVIEW")
        self.assertEqual(status_of(result, 5), "FAIL")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)

    def test_mandatory_list_is_read_from_the_packaging_script(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        self.assertEqual(result["packaging_rules"]["mandatory_count"],
                         len(DEFAULT_MANDATORY))
        self.assertEqual(result["packaging_rules"]["source"], str(rel.script))
        self.assertNotIn("HG-PKG-LEAK-006", rules_of(result),
                         "the fixture script's leak list must match the checker's "
                         "copy, or the drift rule is being tested by accident")
        self.assertEqual(result["packaging_rules"]["forbidden"],
                         list(EXPECTED_FORBIDDEN))

    def test_fallback_rules_are_reported_when_no_script_is_reachable(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=self.tmp / "does-not-exist.sh")
        self.assertEqual(result["packaging_rules"]["source"],
                         "<fallback constants in check_release_package.py>")
        self.assertTrue(any("fallback copies" in n for n in result["notes"]))

    def test_packaging_rule_drift_is_reported(self):
        rel = self.release()
        drifted = self.packaging_script(name="drifted.sh")
        text = drifted.read_text(encoding="utf-8")
        drifted.write_text(text.replace('-name "*.log"', '-name "*.tmp"'),
                           encoding="utf-8")
        result = self.audit(rel.outdir, script=drifted)
        self.assertIn("HG-PKG-LEAK-006", rules_of(result))


# ---------------------------------------------------------------------------
# Fixture 2 - missing LICENSE or NOTICE
# ---------------------------------------------------------------------------


class TestMissingLicenceOrNotice(FixtureCase):
    def test_missing_license_is_fail(self):
        rel = self.release(drop=("LICENSE",))
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LIC-001", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LIC-001")[0]["severity"],
                         "FAIL")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)

    def test_missing_notice_is_fail(self):
        rel = self.release(drop=("NOTICE",))
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LIC-002", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LIC-002")[0]["severity"],
                         "FAIL")

    def test_missing_readme_is_fail_through_the_mandatory_list(self):
        rel = self.release(drop=("README.md",))
        result = self.audit(rel.outdir, script=rel.script)
        src = findings_for(result, "HG-PKG-SRC-001")
        self.assertEqual([f["file"] for f in src], ["README.md"])
        self.assertEqual(src[0]["severity"], "FAIL")

    def test_notice_without_attribution_content_is_review(self):
        rel = self.release(notice="HomeGuardian AI\n\nLicensed under the "
                                  "Apache License, Version 2.0.\n")
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LIC-004", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LIC-004")[0]["severity"],
                         "REVIEW")


# ---------------------------------------------------------------------------
# Fixture 3 - missing runtime dependency attribution (incl. MPL mediabunny)
# ---------------------------------------------------------------------------


class TestDependencyAttributionCoverage(FixtureCase):
    def test_complete_package_attributes_every_component(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        stats = result["stats"]["dependencies"]
        self.assertEqual(stats["npm_components"], 2)
        self.assertEqual(stats["attribution_files"], ["NOTICE"])
        self.assertEqual(stats["covered"], ["leftpad@1.0.0", "tslib@2.6.2"])
        self.assertEqual(stats["uncovered_by_licence"], {})

    def test_unattributed_runtime_dependency_is_fail_and_aggregated(self):
        notice = NOTICE.replace(
            " - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib\n",
            "")
        rel = self.release(notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        group = findings_for(result, "HG-PKG-DEP-002")
        self.assertEqual(len(group), 1, group)
        self.assertEqual(group[0]["severity"], "FAIL")
        self.assertIn("0BSD", group[0]["message"])
        self.assertIn("tslib@2.6.2", group[0]["message"])
        self.assertNotIn("leftpad", group[0]["message"])
        self.assertEqual(result["stats"]["dependencies"]["uncovered_by_licence"],
                         {"0BSD": 1})

    def test_mediabunny_mpl_is_fail_by_name_not_by_licence_summary(self):
        """MPL-2.0 must be named; a generic transitive-coverage pass is not enough."""
        lockfile = json.loads(json.dumps(LOCKFILE))
        lockfile["packages"]["node_modules/mediabunny"] = {
            "version": "1.61.5",
            "license": "MPL-2.0",
            "resolved": "https://registry.npmjs.org/mediabunny/-/mediabunny-1.61.5.tgz",
            "integrity": "sha512-fixture-mediabunny-integrity",
        }
        # Even with an explicit licence-level MPL statement in an npm-scoped
        # context, the component must be identified by name.
        notice = NOTICE.replace(
            "Bundled third-party components:",
            "Web gateway runtime dependencies (npm) are bundled under MPL-2.0, "
            "MIT, 0BSD and Apache-2.0 as applicable.\n\n"
            "Bundled third-party components:")
        rel = self.release(lockfile=lockfile, notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        named = findings_for(result, "HG-PKG-DEP-001")
        self.assertEqual(len(named), 1, [f["message"] for f in result["findings"]])
        self.assertEqual(named[0]["severity"], "FAIL")
        self.assertIn("mediabunny", named[0]["message"])
        self.assertIn("1.61.5", named[0]["message"])
        self.assertIn("MPL-2.0", named[0]["message"])
        self.assertIn("identified", named[0]["message"])
        self.assertEqual(result["stats"]["dependencies"]["per_package_required_missing"],
                         ["mediabunny"])

    def test_naming_mediabunny_clears_the_mpl_finding(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        lockfile["packages"]["node_modules/mediabunny"] = {
            "version": "1.61.5",
            "license": "MPL-2.0",
            "resolved": "https://registry.npmjs.org/mediabunny/-/mediabunny-1.61.5.tgz",
            "integrity": "sha512-fixture-mediabunny-integrity",
        }
        notice = NOTICE.replace(
            " - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib",
            " - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib\n"
            " - mediabunny 1.61.5 -- MPL-2.0 License. "
            "https://github.com/Vanilagy/mediabunny")
        rel = self.release(lockfile=lockfile, notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertNotIn("HG-PKG-DEP-001", rules_of(result))
        self.assertIn("mediabunny@1.61.5",
                      result["stats"]["dependencies"]["covered"])

    def test_same_package_at_two_versions_is_labelled_apart(self):
        """Regression: an aggregate must never read 'tslib, tslib'."""
        lockfile = json.loads(json.dumps(LOCKFILE))
        lockfile["packages"]["node_modules/nested/node_modules/tslib"] = {
            "version": "1.14.1",
            "license": "0BSD",
            "resolved": "https://registry.npmjs.org/tslib/-/tslib-1.14.1.tgz",
            "integrity": "sha512-fixture-tslib-1-integrity",
        }
        notice = NOTICE.replace(
            " - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib\n", "")
        rel = self.release(lockfile=lockfile, notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        group = findings_for(result, "HG-PKG-DEP-002")
        self.assertEqual(len(group), 1, group)
        message = group[0]["message"]
        self.assertIn("tslib@1.14.1", message)
        self.assertIn("tslib@2.6.2", message)
        self.assertNotIn("tslib, tslib", message)
        self.assertEqual(result["stats"]["dependencies"]["uncovered_by_licence"],
                         {"0BSD": 2})

    def test_sqlite_version_is_read_from_the_amalgamation(self):
        """SQLite defines SQLITE_VERSION in sqlite3.c, not in sqlite3.h."""
        directory = self.tmp / "third_party" / "sqlite"
        directory.mkdir(parents=True)
        (directory / "sqlite3.c").write_text(
            '/* amalgamation */\n#define SQLITE_VERSION        "3.53.4"\n'
            '#define SQLITE_VERSION_NUMBER 3053004\n', encoding="utf-8")
        self.assertEqual(crp._vendored_version(directory), "3.53.4")

    def test_vendored_and_fetched_components_are_collected(self):
        files = base_files(extra={
            "CMakeLists.txt": (
                "include(FetchContent)\n"
                "FetchContent_Declare(spdlog GIT_REPOSITORY "
                "https://github.com/gabime/spdlog.git GIT_TAG v1.13.0)\n"
                "FetchContent_Declare(nlohmann_json GIT_REPOSITORY "
                "https://github.com/nlohmann/json.git GIT_TAG v3.11.3)\n"
            ),
            "third_party/sqlite/sqlite3.c":
                '#define SQLITE_VERSION        "3.53.4"\n',
            "third_party/catch2/single_include/catch2/catch.hpp":
                "#define CATCH_VERSION_MAJOR 2\n#define CATCH_VERSION_MINOR 13\n"
                "#define CATCH_VERSION_PATCH 10\n",
        })
        notice = NOTICE + (
            "\nVendored and build-time components:\n"
            " - spdlog 1.13.0 -- MIT License\n"
            " - nlohmann_json 3.11.3 -- MIT License\n"
            " - sqlite 3.53.4 -- public domain\n"
            " - catch2 2.13.10 -- BSL-1.0 License\n"
        )
        rel = self.release(files=files, notice=notice,
                           mandatory=tuple(DEFAULT_MANDATORY) + ("CMakeLists.txt",))
        result = self.audit(rel.outdir, script=rel.script)
        covered = result["stats"]["dependencies"]["covered"]
        self.assertIn("spdlog@1.13.0", covered)
        self.assertIn("nlohmann_json@3.11.3", covered)
        self.assertIn("sqlite@3.53.4", covered)
        self.assertIn("catch2@2.13.10", covered)
        self.assertNotIn("HG-PKG-DEP-001", rules_of(result))

    def test_licence_level_statement_scoped_to_the_ecosystem_is_review(self):
        notice = NOTICE.replace(
            " - tslib 2.6.2 -- 0BSD License. https://github.com/microsoft/tslib\n",
            "").replace(
            "Bundled third-party components:",
            "Bundled third-party components:\n"
            "The npm runtime dependencies of the web gateway are bundled under "
            "0BSD, MIT and Apache-2.0 as applicable.")
        rel = self.release(notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        collective = findings_for(result, "HG-PKG-DEP-003")
        self.assertTrue(collective, rules_of(result))
        self.assertEqual(collective[0]["severity"], "REVIEW")

    def test_no_attribution_record_is_review(self):
        rel = self.release(drop=("NOTICE",))
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-DEP-007", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-DEP-007")[0]["severity"],
                         "REVIEW")

    def test_lockfile_recording_no_licence_is_review(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        del lockfile["packages"]["node_modules/tslib"]["license"]
        rel = self.release(lockfile=lockfile)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-DEP-008", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-DEP-008")[0]["severity"],
                         "REVIEW")

    def test_unreadable_lockfile_is_review(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        del lockfile["packages"]
        rel = self.release(lockfile=lockfile)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-DEP-004", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-DEP-004")[0]["severity"],
                         "REVIEW")


# ---------------------------------------------------------------------------
# Fixture 4 - licence mismatch / selectable expected licence
# ---------------------------------------------------------------------------


class TestManifestLicencePolicy(FixtureCase):
    def test_manifest_licence_conflicting_with_policy_is_fail(self):
        rel = self.release(licence="MIT")
        result = self.audit(rel.outdir, script=rel.script)
        conflict = findings_for(result, "HG-PKG-LIC-020")
        self.assertEqual(len(conflict), 1, rules_of(result))
        self.assertEqual(conflict[0]["severity"], "FAIL")
        self.assertIn("MIT", conflict[0]["message"])

    def test_manifest_with_no_licence_field_is_review(self):
        rel = self.release()
        manifest = rel.outdir / "{}-manifest.txt".format(rel.name)
        manifest.write_text(
            "\n".join(line for line in manifest.read_text().splitlines()
                      if not line.startswith("License:")),
            encoding="utf-8")
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LIC-021", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LIC-021")[0]["severity"],
                         "REVIEW")

    def test_package_json_licence_conflict_is_fail(self):
        files = base_files(extra={
            "package.json": PACKAGE_JSON.replace('"Apache-2.0"', '"MIT"'),
        })
        rel = self.release(files=files)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-LIC-011", rules_of(result))
        self.assertEqual(findings_for(result, "HG-LIC-011")[0]["severity"], "FAIL")

    def test_package_json_without_licence_field_is_review(self):
        files = base_files(extra={
            "package.json": PACKAGE_JSON.replace('  "license": "Apache-2.0",\n', ""),
        })
        rel = self.release(files=files)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-LIC-010", rules_of(result))
        self.assertEqual(findings_for(result, "HG-LIC-010")[0]["severity"], "REVIEW")

    def test_expected_licence_is_selectable(self):
        mit_notice = NOTICE.replace("Apache License, Version 2.0", "MIT License")
        mit_license = ("MIT License\n\nPermission is hereby granted, free of "
                       "charge, to any person obtaining a copy\n")
        files = base_files(extra={
            "LICENSE": mit_license,
            "package.json": PACKAGE_JSON.replace('"Apache-2.0"', '"MIT"'),
        })
        rel = self.release(files=files, notice=mit_notice, licence="MIT")
        result = self.audit(rel.outdir, script=rel.script,
                            expected_licence="MIT")
        self.assertEqual(result["expected_licence"], "MIT")
        self.assertNotIn("HG-PKG-LIC-020", rules_of(result))
        self.assertNotIn("HG-LIC-011", rules_of(result))
        # The same artefact under the wrong policy is a reported conflict.
        against_apache = self.audit(rel.outdir, script=rel.script,
                                    expected_licence="Apache-2.0")
        self.assertIn("HG-PKG-LIC-020", rules_of(against_apache))

    def test_licence_text_not_matching_the_policy_fails(self):
        files = base_files(extra={"LICENSE": "MIT License\n\nPermission is "
                                            "hereby granted.\n"})
        rel = self.release(files=files)
        result = self.audit(rel.outdir, script=rel.script)
        # The licence-TEXT comparison is the submission scanner's HG-LIC-001,
        # reused rather than re-implemented (check 1 keeps it).
        self.assertIn("HG-LIC-001", rules_of(result))
        self.assertEqual(findings_for(result, "HG-LIC-001")[0]["severity"], "FAIL")
        self.assertEqual(status_of(result, 1), "FAIL")


# ---------------------------------------------------------------------------
# Fixture 5 - unpinned dependency
# ---------------------------------------------------------------------------


class TestUnpinnedDependency(FixtureCase):
    def test_missing_integrity_is_review(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        del lockfile["packages"]["node_modules/tslib"]["integrity"]
        rel = self.release(lockfile=lockfile)
        result = self.audit(rel.outdir, script=rel.script)
        unpinned = findings_for(result, "HG-PKG-DEP-004")
        self.assertTrue(unpinned, rules_of(result))
        self.assertTrue(all(f["severity"] == "REVIEW" for f in unpinned))
        self.assertTrue(any("not pinned" in f["message"] for f in unpinned))

    def test_wildcard_range_is_review(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        lockfile["packages"][""]["dependencies"]["leftpad"] = "*"
        files = base_files(lockfile=lockfile, extra={
            "package.json": PACKAGE_JSON.replace('"leftpad": "^1.0.0"',
                                                 '"leftpad": "*"'),
        })
        rel = self.release(files=files)
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-DEP-004")
        self.assertTrue(found, rules_of(result))
        self.assertTrue(any("unconstrained" in f["message"] for f in found))
        self.assertTrue(all(f["severity"] == "REVIEW" for f in found))

    def test_old_lockfile_version_is_review(self):
        lockfile = json.loads(json.dumps(LOCKFILE))
        lockfile["lockfileVersion"] = 1
        del lockfile["packages"]
        rel = self.release(lockfile=lockfile)
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-DEP-004")
        self.assertTrue(any("lockfileVersion" in f["message"] for f in found),
                        [f["message"] for f in found])
        self.assertTrue(all(f["severity"] == "REVIEW" for f in found))


# ---------------------------------------------------------------------------
# Fixture 6 - missing source revision
# ---------------------------------------------------------------------------


class TestSourceRevisionRecorded(FixtureCase):
    def test_revision_recorded_is_pass(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        self.assertEqual(status_of(result, 5), "PASS")
        self.assertEqual(result["stats"]["revision"]["full_sha_records"], [FULL_SHA])

    def test_missing_revision_is_fail(self):
        rel = self.release(manifest=False, name="homeguardian-ai-1.0.0")
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-REV-001", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-REV-001")[0]["severity"],
                         "FAIL")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)

    def test_short_revision_only_is_review(self):
        rel = self.release(manifest=False)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-REV-002", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-REV-002")[0]["severity"],
                         "REVIEW")
        self.assertNotIn("HG-PKG-REV-001", rules_of(result))

    def test_revision_file_inside_the_package_is_accepted(self):
        files = base_files(extra={"REVISION": FULL_SHA + "\n"})
        rel = self.release(files=files, manifest=False, name="homeguardian-ai-1.0.0")
        result = self.audit(rel.outdir, script=rel.script)
        self.assertNotIn("HG-PKG-REV-001", rules_of(result))
        self.assertIn(FULL_SHA, result["stats"]["revision"]["full_sha_records"])

    def test_checksum_not_matching_the_archive_is_fail(self):
        rel = self.release()
        checksum = rel.outdir / "{}.sha256".format(rel.archive.name)
        checksum.write_text("{}  {}\n".format("0" * 64, rel.archive.name),
                            encoding="utf-8")
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-REV-004", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-REV-004")[0]["severity"],
                         "FAIL")

    def test_recorded_revision_is_corroborated_when_a_repo_is_named(self):
        rel = self.release()
        empty_repo = self.tmp / "repo"
        empty_repo.mkdir()
        subprocess.run(["git", "init", "-q", str(empty_repo)], check=True)
        result = self.audit(rel.outdir, script=rel.script, repo=str(empty_repo))
        found = findings_for(result, "HG-PKG-REV-003")
        self.assertEqual(len(found), 1, rules_of(result))
        self.assertEqual(found[0]["severity"], "REVIEW")

    def test_unresolvable_revision_without_a_named_repo_is_only_a_note(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        self.assertNotIn("HG-PKG-REV-003", rules_of(result))


# ---------------------------------------------------------------------------
# Fixtures 7 and 8 - secret-bearing files, unexpected build artifacts
# ---------------------------------------------------------------------------


class TestSecretsAndBuildOutput(FixtureCase):
    def test_credential_shaped_file_is_fail(self):
        rel = self.release(extra_files={
            "deploy/docker/.env.production": "HG_HTTP_SECRET=placeholder\n",
        })
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-LEAK-001")
        self.assertEqual(len(found), 1, rules_of(result))
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertEqual(found[0]["file"], "deploy/docker/.env.production")

    def test_credential_template_file_is_review_not_fail(self):
        rel = self.release(extra_files={
            "deploy/docker/.env.example": "HG_HTTP_SECRET=<set me>\n",
        })
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LEAK-007", rules_of(result))
        self.assertNotIn("HG-PKG-LEAK-001", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LEAK-007")[0]["severity"],
                         "REVIEW")

    def test_explicitly_forbidden_item_is_fail(self):
        rel = self.release(extra_files={"state/audit.db": "not really a db\n"})
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-LEAK-002")
        self.assertEqual(len(found), 1, rules_of(result))
        self.assertEqual(found[0]["severity"], "FAIL")
        self.assertIn("*.db", found[0]["message"])

    def test_unexpected_build_artifacts_are_review(self):
        rel = self.release(extra_files={
            "src/core/registry.o": "\x7fELF fixture\n",
            "src/main.dSYM/Contents/Info.plist": "<plist/>\n",
            "logs/run.log": "line\n",
        })
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-LEAK-004")
        self.assertTrue(found, rules_of(result))
        self.assertTrue(all(f["severity"] == "REVIEW" for f in found),
                        [(f["file"], f["severity"]) for f in found])
        self.assertIn("src/core/registry.o", [f["file"] for f in found])

    def test_build_output_directory_is_reported_once_as_review(self):
        rel = self.release(extra_files={
            "build/CMakeCache.txt": "cache\n",
            "build/obj/deep/nested.o": "obj\n",
        })
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-LEAK-004")
        build_hits = [f for f in found if f["file"] == "build"]
        self.assertEqual(len(build_hits), 1,
                         "a pruned build directory is reported once, not per file")
        self.assertEqual(build_hits[0]["severity"], "REVIEW")

    def test_local_state_is_review(self):
        rel = self.release(extra_files={"src/main.cpp.orig": "backup\n"})
        result = self.audit(rel.outdir, script=rel.script)
        found = findings_for(result, "HG-PKG-LEAK-005")
        self.assertTrue(found, rules_of(result))
        self.assertEqual(found[0]["severity"], "REVIEW")

    def test_database_file_is_fail_through_the_forbidden_list(self):
        """A .db file is FAIL, not REVIEW: the packaging forbids it itself."""
        rel = self.release(extra_files={"config/homeguardian.demo.db": "sqlite\n"})
        result = self.audit(rel.outdir, script=rel.script)
        self.assertIn("HG-PKG-LEAK-002", rules_of(result))
        self.assertEqual(findings_for(result, "HG-PKG-LEAK-002")[0]["severity"],
                         "FAIL")
        self.assertNotIn("HG-PKG-LEAK-005", rules_of(result))

    def test_severity_min_filters_review_findings(self):
        rel = self.release(extra_files={"src/main.cpp.orig": "backup\n"})
        full = self.audit(rel.outdir, script=rel.script)
        only_fail = self.audit(rel.outdir, script=rel.script,
                               severity_min="FAIL")
        self.assertTrue(full["counts"]["REVIEW"])
        self.assertEqual(only_fail["counts"]["REVIEW"], 0)
        self.assertTrue(any("severity-min=FAIL" in n for n in only_fail["notes"]))


# ---------------------------------------------------------------------------
# Redaction: a planted secret must never be emitted
# ---------------------------------------------------------------------------


class TestSecretRedaction(FixtureCase):
    SECRET = base64.b64encode(b"wireguard-test-key-material-32b!").decode("ascii")

    def package_with_secret(self):
        return self.release(extra_files={
            "config/homeguardian.demo.json": json.dumps(
                {"PrivateKey": self.SECRET, "node": "fixture"}, indent=2) + "\n",
            "deploy/docker/.env.production": "HG_HTTP_SECRET={}\n".format(
                self.SECRET),
        })

    def test_planted_secret_never_reaches_stdout_stderr_or_json(self):
        rel = self.package_with_secret()
        code_text, out_text, err_text = run_cli(
            str(rel.outdir), "--package-script", str(rel.script))
        code_json, out_json, err_json = run_cli(
            str(rel.outdir), "--package-script", str(rel.script), "--json")

        for blob in (out_text, err_text, out_json, err_json):
            self.assertNotIn(self.SECRET, blob)
            self.assertNotIn(self.SECRET[:-1], blob)
            self.assertNotIn(self.SECRET[:16], blob)
        self.assertEqual(err_text, "")
        self.assertEqual(err_json, "")

        report = json.loads(out_json)
        creds = findings_for(report, "HG-PKG-LEAK-003")
        self.assertTrue(creds, report["findings"])
        for finding in creds:
            self.assertEqual(finding["value_sha256_8"], csc._sha256_8(self.SECRET))
            self.assertEqual(finding["value_length"], len(self.SECRET))
            self.assertIn(finding["pattern_class"], dict(csc.SECRET_PATTERNS))
            self.assertIn("[redacted:", finding["evidence"])
            self.assertNotIn(self.SECRET, finding["message"])
            self.assertNotIn(self.SECRET, finding["correction"])
        self.assertEqual(code_json, crp.EXIT_FAIL)
        self.assertEqual(code_text, crp.EXIT_FAIL)

    def test_json_report_is_stable_and_documented(self):
        rel = self.release()
        result = self.audit(rel.outdir, script=rel.script)
        for key in ("program", "input", "origin", "package_root", "prefix",
                    "expected_licence", "packaging_rules", "check_names",
                    "checks", "counts", "total", "findings", "notes", "stats"):
            self.assertIn(key, result)
        self.assertEqual(result["program"], "check_release_package")
        self.assertEqual(sorted(result["check_names"]), ["1", "2", "3", "4", "5", "6"])
        self.assertEqual(json.loads(json.dumps(result)), result)
        for finding in result["findings"]:
            for key in ("rule", "severity", "file", "line", "message", "evidence",
                        "correction", "check"):
                self.assertIn(key, finding)

    def test_guard_aborts_rather_than_emit_a_matched_value(self):
        """A finding that embeds a matched value must abort the run (exit 3).

        The masking step is deliberately replaced with one that *injects* the
        matched value into a finding's message, simulating the regression the
        guard exists for.  The scanner's ``enforce_redaction`` must trip, the
        CLI must exit 3, and the value must still not appear in the output.
        """
        rel = self.package_with_secret()
        argv = [str(rel.outdir), "--package-script", str(rel.script)]
        buffer = io.StringIO()
        original = crp._sanitize

        def leaky(findings, values):
            raw = [v for v in values if v]
            out = []
            for finding in findings:
                out.append(finding.__class__(
                    rule=finding.rule, severity=finding.severity,
                    file=finding.file, line=finding.line,
                    message="{} {}".format(finding.message or "", " ".join(raw)),
                    evidence=finding.evidence, correction=finding.correction,
                    klass=finding.klass, value_length=finding.value_length,
                    value_sha256_8=finding.value_sha256_8))
            return out

        crp._sanitize = leaky
        try:
            with contextlib.redirect_stdout(buffer), \
                    contextlib.redirect_stderr(buffer):
                code = crp.main(argv)
        finally:
            crp._sanitize = original
        self.assertEqual(code, crp.EXIT_INTERNAL,
                         "an unsanitised finding must abort, not print")
        self.assertNotIn(self.SECRET, buffer.getvalue())
        self.assertIn("INTERNAL ERROR", buffer.getvalue())

    def test_enforce_redaction_rejects_an_embedded_value(self):
        finding = crp._finding(
            "HG-PKG-LEAK-003", crp.SEV_FAIL, "f", 1,
            "value {} leaked".format(self.SECRET), "e", "c")
        with self.assertRaises(csc.RedactionGuardError):
            csc.enforce_redaction([finding], [self.SECRET])

    def test_mask_is_applied_before_serialisation(self):
        masked = crp._mask("see {} here".format(self.SECRET), [self.SECRET])
        self.assertNotIn(self.SECRET, masked)
        self.assertIn("[redacted]", masked)


# ---------------------------------------------------------------------------
# Importable API contract
# ---------------------------------------------------------------------------


class TestImportableApi(FixtureCase):
    def test_import_is_side_effect_free(self):
        proc = subprocess.run(
            [sys.executable, "-c",
             "import sys; sys.path.insert(0, {!r}); import check_release_package "
             "as m; sys.stdout.write('IMPORTED ' + m.PROGRAM)".format(str(SCRIPTS))],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "IMPORTED check_release_package")
        self.assertEqual(proc.stderr, "")

    def test_entry_point_signature_and_result_shape(self):
        import inspect

        signature = inspect.signature(crp.audit_package)
        self.assertEqual(list(signature.parameters), [
            "package_path", "expected_licence", "package_script", "repo",
            "include_topology", "max_scan_bytes", "severity_min"])
        self.assertTrue(all(
            p.kind is inspect.Parameter.KEYWORD_ONLY
            for name, p in signature.parameters.items()
            if name != "package_path"))

    def test_audit_package_does_not_print(self):
        rel = self.release()
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(buffer):
            result = crp.audit_package(str(rel.outdir),
                                       package_script=rel.script)
        self.assertEqual(buffer.getvalue(), "")
        self.assertEqual(result["counts"], {"FAIL": 0, "REVIEW": 0})

    def test_bad_input_raises_instead_of_exiting(self):
        with self.assertRaises(crp.PackageInputError):
            crp.audit_package(str(self.tmp / "nope"))
        with self.assertRaises(crp.PackageInputError):
            crp.audit_package(str(self.tmp / "not-an-archive.zip"))

    def test_cli_exit_codes_match_the_api(self):
        rel = self.release()
        code, _out, err = run_cli(str(rel.outdir),
                                  "--package-script", str(rel.script))
        self.assertEqual(code, crp.EXIT_OK)
        self.assertEqual(err, "")
        broken = self.release(drop=("NOTICE",))
        code, _out, _err = run_cli(str(broken.outdir),
                                   "--package-script", str(broken.script))
        self.assertEqual(code, crp.EXIT_FAIL)
        code, _out, _err = run_cli(str(self.tmp / "missing"),
                                   "--package-script", str(rel.script))
        self.assertEqual(code, crp.EXIT_INTERNAL)

    def test_quiet_prints_nothing(self):
        rel = self.release(drop=("NOTICE",))
        code, out, err = run_cli(str(rel.outdir), "--package-script",
                                 str(rel.script), "--quiet")
        self.assertEqual(code, crp.EXIT_FAIL)
        self.assertEqual(out, "")
        self.assertEqual(err, "")

    def test_no_value_ever_appears_in_the_notes_either(self):
        """A credential in a packaged file must not leak through the notes."""
        secret = TestSecretRedaction.SECRET
        rel = self.release(extra_files={
            "config/homeguardian.demo.json":
                json.dumps({"PrivateKey": secret}, indent=2) + "\n",
        })
        result = self.audit(rel.outdir, script=rel.script)
        for note in result["notes"]:
            self.assertNotIn(secret, note)


# ---------------------------------------------------------------------------
# Fixture 9 - a mandatory file the candidate dropped (two release documents)
# ---------------------------------------------------------------------------

#: The real defect: MANDATORY_FILES named two release documents the candidate no
#: longer carried, so the packaging script's own mandatory-files step exited 1
#: and wrote no manifest at all.
RELEASE_DOCUMENTS = ("docs/SECURITY_READINESS_V8.md", "docs/RELEASE_CANDIDATE_V21.md")


class TestMandatoryFilesRegression(FixtureCase):
    """A file the packaging declares mandatory and the candidate dropped is FAIL."""

    MANDATORY = tuple(DEFAULT_MANDATORY) + RELEASE_DOCUMENTS

    def test_two_declared_mandatory_documents_absent_are_fail_and_named(self):
        # The candidate dropped both documents, and (as in the real defect) the
        # packaging aborted before it wrote any manifest.
        rel = self.release(mandatory=self.MANDATORY, manifest=False,
                           checksums=False)
        result = self.audit(rel.outdir, script=rel.script)
        src = findings_for(result, "HG-PKG-SRC-001")
        self.assertEqual(sorted(f["file"] for f in src), sorted(RELEASE_DOCUMENTS),
                         rules_of(result))
        for finding in src:
            self.assertEqual(finding["severity"], "FAIL")
            self.assertIn(finding["file"], json.dumps(finding),
                          "the finding must name the missing path")
        self.assertEqual(status_of(result, 2), "FAIL")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)
        # Non-vacuous: the list under test came out of the packaging script
        # verbatim, and it is exactly the declared set that is enforced.
        self.assertEqual(result["packaging_rules"]["source"], str(rel.script))
        self.assertEqual(result["packaging_rules"]["mandatory_count"],
                         len(self.MANDATORY))
        self.assertEqual(
            sorted(f["file"] for f in src), sorted(RELEASE_DOCUMENTS))

    def test_the_same_mandatory_list_is_satisfied_when_the_candidate_carries_them(self):
        """Control: the fixture does not fire when the documents are shipped."""
        files = base_files(extra={name: "# {} fixture\n".format(name)
                                  for name in RELEASE_DOCUMENTS})
        rel = self.release(files=files, mandatory=self.MANDATORY)
        result = self.audit(rel.outdir, script=rel.script)
        self.assertNotIn("HG-PKG-SRC-001", rules_of(result))
        self.assertNotEqual(status_of(result, 2), "FAIL")


# ---------------------------------------------------------------------------
# Fixture 10 - runtime attribution omissions
# ---------------------------------------------------------------------------

ROOT_TREE_PACKAGES = 42       # the ~42 root-tree npm packages that were unattributed
NESTED_PACKAGES = 4           # the Alexa/nested components that were unattributed


def _pinned_entry(name, version, licence):
    return {
        "version": version,
        "license": licence,
        "resolved": "https://registry.npmjs.org/{}/-/{}-{}.tgz".format(
            name, name, version),
        "integrity": "sha512-fixture-{}-{}-integrity".format(name, version),
    }


def _lockfile_with(packages, root=None):
    """A lockfileVersion-3 lockfile carrying ``packages`` (key -> entry)."""
    data = {
        "name": root or "homeguardian-web-mcp",
        "version": "1.0.0",
        "lockfileVersion": 3,
        "packages": {
            "": {"name": root or "homeguardian-web-mcp", "version": "1.0.0",
                 "license": "Apache-2.0",
                 "dependencies": {"rootpkg-00": "^1.0.0"}},
        },
    }
    data["packages"].update(packages)
    return data


def unattributed_root_tree(count=ROOT_TREE_PACKAGES, licence="MIT"):
    """``count`` pinned runtime packages, named so they are easy to assert on."""
    return {
        "node_modules/rootpkg-{:02d}".format(index):
            _pinned_entry("rootpkg-{:02d}".format(index), "1.0.0", licence)
        for index in range(count)
    }


def unattributed_nested(count=NESTED_PACKAGES, licence="MIT"):
    """The nested (Alexa bridge) tree, in its own lockfile/project directory."""
    return {
        "node_modules/alexapkg-{}".format(index):
            _pinned_entry("alexapkg-{}".format(index), "1.0.0", licence)
        for index in range(count)
    }


def with_mediabunny(packages):
    packages = dict(packages)
    packages["node_modules/mediabunny"] = {
        "version": "1.61.5",
        "license": "MPL-2.0",
        "resolved": "https://registry.npmjs.org/mediabunny/-/mediabunny-1.61.5.tgz",
        "integrity": "sha512-fixture-mediabunny-integrity",
    }
    return packages


class TestAttributionOmissionsRegression(FixtureCase):
    """Runtime dependencies absent from the attribution record are detected.

    The real case: ~42 root-tree packages and 4 nested (Alexa) components were
    not attributed at all, and mediabunny 1.61.5 under MPL-2.0 was not named.
    """

    def omitted_package(self, *, notice=None, licence="MIT", with_mpl=True):
        packages = unattributed_root_tree(licence=licence)
        if with_mpl:
            packages = with_mediabunny(packages)
        return self.release(
            lockfile=_lockfile_with(packages),
            extra_files={
                "alexa_skill/package-lock.json": json.dumps(
                    _lockfile_with(unattributed_nested(), root="alexa-bridge"),
                    indent=2) + "\n",
            },
            notice=notice,
        )

    def test_root_tree_and_nested_components_are_fail_and_aggregated(self):
        rel = self.omitted_package()
        result = self.audit(rel.outdir, script=rel.script)
        stats = result["stats"]["dependencies"]

        # Non-vacuous: the lockfiles really were read, and every runtime entry
        # of both trees was collected (47 npm components, not zero).
        self.assertEqual(stats["npm_components"],
                         ROOT_TREE_PACKAGES + NESTED_PACKAGES + 1)
        self.assertEqual(stats["attribution_files"], ["NOTICE"])

        group = findings_for(result, "HG-PKG-DEP-002")
        self.assertEqual(len(group), 1, rules_of(result))
        self.assertEqual(group[0]["severity"], "FAIL")
        self.assertIn("{} runtime component(s) under MIT".format(
            ROOT_TREE_PACKAGES + NESTED_PACKAGES), group[0]["message"])
        self.assertIn("rootpkg-00@1.0.0", group[0]["message"])
        self.assertIn("alexapkg-0@1.0.0", group[0]["message"])
        self.assertEqual(stats["uncovered_by_licence"],
                         {"MIT": ROOT_TREE_PACKAGES + NESTED_PACKAGES})
        # The nested tree is a different project directory, and it is reported
        # in the same aggregate without being merged away.
        self.assertIn("alexa_skill/package-lock.json", group[0]["evidence"])
        self.assertEqual(status_of(result, 6), "FAIL")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)

    def test_an_unnamed_mpl_component_is_fail_by_name_not_by_licence_summary(self):
        lockfile = _lockfile_with(with_mediabunny(
            unattributed_root_tree(count=0)))
        # Even a licence-level statement that DOES mention MPL in an npm-scoped
        # context is not enough: MPL-2.0 needs the component identified.
        notice = NOTICE.replace(
            "Bundled third-party components:",
            "Web gateway runtime dependencies (npm) are bundled under MPL-2.0 "
            "and MIT as applicable.\n\nBundled third-party components:")
        rel = self.release(lockfile=lockfile, notice=notice)
        result = self.audit(rel.outdir, script=rel.script)
        named = findings_for(result, "HG-PKG-DEP-001")
        self.assertEqual(len(named), 1, rules_of(result))
        self.assertEqual(named[0]["severity"], "FAIL")
        for token in ("mediabunny", "1.61.5", "MPL-2.0"):
            self.assertIn(token, named[0]["message"])
        self.assertIn("mediabunny", named[0]["evidence"])
        self.assertEqual(
            result["stats"]["dependencies"]["per_package_required_missing"],
            ["mediabunny"])
        # It is NOT swallowed into the collective bucket for MPL.
        for finding in findings_for(result, "HG-PKG-DEP-003"):
            self.assertNotIn("mediabunny", finding["message"])

    def test_group_coverage_does_not_count_as_individual_attribution(self):
        """A dependency claimed to be covered 'as a group' is REVIEW, not covered."""
        rel = self.release(
            lockfile=_lockfile_with(unattributed_root_tree()),
            notice=NOTICE.replace(
                "Bundled third-party components:",
                "Bundled third-party components:\nThe npm runtime dependencies "
                "of the web gateway are bundled under the MIT License."),
        )
        result = self.audit(rel.outdir, script=rel.script)
        stats = result["stats"]["dependencies"]
        collective = findings_for(result, "HG-PKG-DEP-003")
        self.assertEqual(len(collective), ROOT_TREE_PACKAGES,
                         rules_of(result)[:5])
        self.assertTrue(all(f["severity"] == "REVIEW" for f in collective))
        self.assertIn("rootpkg-00@1.0.0",
                      [f["message"].split("'")[1] for f in collective])
        # The group statement did NOT make any component individually attributed,
        # and it did not clear the finding either.
        self.assertEqual(stats["covered"], [])
        self.assertEqual(stats["uncovered_by_licence"], {})
        self.assertEqual(status_of(result, 6), "REVIEW")
        self.assertNotEqual(status_of(result, 6), "PASS")
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_REVIEW)


# ---------------------------------------------------------------------------
# Fixture 11 - the reviewed allow-list for synthetic credential fixtures
# ---------------------------------------------------------------------------

#: The credential-shaped literals that genuinely ship inside TEST files.  Each is
#: a synthetic fixture - a negative-test password, a unit test's local secret, a
#: key-material literal whose decoded form contains "test-key-material" - but the
#: checker cannot prove that from the artefact, so every one of them is FAIL.
TEST_KEY_MATERIAL = base64.b64encode(b"wireguard-test-key-material-32b!").decode("ascii")

SYNTHETIC_TEST_SECRETS = {
    "web_mcp/tests/auth.test.js":
        'server.start();\nconst ' + 'pass' + 'word = "sj-test-password-8842";\n',
    "tests/cpp/test_secret_store.cpp":
        '#include <string>\n'
        'std::string ' + 'sec' + 'ret = "hg-local-test-secret-material-01";\n',
    "tests/fixtures/webrtc.fixture.txt":
        'fixture key material follows\n'
        'Private' + 'Key = "' + TEST_KEY_MATERIAL + '"\n',
    "web_mcp/tests/mcp.test.js":
        'const ' + 'api_' + 'key = "hg-test-api-key-0001";\n',
    "tests/cpp/test_alexa_credentials.cpp":
        'std::string ' + 'access_' + 'token = "alexa-test-token-123456";\n',
    "web_mcp/tests/doorbell.test.js":
        'const ' + 'client_sec' + 'ret = "hg-test-client-secret-77";\n',
    "tests/fixtures/synthetic_tokens.txt":
        'github_' + 'token = "' + 'ghp_' + 'testonly00000000000000000000000000"\n',
}

#: The raw values, kept only so the tests can assert they never appear in any
#: output.  The checker never carries them into a finding.
SYNTHETIC_TEST_SECRET_VALUES = (
    "sj-test-password-8842",
    "hg-local-test-secret-material-01",
    TEST_KEY_MATERIAL,
    "hg-test-api-key-0001",
    "alexa-test-token-123456",
    "hg-test-client-secret-77",
    "ghp_" + "testonly00000000000000000000000000",
)


class TestSyntheticSecretAllowlist(FixtureCase):
    """The reviewed allow-list: what it downgrades, and what it cannot hide.

    What it DOES: downgrade ONE FAIL credential finding - matched by file, line,
    pattern class and the matched value's sha256[:8] - to REVIEW, with the
    finding still reported, still carrying its class/length/digest, and with the
    value still never printed.
    What it does NOT do: suppress the finding, cover any other value at the same
    location, or change the behaviour of a run that has no allow-list.
    """

    def synthetic_package(self, secrets=None):
        return self.release(extra_files=dict(secrets or SYNTHETIC_TEST_SECRETS))

    def credential_findings(self, result):
        return findings_for(result, "HG-PKG-LEAK-003")

    def credential_finding(self, result, *, file, pattern_class):
        for finding in self.credential_findings(result):
            if finding["file"] == file and finding["pattern_class"] == pattern_class:
                return finding
        self.fail("no HG-PKG-LEAK-003 finding for {} / {}: {}".format(
            file, pattern_class, json.dumps(self.credential_findings(result))))

    def allowlist_file(self, entries, name="release-secret-allowlist.json"):
        path = self.tmp / name
        path.write_text(json.dumps({
            "format": crp.SECRET_ALLOWLIST_FORMAT,
            "entries": entries,
        }, indent=2), encoding="utf-8")
        return path

    # -- (a) an allow-listed synthetic fixture is REVIEW, not FAIL -------------

    def test_allowlisted_synthetic_fixture_is_review_and_the_finding_says_so(self):
        rel = self.synthetic_package()
        baseline = self.audit(rel.outdir, script=rel.script)
        creds = self.credential_findings(baseline)
        # Non-vacuous pre-condition: with no allow-list every one of these 8
        # fixture literals is FAIL, including the test-key-material one.
        self.assertEqual(len(creds), 8, rules_of(baseline))
        self.assertTrue(all(f["severity"] == "FAIL" for f in creds))
        key_finding = self.credential_finding(
            baseline, file="tests/fixtures/webrtc.fixture.txt",
            pattern_class="wireguard-key")
        # The "test-key-material" case really is the literal under test.
        self.assertIn(TEST_KEY_MATERIAL,
                      SYNTHETIC_TEST_SECRETS["tests/fixtures/webrtc.fixture.txt"])

        target = self.credential_finding(
            baseline, file="web_mcp/tests/auth.test.js",
            pattern_class="secret-assignment")
        allow = self.allowlist_file([{
            "file": target["file"],
            "line": target["line"],
            "pattern_class": target["pattern_class"],
            "value_sha256_8": target["value_sha256_8"],
            "reason": "negative-test password asserted by the JS test",
        }])
        # Non-vacuous: the entry carries ONLY the truncated digest, never the
        # value, so the checked-in file cannot leak what it describes.
        self.assertNotIn("sj-test-password-8842",
                         allow.read_text(encoding="utf-8"))

        code, out, err = run_cli(str(rel.outdir), "--package-script",
                                 str(rel.script), "--json",
                                 "--secret-allowlist", str(allow))
        report = json.loads(out)
        reviewed = self.credential_finding(
            report, file=target["file"], pattern_class=target["pattern_class"])
        self.assertEqual(reviewed["severity"], "REVIEW")
        self.assertIn("ALLOW-LISTED FOR REVIEW", reviewed["message"])
        self.assertIn("synthetic fixture", reviewed["message"])
        # The finding is not suppressed: it is still there, with the same
        # identifying fields, and it still cannot print the value.
        self.assertEqual(reviewed["value_sha256_8"], target["value_sha256_8"])
        self.assertEqual(reviewed["value_length"], target["value_length"])
        self.assertEqual(reviewed["line"], target["line"])
        self.assertIn("[redacted:", reviewed["evidence"])
        # Everything else is untouched - the allow-list is not a blanket pass.
        others = [f for f in self.credential_findings(report)
                  if f["file"] != target["file"]]
        self.assertEqual(len(others), 7, rules_of(report))
        self.assertTrue(all(f["severity"] == "FAIL" for f in others),
                        [(f["file"], f["severity"]) for f in others])
        self.assertEqual(
            self.credential_finding(report,
                                    file="tests/fixtures/webrtc.fixture.txt",
                                    pattern_class="wireguard-key")["severity"],
            "FAIL")
        self.assertEqual(key_finding["severity"], "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 1)
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 1)
        self.assertEqual(code, crp.EXIT_FAIL)

        for blob in (out, err):
            for value in SYNTHETIC_TEST_SECRET_VALUES:
                self.assertNotIn(value, blob)
        self.assertEqual(err, "")

    # -- (b) the allow-list cannot hide a DIFFERENT value ---------------------

    def test_changing_the_value_at_an_allowlisted_location_returns_to_fail(self):
        original = self.synthetic_package()
        first = self.audit(original.outdir, script=original.script)
        target = self.credential_finding(
            first, file="web_mcp/tests/auth.test.js",
            pattern_class="secret-assignment")
        allow = self.allowlist_file([{
            "file": target["file"], "line": target["line"],
            "pattern_class": target["pattern_class"],
            "value_sha256_8": target["value_sha256_8"],
            "reason": "negative-test password asserted by the JS test",
        }])

        # Control: the very same package, through this allow-list, is REVIEW.
        with mock.patch.dict(os.environ,
                             {"HG_SECRET_ALLOWLIST": str(allow)}):
            reviewed = self.audit(original.outdir, script=original.script)
        self.assertEqual(
            self.credential_finding(reviewed, file=target["file"],
                                    pattern_class=target["pattern_class"]
                                    )["severity"], "REVIEW")
        self.assertEqual(reviewed["stats"]["secret_allowlist"]["applied"], 1)

        # Now change ONLY the value, on the same line and in the same class.
        mutated = dict(SYNTHETIC_TEST_SECRETS)
        before = SYNTHETIC_TEST_SECRETS["web_mcp/tests/auth.test.js"]
        mutated["web_mcp/tests/auth.test.js"] = (
            'server.start();\nconst ' + 'pass' + 'word = "sj-test-password-9999";\n')
        self.assertEqual(len(mutated["web_mcp/tests/auth.test.js"]), len(before),
                         "the edit must keep the file the same size and shape")
        changed = self.synthetic_package(mutated)

        with mock.patch.dict(os.environ,
                             {"HG_SECRET_ALLOWLIST": str(allow)}):
            after = self.audit(changed.outdir, script=changed.script)
        finding = self.credential_finding(after, file=target["file"],
                                         pattern_class=target["pattern_class"])
        self.assertEqual(finding["line"], target["line"])
        self.assertNotEqual(finding["value_sha256_8"], target["value_sha256_8"])
        self.assertEqual(finding["severity"], "FAIL")
        self.assertNotIn("ALLOW-LISTED", finding["message"])
        # Non-vacuous: the entry was still loaded, and it matched nothing.
        self.assertEqual(after["stats"]["secret_allowlist"]["entries"], 1)
        self.assertEqual(after["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(crp.exit_code_for(after), crp.EXIT_FAIL)

    def test_an_entry_that_omits_the_value_digest_matches_nothing(self):
        """An entry without all four keys suppresses nothing (safe direction)."""
        rel = self.synthetic_package()
        baseline = self.audit(rel.outdir, script=rel.script)
        target = self.credential_finding(
            baseline, file="web_mcp/tests/auth.test.js",
            pattern_class="secret-assignment")
        # The file+line+class are right, but the value key is missing.
        allow = self.allowlist_file([{
            "file": target["file"], "line": target["line"],
            "pattern_class": target["pattern_class"],
            "reason": "tried to allow-list by location alone",
        }], name="location-only.json")
        code, out, _err = run_cli(str(rel.outdir), "--package-script",
                                  str(rel.script), "--json",
                                  "--secret-allowlist", str(allow))
        report = json.loads(out)
        self.assertEqual(
            self.credential_finding(report, file=target["file"],
                                    pattern_class=target["pattern_class"]
                                    )["severity"], "FAIL")
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 0)
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertTrue(any("malformed" in n for n in report["notes"]),
                        report["notes"])
        self.assertEqual(code, crp.EXIT_FAIL)

    # -- (c) no allow-list: unchanged behaviour ------------------------------

    def test_with_no_allowlist_the_findings_are_unchanged(self):
        rel = self.synthetic_package()
        # Neutralise any ambient HG_SECRET_ALLOWLIST so "no allow-list" is
        # exactly what this run has.
        with mock.patch.dict(os.environ, {"HG_SECRET_ALLOWLIST": ""}):
            result = self.audit(rel.outdir, script=rel.script)
        creds = self.credential_findings(result)
        self.assertEqual(len(creds), 8, rules_of(result))
        self.assertTrue(all(f["severity"] == "FAIL" for f in creds))
        self.assertNotIn("ALLOW-LISTED", json.dumps(result))
        self.assertIsNone(result["stats"]["secret_allowlist"]["path"])
        self.assertEqual(result["stats"]["secret_allowlist"]["entries"], 0)
        self.assertEqual(result["stats"]["secret_allowlist"]["applied"], 0)
        self.assertEqual(result["counts"]["REVIEW"], 0)
        self.assertEqual(crp.exit_code_for(result), crp.EXIT_FAIL)
        # The redaction guarantee is unchanged by the mechanism's presence.
        blob = json.dumps(result)
        for value in SYNTHETIC_TEST_SECRET_VALUES:
            self.assertNotIn(value, blob)
            self.assertNotIn(value, " ".join(result["notes"]))

    def test_a_malformed_allowlist_suppresses_nothing(self):
        rel = self.synthetic_package()
        broken = self.tmp / "broken.json"
        broken.write_text("{ this is not json", encoding="utf-8")
        code, out, err = run_cli(str(rel.outdir), "--package-script",
                                 str(rel.script), "--json",
                                 "--secret-allowlist", str(broken))
        report = json.loads(out)
        creds = self.credential_findings(report)
        self.assertEqual(len(creds), 8, rules_of(report))
        self.assertTrue(all(f["severity"] == "FAIL" for f in creds))
        self.assertEqual(report["stats"]["secret_allowlist"]["applied"], 0)
        self.assertTrue(any("could not be read" in n for n in report["notes"]),
                        report["notes"])
        self.assertEqual(code, crp.EXIT_FAIL)

    def test_a_missing_allowlist_file_suppresses_nothing(self):
        rel = self.synthetic_package()
        absent = self.tmp / "not-there.json"
        code, out, _err = run_cli(str(rel.outdir), "--package-script",
                                  str(rel.script), "--json",
                                  "--secret-allowlist", str(absent))
        report = json.loads(out)
        self.assertTrue(all(f["severity"] == "FAIL"
                            for f in self.credential_findings(report)))
        self.assertEqual(report["stats"]["secret_allowlist"]["entries"], 0)
        self.assertTrue(any("no secret allow-list at" in n
                            for n in report["notes"]), report["notes"])
        self.assertEqual(code, crp.EXIT_FAIL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
