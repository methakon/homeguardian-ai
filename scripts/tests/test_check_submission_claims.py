#!/usr/bin/env python3
"""Unit tests for scripts/check_submission_claims.py.

Everything here builds its own fixture trees under tempfile.mkdtemp(): the tests
never read the real repository, so they are deterministic and safe to run
anywhere.  Only the standard library is used.

Run:
    python3 scripts/tests/test_check_submission_claims.py -v
    python3 -m unittest discover -s scripts/tests -t scripts -v
"""

from __future__ import annotations

import io
import contextlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_submission_claims as csc  # noqa: E402

APACHE_HEADER = (
    "\n                                 Apache License\n"
    "                           Version 2.0, January 2004\n"
    "                        http://www.apache.org/licenses/\n"
)

NEGATIVE_CLAIM_LINES = """\
- VPN / WireGuard: not implemented in this fixture.
- Remote access: no public ingress is exposed by this fixture.
- Remote MCP reachability: not implemented; no remote MCP endpoint is exposed.
- Mi A3 / phone-hosted service: not implemented in this fixture.
- V380 camera: no V380 camera support is present in this fixture.
- Physical hardware verification: not verified on any physical device.
- Physical camera capture: no physical camera or microphone is ever activated.
- Alexa integration: not built; no Alexa+ device integration exists in this fixture.
"""

#: Every subject is mentioned and every mention is an explicit negation, so the
#: claim matrix must stay silent.  This is the false-positive guard fixture.
CLEAN_FILES = {
    "README.md": (
        "# Fixture Project\n\n"
        "A quiet, honest fixture submission used by the compliance scanner tests.\n\n"
        "## What is implemented\n\n"
        "- A local-only event pipeline with a fail-closed consent gate.\n"
        "- A loopback HTTP adapter that never listens on a public interface.\n\n"
        "## Not implemented in this fixture\n\n" + NEGATIVE_CLAIM_LINES + "\n"
        "## License\n\n"
        "This project is licensed under the Apache License, Version 2.0.\n"
    ),
    "LICENSE": APACHE_HEADER,
    "NOTICE": (
        "Fixture Project\nCopyright 2026 Fixture Author\n\n"
        "This product is licensed under the Apache License, Version 2.0 (see the\n"
        "LICENSE file).\n\n"
        "Third-party components:\n"
        "  - fixture-json \u2014 MIT License. https://example.invalid/fixture-json\n"
    ),
    "package.json": json.dumps(
        {
            "name": "fixture-project",
            "version": "1.2.3",
            "license": "Apache-2.0",
            "dependencies": {},
        },
        indent=2,
    ),
    "docs/DEVPOST_SUBMISSION.md": (
        "# Fixture Project \u2014 Devpost description\n\n"
        "This fixture has no remote access, no VPN and no MCP server.\n\n"
        + NEGATIVE_CLAIM_LINES
    ),
    "docs/DEVPOST_DEMONSTRATION_GUIDE.md": (
        "# Demonstration guide\n\n"
        "Everything below runs on loopback; no remote MCP reachability is used\n"
        "and no phone-hosted service is involved.\n\n"
        "1. Start the local service.\n"
        "2. Open the loopback URL.\n"
    ),
    "docs/SUBMISSION_VIDEO_SCRIPT.md": (
        "# Video script\n\n"
        "Narration: this is a local simulation; no physical camera is used and no\n"
        "Alexa+ integration is claimed.\n"
    ),
    "docs/SUBMISSION_PACKAGE.md": (
        "# Submission package\n\n"
        "**License:** Apache License 2.0 (see LICENSE and NOTICE).\n\n"
        "Everything in this package is synthetic; the fixture has no hardware\n"
        "verification, no remote access and no VPN.\n"
    ),
    "docs/PROOF_REGISTER.md": (
        "# Proof register\n\n"
        "| claim | proof |\n|---|---|\n"
        "| consent gate blocks delivery | fixture test |\n"
    ),
    "VERSION": "1.2.3\n",
}


def write_tree(base: Path, files: dict[str, str]) -> Path:
    for rel, content in files.items():
        path = base / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return base


def run_cli(*argv: str) -> tuple[int, str, str]:
    """Run main() in-process, capturing stdout/stderr."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = csc.main(list(argv))
    return code, out.getvalue(), err.getvalue()


def scan_json(root: Path, *extra: str) -> dict:
    code, out, _err = run_cli(str(root), "--json", *extra)
    return {"exit": code, "report": json.loads(out)}


class FixtureCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="hg-scan-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def tree(self, files: dict[str, str]) -> Path:
        base = self.tmp / "tree"
        base.mkdir(exist_ok=True)
        return write_tree(base, files)

    def git(self, repo: Path, *args: str, date: str | None = None) -> str:
        env = dict(os.environ)
        env.setdefault("GIT_AUTHOR_NAME", "Fixture")
        env.setdefault("GIT_AUTHOR_EMAIL", "fixture@example.invalid")
        env.setdefault("GIT_COMMITTER_NAME", "Fixture")
        env.setdefault("GIT_COMMITTER_EMAIL", "fixture@example.invalid")
        if date:
            env["GIT_AUTHOR_DATE"] = date
            env["GIT_COMMITTER_DATE"] = date
        proc = subprocess.run(
            ["git", "-C", str(repo), *args],
            capture_output=True,
            text=True,
            env=env,
            check=True,
        )
        return proc.stdout.strip()

    def git_repo(self, initial: dict[str, str]) -> Path:
        repo = self.tmp / "repo"
        repo.mkdir()
        self.git(repo, "init", "-q")
        write_tree(repo, initial)
        self.git(repo, "add", "-A")
        self.git(repo, "commit", "-q", "-m", "initial", date="2026-01-02T00:00:00")
        return repo

    @staticmethod
    def rules(report: dict) -> list[str]:
        return sorted({f["rule"] for f in report["findings"]})

    @staticmethod
    def findings_for(report: dict, rule: str) -> list[dict]:
        return [f for f in report["findings"] if f["rule"] == rule]


# ---------------------------------------------------------------------------
# Mandatory test (a): a clean fixture tree must produce zero findings
# ---------------------------------------------------------------------------


class TestCleanFixtureNoFalsePositives(FixtureCase):
    def test_clean_fixture_yields_zero_findings_and_exit_0(self) -> None:
        root = self.tree(CLEAN_FILES)

        code, out, err = run_cli(str(root), "--json")
        report = json.loads(out)

        self.assertEqual(err, "")
        self.assertEqual(report["findings"], [])
        self.assertEqual(report["counts"], {"FAIL": 0, "REVIEW": 0})
        self.assertEqual(code, csc.EXIT_OK)

        # ... and the human-readable report agrees.
        code_text, out_text, _ = run_cli(str(root))
        self.assertEqual(code_text, csc.EXIT_OK)
        self.assertIn("no findings", out_text)

        # Missing by design in this fixture: reported as notes, never findings.
        self.assertTrue(
            any("DEVPOST_DEMONSTRATION_GUIDE" not in n for n in report["notes"])
            or report["notes"] == []
        )

    def test_clean_fixture_submission_scope_also_zero(self) -> None:
        root = self.tree(CLEAN_FILES)
        result = scan_json(root, "--doc-scope", "submission")
        self.assertEqual(result["report"]["findings"], [])
        self.assertEqual(result["exit"], csc.EXIT_OK)


# ---------------------------------------------------------------------------
# Mandatory test (b): credentials never reach stdout/JSON; the guard fires
# ---------------------------------------------------------------------------


class TestCredentialRedaction(FixtureCase):
    SECRET = "d2lyZWd1YXJkLXRlc3Qta2V5LW1hdGVyaWFsLTMyYiE="  # 43 base64 chars + '='

    def credential_tree(self) -> Path:
        return self.tree(
            {
                "README.md": "# Fixture\n\nSee the notes.\n",
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Notes\n\nPrivateKey = {}\n".format(self.SECRET)
                ),
            }
        )

    def test_wireguard_key_flagged_and_value_never_printed(self) -> None:
        root = self.credential_tree()

        code_text, out_text, err_text = run_cli(str(root))
        code_json, out_json, err_json = run_cli(str(root), "--json")

        for blob in (out_text, err_text, out_json, err_json):
            self.assertNotIn(self.SECRET, blob)
            self.assertNotIn(self.SECRET[:-1], blob)  # without the padding too
        self.assertEqual(err_text, "")

        report = json.loads(out_json)
        creds = self.findings_for(report, "HG-CRED-001")
        self.assertEqual(len(creds), 1, report["findings"])
        finding = creds[0]
        self.assertEqual(finding["pattern_class"], "wireguard-key")
        self.assertEqual(finding["value_length"], len(self.SECRET))
        self.assertEqual(finding["value_sha256_8"], csc._sha256_8(self.SECRET))
        self.assertEqual(code_json, csc.EXIT_FAIL)
        self.assertIn("[redacted:", finding["evidence"])
        # The rule id, file and line are still reported for the human.
        self.assertEqual(finding["file"], "docs/DEVPOST_SUBMISSION.md")
        self.assertEqual(finding["line"], 3)

    def test_redaction_guard_fires_when_a_message_embeds_the_value(self) -> None:
        leaking = csc.Finding(
            rule="HG-CRED-001",
            severity=csc.SEV_FAIL,
            file="docs/DEVPOST_SUBMISSION.md",
            line=3,
            message="private key {} found".format(self.SECRET),
            evidence="PrivateKey = {}".format(self.SECRET),
            correction="rotate it",
        )
        with self.assertRaises(csc.RedactionGuardError) as ctx:
            csc.enforce_redaction([leaking], [self.SECRET])
        # The guard's own message must not disclose the value either.
        self.assertNotIn(self.SECRET, str(ctx.exception))
        self.assertIn("length", str(ctx.exception))

    def test_redaction_guard_passes_clean_findings(self) -> None:
        clean = csc.Finding(
            rule="HG-CRED-001",
            severity=csc.SEV_FAIL,
            file="docs/x.md",
            line=1,
            message="possible published credential (value withheld)",
            evidence="[redacted: class=x]",
            correction="rotate it",
        )
        csc.enforce_redaction([clean], [self.SECRET])  # must not raise

    def test_guard_aborts_the_whole_run_with_internal_error(self) -> None:
        """If a future edit inlines the value, the CLI must refuse to print."""
        root = self.credential_tree()
        original = csc.scan_credentials

        def leaking_scan(doc_lines):
            _findings, sensitive = original(doc_lines)
            bad = csc.Finding(
                rule="HG-CRED-999",
                severity=csc.SEV_FAIL,
                file="README.md",
                line=1,
                message="leak {}".format(self.SECRET),
                evidence="leak {}".format(self.SECRET),
                correction="leak {}".format(self.SECRET),
            )
            return [bad], sensitive

        csc.scan_credentials = leaking_scan
        try:
            code, out, err = run_cli(str(root), "--json")
        finally:
            csc.scan_credentials = original

        self.assertEqual(code, csc.EXIT_INTERNAL)
        self.assertIn("INTERNAL ERROR", err)
        self.assertNotIn(self.SECRET, out)
        self.assertNotIn(self.SECRET, err)


# ---------------------------------------------------------------------------
# Rule 1 - licensing
# ---------------------------------------------------------------------------


class TestLicensingRules(FixtureCase):
    def test_lic001_mit_license_is_a_fail(self) -> None:
        files = dict(CLEAN_FILES)
        files["LICENSE"] = "MIT License\n\nPermission is hereby granted, free of charge...\n"
        root = self.tree(files)
        result = scan_json(root)
        self.assertIn("HG-LIC-001", self.rules(result["report"]))
        finding = self.findings_for(result["report"], "HG-LIC-001")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertEqual(finding["file"], "LICENSE")
        self.assertIn("MIT", finding["message"])
        self.assertEqual(result["exit"], csc.EXIT_FAIL)

    def test_lic001_missing_license_is_a_fail(self) -> None:
        files = {k: v for k, v in CLEAN_FILES.items() if k != "LICENSE"}
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-001")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertIn("not found", finding["message"])

    def test_lic002_missing_notice_is_a_review(self) -> None:
        files = {k: v for k, v in CLEAN_FILES.items() if k != "NOTICE"}
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-002")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertEqual(result["exit"], csc.EXIT_REVIEW)

    def test_lic003_notice_without_project_license_is_a_review(self) -> None:
        files = dict(CLEAN_FILES)
        files["NOTICE"] = "Fixture Project\n\nThird-party: fixture-json (MIT).\n"
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-003")[0]
        self.assertEqual(finding["severity"], "REVIEW")

    def test_lic010_manifest_without_license_field_is_a_review(self) -> None:
        files = dict(CLEAN_FILES)
        files["package.json"] = json.dumps({"name": "fixture-project", "version": "1.2.3"})
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-010")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("license", finding["message"])

    def test_lic011_manifest_license_conflict_is_a_fail(self) -> None:
        files = dict(CLEAN_FILES)
        files["package.json"] = json.dumps(
            {"name": "fixture-project", "version": "1.2.3", "license": "MIT"}
        )
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-011")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertEqual(result["exit"], csc.EXIT_FAIL)

    def test_lic020_competing_license_in_devpost_doc_is_a_review(self) -> None:
        files = dict(CLEAN_FILES)
        files["docs/DEVPOST_SUBMISSION.md"] += (
            "\nCompanion tooling is distributed under the MIT License.\n"
        )
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-020")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("docs/DEVPOST_SUBMISSION.md", finding["file"])

    def test_lic020_project_level_assertion_is_a_fail(self) -> None:
        files = dict(CLEAN_FILES)
        files["docs/DEVPOST_SUBMISSION.md"] = (
            "# Fixture\n\nThis project is licensed under the MIT License.\n"
        )
        root = self.tree(files)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-LIC-020")[0]
        self.assertEqual(finding["severity"], "FAIL")

    def test_third_party_attribution_line_is_not_flagged(self) -> None:
        files = dict(CLEAN_FILES)
        files["docs/PROOF_REGISTER.md"] += (
            "\n| bundled dependency | fixture-json \u2014 MIT License "
            "(https://example.invalid) |\n"
        )
        root = self.tree(files)
        result = scan_json(root)
        self.assertEqual(self.findings_for(result["report"], "HG-LIC-020"), [])


# ---------------------------------------------------------------------------
# Rule 2 - contradictory claims
# ---------------------------------------------------------------------------

CONTRADICTION_FILES = {
    "docs/DEVPOST_SUBMISSION.md": (
        "# Claims\n\nRemote MCP reachability is implemented and verified.\n"
    ),
    "docs/SUBMISSION_PACKAGE.md": (
        "# Package\n\nRemote MCP reachability is not verified; no remote MCP\n"
        "endpoint exists yet.\n"
    ),
}


class TestClaimRules(FixtureCase):
    def test_claim001_contradiction_is_a_fail(self) -> None:
        root = self.tree(CONTRADICTION_FILES)
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-CLAIM-001")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertIn("remote MCP", finding["message"])
        self.assertIn("ASSERTED", finding["evidence"])
        self.assertIn("NEGATED", finding["evidence"])
        self.assertEqual(result["exit"], csc.EXIT_FAIL)

    def test_claim002_unqualified_positive_is_a_review(self) -> None:
        root = self.tree(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Claims\n\nV380 camera integration is implemented and tested.\n"
                ),
                "LICENSE": APACHE_HEADER,
                "NOTICE": "Fixture Project\n\nLicensed under the Apache License, Version 2.0.\n",
            }
        )
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-CLAIM-002")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("V380", finding["message"])
        self.assertEqual(result["exit"], csc.EXIT_REVIEW)

    def test_claim_aspects_are_not_conflated(self) -> None:
        """Camera-capture negation must not 'contradict' device verification."""
        root = self.tree(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# H-03\n\nThe physical device verification passed on real hardware.\n"
                ),
                "docs/PROOF_REGISTER.md": (
                    "# Proof\n\nNo physical camera capture was performed; the camera is\n"
                    "never opened in this fixture.\n"
                ),
                "LICENSE": APACHE_HEADER,
                "NOTICE": "Fixture Project\n\nLicensed under the Apache License, Version 2.0.\n",
            }
        )
        report = scan_json(root)["report"]
        contradictions = [
            f for f in report["findings"] if f["rule"] in ("HG-CLAIM-001", "HG-CLAIM-003")
        ]
        self.assertEqual(contradictions, [], contradictions)
        # The device claim is still surfaced, but only as "no limitation stated".
        review = self.findings_for(report, "HG-CLAIM-002")
        self.assertEqual(len(review), 1, report["findings"])
        self.assertIn("[device]", review[0]["message"])

    def test_claim003_older_negated_file_is_review_not_fail(self) -> None:
        """A superseded statement in an older file is history, not a lie."""
        repo = self.git_repo(
            {self.old_doc(): "Remote MCP reachability is not implemented yet.\n"}
        )
        (repo / "docs/DEVPOST_SUBMISSION.md").write_text(
            "# Claims\n\nRemote MCP reachability is implemented and verified.\n",
            encoding="utf-8",
        )
        # A later commit for the newer file so the git dates differ by day.
        self.git(repo, "add", "-A")
        self.git(
            repo,
            "commit",
            "-q",
            "-m",
            "later",
            date="2026-03-04T00:00:00",
        )
        result = scan_json(repo)
        findings = [f for f in result["report"]["findings"] if f["rule"].startswith("HG-CLAIM")]
        self.assertEqual(len(findings), 1, findings)
        self.assertEqual(findings[0]["rule"], "HG-CLAIM-003")
        self.assertEqual(findings[0]["severity"], "REVIEW")
        self.assertIn("superseded", findings[0]["message"])

    @staticmethod
    def old_doc() -> str:
        return "docs/PROOF_REGISTER.md"


# ---------------------------------------------------------------------------
# Rule 3 - stale revision references
# ---------------------------------------------------------------------------


class TestRevisionRules(FixtureCase):
    def test_rev001_missing_revision_is_a_fail(self) -> None:
        fake = "0f1e2d3c4b5a69788796a5b4c3d2e1f00918a7b6"
        repo = self.git_repo(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Notes\n\nThe demo was verified at commit {} .\n".format(fake)
                )
            }
        )
        result = scan_json(repo)
        finding = self.findings_for(result["report"], "HG-REV-001")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertIn(fake, finding["message"])
        self.assertEqual(result["exit"], csc.EXIT_FAIL)

    def test_rev002_sha_outside_published_history_is_a_review(self) -> None:
        repo = self.git_repo({"docs/DEVPOST_SUBMISSION.md": "# Notes\n\nSee below.\n"})
        head = self.git(repo, "rev-parse", "HEAD")
        # A commit on a side branch: it exists, but is not an ancestor of HEAD.
        self.git(repo, "checkout", "-q", "-b", "side")
        (repo / "side.txt").write_text("side\n", encoding="utf-8")
        self.git(repo, "add", "-A")
        self.git(repo, "commit", "-q", "-m", "side work", date="2026-02-02T00:00:00")
        side = self.git(repo, "rev-parse", "HEAD")
        self.git(repo, "checkout", "-q", "-")
        (repo / "docs/DEVPOST_SUBMISSION.md").write_text(
            "# Notes\n\nAncestor commit {} and side commit {}.\n".format(head, side),
            encoding="utf-8",
        )
        self.git(repo, "add", "-A")
        self.git(repo, "commit", "-q", "-m", "cite both", date="2026-02-03T00:00:00")

        result = scan_json(repo)
        reviews = self.findings_for(result["report"], "HG-REV-002")
        self.assertEqual(len(reviews), 1, result["report"]["findings"])
        self.assertIn(side, reviews[0]["message"])
        self.assertEqual(reviews[0]["severity"], "REVIEW")
        # The ancestor reference stays silent.
        self.assertNotIn(head, reviews[0]["message"])

    def test_rev003_bare_hex_token_is_a_review(self) -> None:
        token = "abcdef0123456789abcdef0123456789abcdef01"
        repo = self.git_repo(
            {
                "docs/PROOF_REGISTER.md": (
                    "# Register\n\ncatalog hash {} identifies the fixture catalogue.\n".format(
                        token
                    )
                )
            }
        )
        result = scan_json(repo)
        finding = self.findings_for(result["report"], "HG-REV-003")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("not presented as a revision", finding["message"])

    def test_digest_and_identifier_context_is_not_flagged(self) -> None:
        repo = self.git_repo(
            {
                "docs/PROOF_REGISTER.md": (
                    "# Register\n\n"
                    "The artifact sha256 9f2c1e0b7a5d4c3b2a1908f7e6d5c4b3a29180ff7ee6dd5c4b3a291807fedcba is pinned.\n"
                    "Device node_id=hg-443a03b54537c21f reported the model string.\n"
                )
            }
        )
        result = scan_json(repo)
        self.assertEqual(
            [f for f in result["report"]["findings"] if f["rule"].startswith("HG-REV")],
            [],
        )

    def test_rev010_version_disagreement_with_manifest_is_a_review(self) -> None:
        root = self.tree(
            {
                "package.json": json.dumps(
                    {"name": "fixture-pkg", "version": "2.0.0", "license": "Apache-2.0"}
                ),
                "docs/SUBMISSION_PACKAGE.md": (
                    "# Package\n\nfixture-pkg release version 1.4.0 is packaged here.\n"
                ),
                "LICENSE": APACHE_HEADER,
                "NOTICE": "Fixture\n\nLicensed under the Apache License, Version 2.0.\n",
            }
        )
        result = scan_json(root)
        finding = self.findings_for(result["report"], "HG-REV-010")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("1.4.0", finding["message"])

    def test_rev011_future_date_in_packaged_metadata_is_a_review(self) -> None:
        repo = self.git_repo(
            {
                "docs/SUBMISSION_PACKAGE.md": (
                    "# Package\n\nThe fixture was published on 2099-01-01.\n"
                )
            }
        )
        result = scan_json(repo)
        finding = self.findings_for(result["report"], "HG-REV-011")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("2099-01-01", finding["message"])


# ---------------------------------------------------------------------------
# Rule 4 - possible published credentials
# ---------------------------------------------------------------------------


class TestCredentialRules(FixtureCase):
    def scan_line(self, line: str) -> dict:
        root = self.tree({"docs/DEVPOST_SUBMISSION.md": "# Notes\n\n" + line + "\n"})
        return scan_json(root)["report"]

    def test_cred002_private_key_header(self) -> None:
        report = self.scan_line("-----BEGIN RSA PRIVATE KEY-----")
        finding = self.findings_for(report, "HG-CRED-002")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertEqual(finding["pattern_class"], "private-key-header")

    def test_cred003_aws_access_key_id(self) -> None:
        report = self.scan_line("AKIAIOSFODNN7EXAMPLE is the access key id")
        finding = self.findings_for(report, "HG-CRED-003")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertNotIn("AKIAIOSFODNN7EXAMPLE", json.dumps(report))

    def test_cred004_github_token(self) -> None:
        token = "ghp_" + "A1b2C3d4E5f6G7h8I9j0" + "K1L2M3n4"
        report = self.scan_line("token = {}".format(token))
        rules = self.rules(report)
        self.assertTrue(
            {"HG-CRED-004", "HG-CRED-006"} & set(rules), rules
        )
        self.assertNotIn(token, json.dumps(report))

    def test_cred005_google_api_key(self) -> None:
        token = "AIza" + "SyD9f0e1d2c3b4a5F6g7H8i9J0kL1m2N3oP"
        report = self.scan_line("api_key = {}".format(token))
        self.assertIn("HG-CRED-005", self.rules(report))
        self.assertNotIn(token, json.dumps(report))

    def test_cred006_generic_secret_assignment(self) -> None:
        report = self.scan_line('password = "hunter2hunter2hunter2"')
        finding = self.findings_for(report, "HG-CRED-006")[0]
        self.assertEqual(finding["severity"], "FAIL")
        self.assertNotIn("hunter2hunter2hunter2", json.dumps(report))
        self.assertEqual(finding["value_length"], len("hunter2hunter2hunter2"))

    def test_cred006_placeholder_assignment_is_not_flagged(self) -> None:
        for placeholder in (
            'api_key = "REPLACE_ME"',
            "password = ${DB_PASSWORD}",
            'secret = "xxxxxxxx"',
        ):
            with self.subTest(placeholder=placeholder):
                report = self.scan_line(placeholder)
                self.assertNotIn("HG-CRED-006", self.rules(report))

    def test_cred007_private_ipv4_topology_is_a_review(self) -> None:
        report = self.scan_line("The hub VPN address is 10.66.0.2 and the peer is on 192.168.1.50")
        findings = self.findings_for(report, "HG-CRED-007")
        self.assertEqual(len(findings), 2, findings)
        self.assertEqual(findings[0]["severity"], "REVIEW")
        self.assertEqual(findings[0]["pattern_class"], "private-ipv4-topology")
        blob = json.dumps(report)
        self.assertNotIn("10.66.0.2", blob)
        self.assertNotIn("192.168.1.50", blob)

    def test_cred008_public_ipv4_literal_is_a_review(self) -> None:
        report = self.scan_line("The relay instance is at 203.0.113.7 publicly")
        finding = self.findings_for(report, "HG-CRED-008")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertNotIn("203.0.113.7", json.dumps(report))

    def test_loopback_and_unspecified_addresses_are_not_flagged(self) -> None:
        report = self.scan_line("The adapter binds 127.0.0.1 and never 0.0.0.0")
        self.assertEqual(
            [f for f in report["findings"] if f["rule"].startswith("HG-CRED")], []
        )

    def test_all_credential_findings_carry_no_value(self) -> None:
        secret = "hunter2hunter2hunter2"
        report = self.scan_line('secret = "{}" and host 10.1.2.3'.format(secret))
        for finding in report["findings"]:
            if not finding["rule"].startswith("HG-CRED"):
                continue
            self.assertIn("value_length", finding)
            self.assertIn("value_sha256_8", finding)
            self.assertIn("pattern_class", finding)
            self.assertNotIn(secret, finding["message"])
            self.assertNotIn(secret, finding["evidence"])


# ---------------------------------------------------------------------------
# Rule 5 - physical-test precision
# ---------------------------------------------------------------------------


class TestPhysicalPrecisionRules(FixtureCase):
    def test_phys001_phone_coupled_with_vpn_implying_a_test_is_a_review(self) -> None:
        root = self.tree(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Remote access\n\n"
                    "The Mi A3 phone-hosted MCP endpoint works over the VPN tunnel.\n"
                )
            }
        )
        report = scan_json(root)["report"]
        finding = self.findings_for(report, "HG-PHYS-001")[0]
        self.assertEqual(finding["severity"], "REVIEW")
        self.assertIn("physical", finding["message"])

    def test_phys001_negated_phone_statement_is_not_flagged(self) -> None:
        root = self.tree(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Remote access\n\nNo phone-hosted MCP endpoint is reachable over "
                    "the VPN; nothing was tested on the phone.\n"
                )
            }
        )
        report = scan_json(root)["report"]
        self.assertEqual(self.findings_for(report, "HG-PHYS-001"), [])

    def test_phys002_physical_camera_claim_is_reported_separately(self) -> None:
        root = self.tree(
            {
                "docs/DEVPOST_SUBMISSION.md": (
                    "# Capture\n\nThe physical camera capture was verified on real hardware.\n"
                ),
                "docs/PROOF_REGISTER.md": (
                    "# Register\n\nThe physical device verification passed for the "
                    "Wi-Fi ingress path.\n"
                ),
            }
        )
        report = scan_json(root)["report"]
        camera = self.findings_for(report, "HG-PHYS-002")
        self.assertEqual(len(camera), 1, report["findings"])
        self.assertIn("camera", camera[0]["evidence"])
        # The device statement must not be reported as a camera-capture claim.
        self.assertNotIn("device verification", camera[0]["evidence"])


# ---------------------------------------------------------------------------
# CLI behaviour: exit codes, JSON parity, --severity-min, --paths
# ---------------------------------------------------------------------------


class TestCliBehaviour(FixtureCase):
    def test_exit_code_2_for_review_only(self) -> None:
        files = dict(CLEAN_FILES)
        files["package.json"] = json.dumps({"name": "fixture-project", "version": "1.2.3"})
        root = self.tree(files)
        code, _out, _err = run_cli(str(root))
        self.assertEqual(code, csc.EXIT_REVIEW)

    def test_exit_code_0_when_clean(self) -> None:
        root = self.tree(CLEAN_FILES)
        code, _out, _err = run_cli(str(root), "--quiet")
        self.assertEqual(code, csc.EXIT_OK)
        self.assertEqual(_out, "")

    def test_json_and_text_reports_agree_on_findings(self) -> None:
        root = self.tree(CONTRADICTION_FILES)
        _code, out_text, _ = run_cli(str(root))
        _code2, out_json, _ = run_cli(str(root), "--json")
        report = json.loads(out_json)
        for finding in report["findings"]:
            self.assertIn(finding["rule"], out_text)
            self.assertIn(finding["file"], out_text)
        self.assertIn("FAIL={}".format(report["counts"]["FAIL"]), out_text)

    def test_severity_min_fail_filters_reviews(self) -> None:
        files = dict(CLEAN_FILES)
        files["package.json"] = json.dumps({"name": "fixture-project", "version": "1.2.3"})
        files["docs/DEVPOST_SUBMISSION.md"] = (
            "# Claims\n\nRemote MCP reachability is implemented and verified.\n\n"
            "Remote MCP reachability is not verified.\n"
        )
        root = self.tree(files)
        _code, out, _ = run_cli(str(root), "--json", "--severity-min", "FAIL")
        report = json.loads(out)
        self.assertTrue(report["findings"])
        self.assertTrue(all(f["severity"] == "FAIL" for f in report["findings"]))
        self.assertEqual(report["counts"]["REVIEW"], 0)

    def test_paths_option_restricts_the_scan(self) -> None:
        files = dict(CLEAN_FILES)
        files["docs/DEVPOST_SUBMISSION.md"] = (
            "# Claims\n\nRemote MCP reachability is implemented and verified.\n"
        )
        root = self.tree(files)
        quoted = Path("docs/PROOF_REGISTER.md")
        _code, out, _ = run_cli(str(root), "--json", "--paths", str(quoted))
        report = json.loads(out)
        self.assertEqual(report["stats"]["files_scanned"], 1)
        self.assertEqual(
            [f for f in report["findings"] if f["rule"].startswith("HG-CLAIM")], []
        )

    def test_missing_tree_is_an_internal_error(self) -> None:
        code, _out, err = run_cli(str(self.tmp / "does-not-exist"))
        self.assertEqual(code, csc.EXIT_INTERNAL)
        self.assertIn("not a directory", err)

    def test_scanner_is_offline_and_read_only(self) -> None:
        """No socket use, no writes: the fixture tree is byte-identical after."""
        root = self.tree(CLEAN_FILES)
        before = {p: p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}
        run_cli(str(root), "--json")
        after = {p: p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}
        self.assertEqual(before, after)
        source = (Path(csc.__file__)).read_text(encoding="utf-8")
        for banned in ("socket", "urllib.request", "http.client", "requests"):
            self.assertNotIn(banned, source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
