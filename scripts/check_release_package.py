#!/usr/bin/env python3
"""check_release_package.py - offline audit of a PRODUCED release package.

Purpose
-------
``scripts/check_submission_claims.py`` audits the *working tree* before a
submission is published.  This checker audits the artefact that is actually
handed to a judge: the release package produced by ``scripts/package_release.sh``
(the ``*-source.tar.gz`` archive, or the output directory that holds it together
with its ``.sha256`` / ``.sha512`` / ``-manifest.txt`` companions).

The six checks
--------------
 1. LICENCE AND NOTICE MATERIAL IS PRESENT.  ``LICENSE`` and ``NOTICE`` must be
    in the package (the packaging script declares both mandatory) and the
    licence *text* must be the licence selected with ``--expected-license``.
    The third-party attribution material the packaging is supposed to carry is
    ``NOTICE`` itself; a NOTICE that carries no attribution content is REVIEW.
 2. THE DECLARED MANDATORY FILES ARE PRESENT.  The mandatory list is read, at
    run time, out of ``scripts/package_release.sh`` (``MANDATORY_FILES=(...)``)
    and used verbatim; a documented fallback copy of the same list is used only
    when that script is not reachable (and that is reported).  On top of the
    mandatory list, the entries the judge-release harness consumes but the
    packaging does not declare mandatory are reported as REVIEW.
 3. MANIFEST LICENCE VALUES MATCH THE SELECTED POLICY.  Same semantics as the
    submission scanner: a manifest with no licence field is REVIEW, a manifest
    whose licence conflicts with the selected licence is FAIL, a match is
    silent.  This is implemented by CALLING the scanner's ``scan_licensing()``
    rather than by re-implementing it.  The licence value the release manifest
    records is compared too.
 4. NO UNEXPECTED SECRETS OR BUILD OUTPUTS ARE PACKAGED (the highest-value
    check).  Credential-shaped files, credential-shaped values inside packaged
    files, build output/intermediates, local state and captured media.  Matched
    values are NEVER printed: only the pattern class, the value length and the
    first 8 hex of its SHA-256 are reported, and every finding is masked before
    the scanner's own redaction guard is run over it as a final assertion.
    Credential findings are never suppressed by a heuristic; the only mechanism
    that may downgrade one is the reviewed allow-list documented below.
 5. THE PACKAGE RECORDS ITS SOURCE REVISION.  Verifies the recorded revision
    (release manifest fields, revision files inside the package, the archive /
    package-directory name) and, when a repository is available, corroborates
    that the recorded commit exists.  Recorded digests are verified against the
    archive they describe.
 6. RUNTIME DEPENDENCY ATTRIBUTION COVERAGE.  The runtime dependencies the
    package resolves - every non-dev entry of every ``package-lock.json`` /
    ``npm-shrinkwrap.json`` in the package, plus the vendored and FetchContent
    C++ components the build actually uses - are cross-checked against the
    attribution record (``NOTICE`` and the third-party notice/licence material
    the package carries).  A runtime dependency present in the lockfile and
    absent from the attribution record is reported (``HG-PKG-DEP-002``,
    aggregated per licence).  MPL-2.0 is treated as a PER-PACKAGE licence: a
    component that resolves under MPL-2.0 must be identified by name, so
    ``mediabunny`` missing from the record is FAIL (``HG-PKG-DEP-001``) rather
    than merely "some transitive packages are covered" - see
    ``PER_PACKAGE_REQUIRED_LICENCES`` for the reasons, and
    ``check_dependency_attribution()`` for the full severity model.

Precedence rule for severity (deliberate, and the reason the tiers are stable)
-----------------------------------------------------------------------------
A packaged entry that the packaging script's OWN exclusion list forbids is FAIL:
if it is in the artefact, the packaging gate was bypassed or the rules changed.
Everything else in the same family (build output, local state, captured media)
is REVIEW: unexpected, but not a disclosure.  See ``PACKAGING_FORBIDDEN_FALLBACK``.

Reviewed secret allow-list (synthetic credential fixtures)
-----------------------------------------------------------
Some credential-shaped literals genuinely ship inside a release *because they
are test fixtures*: a negative-test password in a JavaScript test, a local
secret in a C++ unit test, a key-material literal a test decodes.  The checker
cannot prove from an artefact that such a literal is a fixture, so it leaves the
finding FAIL.  This checker therefore carries **no heuristic that suppresses a
credential finding** - a heuristic that hides one real secret is worse than a
false positive.

What it carries instead is a REVIEWED ALLOW-LIST: a checked-in JSON file that
names an individual finding by all four of

  * the package-relative file,
  * the line number,
  * the credential pattern class (``wireguard-key``, ``secret-assignment``, ...),
    and
  * the first 8 hex of the SHA-256 of the matched *value*,

and records why the value is a reviewed fixture.  The value itself is never
written into the allow-list and never leaves the tool: the truncated digest is
what identifies it, exactly as in every other credential finding.

An allow-listed finding is downgraded FAIL -> REVIEW and its message says it is
allow-listed for review: the finding stays visible and is never suppressed.
Because the entry is keyed by the value's digest, editing the value at that
location - which is what a real leak looks like - returns the finding to FAIL.
The allow-list can only cover the one value that was reviewed; it cannot hide a
different one, and it cannot be widened to a path, a class or a line range.
With no allow-list present the behaviour is the unchanged, un-suppressed one.

Resolution order:
  1. ``--secret-allowlist FILE`` on the CLI, or ``HG_SECRET_ALLOWLIST`` for the
     importable API (the CLI flag wins);
  2. ``scripts/release-secret-allowlist.json`` beside this script, when it
     exists.  The repository ships no such file, so by default nothing is
     allow-listed.
A missing, unreadable or malformed allow-list suppresses nothing (the safe
direction) and is reported in the notes.

Allow-list format (JSON):
  {
    "format": 1,
    "entries": [
      {
        "file": "web_mcp/tests/auth.test.js",
        "line": 42,
        "pattern_class": "secret-assignment",
        "value_sha256_8": "9fdc99be",
        "reason": "negative-test password asserted by this test; reviewed"
      }
    ]
  }

A reviewer adds an entry by taking ``file``, ``line``, ``pattern_class`` and
``value_sha256_8`` straight out of the finding (all four are in the text report
and in ``--json``), reading that line to confirm the value is a fixture, and
committing the entry - so the downgrade is a reviewed diff of data rather than a
change to the rules.

Redaction discipline
--------------------
Reused from the submission scanner: ``SECRET_PATTERNS``, the placeholder filter,
``_sha256_8`` and ``enforce_redaction``.  This checker additionally MASKS every
matched value out of every finding before serialising, and then runs the
scanner's guard over the masked result, so a leak is refused twice over rather
than once.  ``enforce_redaction`` tripping is exit code 3.

Rule ids
--------
Reused where the semantics are reused (``HG-LIC-010``/``HG-LIC-011`` for
manifest licence fields).  Package-specific rules are namespaced ``HG-PKG-*``:
``LIC`` licence material, ``SRC`` mandatory/judge-release source entries,
``LEAK`` packaged secrets and build output, ``REV`` source revision, ``DEP``
runtime dependency attribution.

Exit codes (same as the submission scanner)
-------------------------------------------
  0  no FAIL and no REVIEW findings
  1  at least one FAIL finding
  2  at least one REVIEW finding and no FAIL
  3  internal error (redaction guard tripped, unreadable input, missing scanner)

Usage
-----
  python3 scripts/check_release_package.py PATH [--json]
        [--expected-license Apache-2.0|MIT] [--severity-min FAIL|REVIEW]
        [--package-script FILE] [--repo DIR] [--include-topology]
        [--max-scan-bytes N] [--quiet]

For another generator (stable interface, no printing, no side effects)
---------------------------------------------------------------------
  import sys
  sys.path.insert(0, "<repo>/scripts")
  from check_release_package import audit_package, exit_code_for

  result = audit_package("/path/to/homeguardian-ai-1.0.0-...-source.tar.gz")
  code = exit_code_for(result)          # 0 / 1 / 2, same contract as the CLI

``audit_package(package_path, *, expected_licence=None, package_script=None,
repo=None, include_topology=False, max_scan_bytes=2097152,
severity_min="REVIEW") -> dict`` accepts a directory (the packaging output
directory, or an extracted package tree) or a tarball.  It prints nothing,
writes nothing outside a private temporary directory, never calls ``sys.exit``,
and raises ``PackageInputError`` (bad input path) or ``ScannerUnavailable``
(the sibling scanner is missing) instead of exiting.  ``run_audit()`` returns
the same information as a ``Report`` object, and ``exit_code_for()`` maps a
result dict to the exit code.  The CLI is a thin wrapper over these.

JSON schema of the returned dict (stable; every finding object is also produced
by ``check_submission_claims.Finding.to_dict()``, extended with ``check``):

  {
    "program": "check_release_package",       # str, constant
    "input": "<path as given>",               # str
    "origin": "tarball" | "directory",        # str
    "package_root": "<abs path>",             # str, the extracted/authored tree
    "prefix": "<archive top-level dir>"|null, # str|null
    "expected_licence": "Apache-2.0",         # str, the selected policy
    "packaging_rules": {                      # where the rules came from
        "source": "<path to package_release.sh or fallback marker>",
        "mandatory_count": 46,                # int
        "forbidden": ["*.db", ...]            # list[str]
    },
    "check_names": {"1": "...", ..., "6": "..."},
    "checks": {                               # per-check roll-up, keys "1".."6"
        "1": {"name": str, "status": "PASS"|"REVIEW"|"FAIL", "findings": int},
        ...
    },
    "counts": {"FAIL": int, "REVIEW": int},   # severity totals
    "total": int,                             # len(findings)
    "findings": [                             # sorted FAIL-first, then rule id
        {
          "rule": "HG-PKG-LEAK-003",          # str, rule id
          "severity": "FAIL"|"REVIEW",        # str
          "file": "<path relative to the package>",
          "line": 41,                         # int, 0 when not line-based
          "message": str,                     # never contains a matched secret
          "evidence": str,                    # never contains a matched secret
          "correction": str,                  # what to change
          "pattern_class": str,               # credential findings only
          "value_length": int,                # credential findings only
          "value_sha256_8": str,              # credential findings only
          "check": 4                          # int, owning check (6 checks)
        }
    ],
    "notes": [str, ...],                      # informational, no finding weight
    "stats": {                                # per-check detail
        "archive": str|null, "archive_entries": int,
        "release_manifest": str|null,
        "packaging": {...}, "revision": {...}, "dependencies": {...},
        "secret_allowlist": {                 # reviewed allow-list of fixtures
            "path": str|null,                 # null when none was resolved
            "format": 1,                      # int
            "entries": int,                   # reviewed entries loaded
            "applied": int                    # findings downgraded FAIL->REVIEW
        }
    }
  }

History and licensing are NOT touched by this script: it is a read-only checker
for an already-produced artefact.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence

PROGRAM = "check_release_package"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_REVIEW = 2
EXIT_INTERNAL = 3

SEV_FAIL = "FAIL"
SEV_REVIEW = "REVIEW"
SEVERITIES = (SEV_FAIL, SEV_REVIEW)

CHECK_NAMES = {
    1: "licence-and-notice",
    2: "mandatory-files",
    3: "manifest-licence-policy",
    4: "secrets-and-build-output",
    5: "source-revision-recorded",
    6: "dependency-attribution-coverage",
}

#: rule id -> the check that owns it.  Anything not listed is reported under
#: check 0 ("unclassified") so a future rule can never be silently dropped.
RULE_CHECK = {
    # reused from check_submission_claims.scan_licensing()
    "HG-LIC-001": 1,  # LICENSE text is not the expected licence      (FAIL)
    "HG-LIC-002": 1,  # NOTICE absent                                 (REVIEW)
    "HG-LIC-003": 1,  # NOTICE does not state the project licence     (REVIEW)
    "HG-LIC-010": 3,  # manifest declares no licence field            (REVIEW)
    "HG-LIC-011": 3,  # manifest licence conflicts with the policy    (FAIL)
    # package-specific
    "HG-PKG-LIC-001": 1,
    "HG-PKG-LIC-002": 1,
    "HG-PKG-LIC-004": 1,
    "HG-PKG-LIC-020": 3,
    "HG-PKG-LIC-021": 3,
    "HG-PKG-SRC-001": 2,
    "HG-PKG-SRC-010": 2,
    "HG-PKG-SRC-030": 2,
    "HG-PKG-LEAK-001": 4,
    "HG-PKG-LEAK-002": 4,
    "HG-PKG-LEAK-003": 4,
    "HG-PKG-LEAK-004": 4,
    "HG-PKG-LEAK-005": 4,
    "HG-PKG-LEAK-006": 4,
    "HG-PKG-LEAK-007": 4,
    "HG-PKG-REV-001": 5,
    "HG-PKG-REV-002": 5,
    "HG-PKG-REV-003": 5,
    "HG-PKG-REV-004": 5,
    # check 6 - runtime dependency attribution coverage
    "HG-PKG-DEP-001": 6,
    "HG-PKG-DEP-002": 6,
    "HG-PKG-DEP-003": 6,
    "HG-PKG-DEP-004": 6,
    "HG-PKG-DEP-005": 6,
    "HG-PKG-DEP-006": 6,
    "HG-PKG-DEP-007": 6,
    "HG-PKG-DEP-008": 6,
}

# --------------------------------------------------------------------------
# Reuse of the submission scanner (mandatory: no duplicated semantics)
# --------------------------------------------------------------------------

def _import_scanner():
    """Import ``check_submission_claims``, tolerating a script sitting beside us."""
    try:
        import check_submission_claims as module  # type: ignore
        return module
    except ImportError:
        here = Path(__file__).resolve().parent
        if str(here) not in sys.path:
            sys.path.insert(0, str(here))
        import check_submission_claims as module  # type: ignore
        return module


try:
    csc = _import_scanner()
except Exception as _exc:  # pragma: no cover - exercised via main()
    csc = None
    _CSC_ERROR = _exc
else:
    _CSC_ERROR = None

Finding = csc.Finding if csc is not None else None  # type: ignore


class ScannerUnavailable(RuntimeError):
    """The sibling submission scanner could not be imported."""


def _require():
    if csc is None:
        raise ScannerUnavailable(
            "check_submission_claims.py is required (it is imported, not "
            "duplicated, for the licence and credential semantics): {}".format(
                _CSC_ERROR
            )
        )
    return csc


def _finding(rule: str, severity: str, file: str, line: int, message: str,
             evidence: str, correction: str, klass: str | None = None,
             value_length: int | None = None,
             value_sha256_8: str | None = None):
    return _require().Finding(
        rule=rule,
        severity=severity,
        file=file,
        line=line,
        message=message,
        evidence=evidence,
        correction=correction,
        klass=klass,
        value_length=value_length,
        value_sha256_8=value_sha256_8,
    )


def _sha256_8(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", "replace")).hexdigest()[:8]


def _snippet(text: str, width: int = 120) -> str:
    flat = " ".join((text or "").split())
    if len(flat) <= width:
        return flat
    return flat[: width - 1] + "\u2026"


def _mask(text: str, values: Sequence[str]) -> str:
    """Blank every matched sensitive value out of a string.

    Runs before serialisation; the scanner's ``enforce_redaction`` is then run
    as the assertion that nothing slipped through.
    """
    out = text or ""
    for value in values:
        if value and len(value) >= 4 and value in out:
            out = out.replace(value, "[redacted]")
    return out


def _sanitize(findings: Iterable, values: Sequence[str]) -> list:
    """Return findings with every sensitive value masked out of their text."""
    if not values:
        return list(findings)
    clean = []
    for f in findings:
        clean.append(
            f.__class__(
                rule=f.rule,
                severity=f.severity,
                file=_mask(f.file, values),
                line=f.line,
                message=_mask(f.message, values),
                evidence=_mask(f.evidence, values),
                correction=_mask(f.correction, values),
                klass=_mask(f.klass, values) if f.klass else f.klass,
                value_length=f.value_length,
                value_sha256_8=f.value_sha256_8,
            )
        )
    return clean


# --------------------------------------------------------------------------
# Reviewed secret allow-list (synthetic credential fixtures)
# --------------------------------------------------------------------------

#: Environment variable naming the allow-list for the importable API; the CLI's
#: ``--secret-allowlist`` takes precedence over it.
ENV_SECRET_ALLOWLIST = "HG_SECRET_ALLOWLIST"

#: Where a checked-in allow-list lives, beside this script.  The repository ships
#: no such file, so the default audit is the un-suppressed one.
SECRET_ALLOWLIST_NAME = "release-secret-allowlist.json"

SECRET_ALLOWLIST_FORMAT = 1

#: An entry matches a finding only when ALL FOUR of these agree.  Keying on the
#: matched value's ``sha256[:8]`` is what stops an entry from covering a
#: *different* secret at the same file, line and pattern class.
ALLOWLIST_KEYS = ("file", "line", "pattern_class", "value_sha256_8")


def resolve_secret_allowlist(explicit=None):
    """Where the allow-list comes from: the argument, then the env, then the repo."""
    if explicit:
        return Path(explicit)
    env = os.environ.get(ENV_SECRET_ALLOWLIST)
    if env:
        return Path(env)
    beside = Path(__file__).resolve().parent / SECRET_ALLOWLIST_NAME
    return beside if beside.is_file() else None


def load_secret_allowlist(path) -> tuple[dict, list[str]]:
    """Read a reviewed secret allow-list.  Returns (keys, notes).

    ``keys`` maps ``(file, line, pattern_class, value_sha256_8)`` to the
    reviewer's reason.  A missing, unreadable or malformed allow-list yields no
    keys and a note: the safe direction is that every credential-shaped value
    stays FAIL, so a broken allow-list can never widen what is suppressed.
    """
    notes: list[str] = []
    if path is None:
        return {}, notes
    path = Path(path)
    if not path.is_file():
        notes.append(
            "no secret allow-list at {} (an entry names a reviewed synthetic "
            "credential fixture by file, line, pattern class and value "
            "sha256[:8]); nothing is allow-listed".format(path)
        )
        return {}, notes
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError) as exc:
        notes.append(
            "the secret allow-list {} could not be read ({}); no credential "
            "finding is allow-listed and every credential-shaped value stays "
            "FAIL".format(path, type(exc).__name__)
        )
        return {}, notes
    entries = data.get("entries") if isinstance(data, dict) else None
    if not isinstance(entries, list):
        notes.append(
            "the secret allow-list {} carries no 'entries' list; no credential "
            "finding is allow-listed".format(path)
        )
        return {}, notes

    keys: dict = {}
    malformed = 0
    for entry in entries:
        if not isinstance(entry, dict) or any(key not in entry
                                              for key in ALLOWLIST_KEYS):
            malformed += 1
            continue
        try:
            line = int(entry["line"])
        except (TypeError, ValueError):
            malformed += 1
            continue
        keys[(str(entry["file"]), line, str(entry["pattern_class"]),
              str(entry["value_sha256_8"]).lower())] = str(
                  entry.get("reason") or "")
    notes.append(
        "secret allow-list {}: {} reviewed entr{} loaded{}; an allow-listed "
        "finding is downgraded to REVIEW (never suppressed), and a changed "
        "value at the same file/line/pattern class returns to FAIL".format(
            path, len(keys), "y" if len(keys) == 1 else "ies",
            "" if keys else " (nothing is allow-listed)",
        )
    )
    if malformed:
        notes.append(
            "{} allow-list entr{} were malformed (each needs {}, and an integer "
            "line) and were ignored, so any finding they describe stays "
            "FAIL".format(malformed, "y" if malformed == 1 else "ies",
                          ", ".join(ALLOWLIST_KEYS))
        )
    return keys, notes


def apply_secret_allowlist(findings, keys) -> tuple[list, int]:
    """Downgrade FAIL -> REVIEW for the credential findings ``keys`` names.

    Returns (findings, applied).  Only ``HG-PKG-LEAK-003`` FAIL findings are
    eligible, and only when the entry's file, line, pattern class AND value
    ``sha256[:8]`` all agree.  The finding keeps its class, length and truncated
    digest, so the value is still never printed; only the severity and the
    message change.  A value changed at that location does not match the entry
    and stays FAIL.
    """
    if not keys:
        return list(findings), 0
    out = []
    applied = 0
    for finding in findings:
        key = (finding.file, finding.line, finding.klass or "",
               (finding.value_sha256_8 or "").lower())
        if (finding.rule == "HG-PKG-LEAK-003" and finding.severity == SEV_FAIL
                and key in keys):
            applied += 1
            reason = keys[key]
            out.append(finding.__class__(
                rule=finding.rule,
                severity=SEV_REVIEW,
                file=finding.file,
                line=finding.line,
                message=(
                    "{}; ALLOW-LISTED FOR REVIEW: this value, at this file, "
                    "line and pattern class, is recorded in the reviewed secret "
                    "allow-list as a synthetic fixture ({}), so this finding is "
                    "reported as REVIEW rather than FAIL. It is not suppressed: "
                    "a different value here - which is what a real leak looks "
                    "like - is not covered by the entry and returns to "
                    "FAIL.".format(finding.message, reason or "no reason recorded")
                ),
                evidence=finding.evidence,
                correction=(
                    "If the value at {}:{} changes, re-run this checker, confirm "
                    "the new value is intended, and re-record it in the "
                    "allow-list as a reviewed change; do not widen an entry to a "
                    "path, a pattern class or a line range.".format(
                        finding.file, finding.line)
                ),
                klass=finding.klass,
                value_length=finding.value_length,
                value_sha256_8=finding.value_sha256_8,
            ))
        else:
            out.append(finding)
    return out, applied


# --------------------------------------------------------------------------
# Packaging rules, read out of package_release.sh (verbatim reuse)
# --------------------------------------------------------------------------

#: Fallback copy of ``MANDATORY_FILES=(...)`` in scripts/package_release.sh.
#: Used only when that script is not reachable, and reported in the notes when
#: it is used.  ``--package-script`` points the checker at the real file.
MANDATORY_FILES_FALLBACK = (
    "README.md",
    "LICENSE",
    "NOTICE",
    "CMakeLists.txt",
    "cmake/hg_web_assets.h.in",
    "src/core/AlexaContract.h",
    "src/core/AlexaCredentialStore.cpp",
    "src/mcp/McpHttpServer.cpp",
    "src/mcp/McpProtocol.cpp",
    "src/net/DeviceRegistry.cpp",
    "src/bridge/hg_alexa_bridge.cpp",
    "src/bridge/NativeHttp.h",
    "config/homeguardian.demo.json",
    "config/homeguardian.json",
    "web_mcp/server.js",
    "web_mcp/auth.js",
    "web_mcp/webrtc_streamer.js",
    "web_mcp/device_adapter.js",
    "web_mcp/mcp_adapter.js",
    "web_mcp/public/index.html",
    "deploy/docker/Dockerfile",
    "deploy/docker/docker-compose.yml",
    "deploy/docker/entrypoint.sh",
    "docs/INDEX.md",
    "docs/ARCHITECTURE.md",
    "docs/DECISIONS.md",
    "docs/DEMO_WEB.md",
    "docs/DEVELOPMENT.md",
    "docs/DISTRIBUTED_DEPLOYMENT.md",
    "docs/SECURITY_READINESS_V8.md",
    "docs/RELEASE_CANDIDATE_V21.md",
    "docs/DEPLOYMENT_UBUNTU_DOCKER.md",
    "docs/ORACLE_VPS_DEPLOYMENT_PLAN.md",
    "docs/RELEASE_AUDIT_STATUS.md",
    "docs/DEVPOST_DEMONSTRATION_GUIDE.md",
    "docs/SUBMISSION_PACKAGE.md",
    "docs/SYNC_REPORT_GDRIVE_01_05.md",
    "docs/SUBMISSION_VIDEO_SCRIPT.md",
    "docs/DEVPOST_SUBMISSION.md",
    "docs/PROOF_REGISTER.md",
    "scripts/verify_synthetic_mcp_demo.sh",
    "scripts/test_hg_sec_01_independent.sh",
    "scripts/bench_demo.sh",
    "scripts/verify_retention.sh",
    "scripts/verify_mcp.sh",
    "scripts/package_release.sh",
)

#: Fallback copy of the packaging script's own leak-check list (step 4).
PACKAGING_FORBIDDEN_FALLBACK = (
    ".git",
    "*.db",
    "*.log",
    "node_modules",
    "*.secret",
    "id_rsa*",
    "*.key",
)

#: Entries the judge-release harness (scripts/verify_judge_release.sh) consumes
#: that the packaging script does NOT declare mandatory.  Derivation is named in
#: the message; these are REVIEW, never FAIL, because the packaging's own gate
#: is authoritative for what the package must carry.
JUDGE_RELEASE_ENTRIES = (
    ("deploy/coordinator/service.js",
     "verify_judge_release.sh COORDINATOR_SCRIPT (outbound bridge coordinator)"),
    ("scripts/verify_judge_release.sh",
     "the judge-release verification harness itself"),
)

#: Bundled third-party directories NOTICE may name; NOTICE claiming a bundled
#: component the package does not carry is a REVIEW (attribution vs artefact).
NOTICE_BUNDLED_PATH_RE = re.compile(r"\bthird_party/[A-Za-z0-9._/-]+")


@dataclass
class PackagingRules:
    mandatory: tuple[str, ...]
    forbidden: tuple[str, ...]
    source: str
    notes: list[str] = field(default_factory=list)


MANDATORY_BLOCK_RE = re.compile(
    r"^[ \t]*(?:declare[ \t]+-a[ \t]+)?MANDATORY_FILES[ \t]*=\((?P<body>.*?)^[ \t]*\)[ \t]*$",
    re.S | re.M,
)
QUOTED_RE = re.compile(r"""["']([^"']+)["']""")
FORBIDDEN_BLOCK_RE = re.compile(
    r"(?:forbidden artifacts|LEAK_COUNT=0)(?P<body>.*?)(?:MANIFEST|^echo )",
    re.S | re.M,
)
NAME_TOKEN_RE = re.compile(r"""-name[ \t]+["']([^"']+)["']""")


def default_package_script() -> Path | None:
    """Locate scripts/package_release.sh: beside this script, then under cwd."""
    beside = Path(__file__).resolve().parent / "package_release.sh"
    if beside.is_file():
        return beside
    cwd = Path.cwd() / "scripts" / "package_release.sh"
    if cwd.is_file():
        return cwd
    return None


def load_packaging_rules(script: Path | None) -> PackagingRules:
    """Read the mandatory/forbidden lists out of the packaging script."""
    if script is None or not script.is_file():
        return PackagingRules(
            mandatory=MANDATORY_FILES_FALLBACK,
            forbidden=PACKAGING_FORBIDDEN_FALLBACK,
            source="<fallback constants in check_release_package.py>",
            notes=[
                "scripts/package_release.sh was not reachable, so the documented "
                "fallback copies of MANDATORY_FILES and the leak-check list were "
                "used; pass --package-script to read the real ones"
            ],
        )
    try:
        text = script.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - unreadable script
        return PackagingRules(
            mandatory=MANDATORY_FILES_FALLBACK,
            forbidden=PACKAGING_FORBIDDEN_FALLBACK,
            source="<fallback constants in check_release_package.py>",
            notes=["could not read {}: {}".format(script, exc)],
        )

    notes: list[str] = []
    match = MANDATORY_BLOCK_RE.search(text)
    mandatory: tuple[str, ...] = MANDATORY_FILES_FALLBACK
    if match:
        body = "\n".join(
            line for line in match.group("body").splitlines()
            if not line.lstrip().startswith("#")
        )
        entries = tuple(QUOTED_RE.findall(body))
        if entries:
            mandatory = entries
        else:
            notes.append("MANDATORY_FILES block in {} parsed to zero entries; "
                         "fallback list used".format(script))
    else:
        notes.append("no MANDATORY_FILES block found in {}; fallback list "
                     "used".format(script))

    forbidden: tuple[str, ...] = PACKAGING_FORBIDDEN_FALLBACK
    block = FORBIDDEN_BLOCK_RE.search(text)
    if block:
        tokens = tuple(NAME_TOKEN_RE.findall(block.group("body")))
        if tokens:
            forbidden = tokens
        else:
            notes.append("the packaging leak-check list in {} parsed to zero "
                         "entries; fallback list used".format(script))
    else:
        notes.append("no packaging leak-check block found in {}; fallback list "
                     "used".format(script))

    return PackagingRules(
        mandatory=mandatory,
        forbidden=forbidden,
        source=str(script),
        notes=notes,
    )


# --------------------------------------------------------------------------
# Package loading: a tarball, or a directory that holds one
# --------------------------------------------------------------------------

ARCHIVE_SUFFIXES = (".tar.gz", ".tgz", ".tar.bz2", ".tar.xz", ".tar")
CHECKSUM_SUFFIXES = (".sha256", ".sha512")
MANIFEST_SUFFIX = "-manifest.txt"

#: ``homeguardian-ai-1.0.0-sec-<10 hex>-prep-<10 hex>-source.tar.gz``
PACKAGE_NAME_RE = re.compile(
    r"(?P<slug>[A-Za-z0-9._-]+?)-sec-(?P<sec>[0-9a-f]{7,40})-prep-(?P<prep>[0-9a-f]{7,40})"
    r"(?:-source)?(?P<ext>\.tar\.gz|\.tgz|\.tar\.bz2|\.tar\.xz|\.tar)?$"
)
FULL_SHA_RE = re.compile(r"\b[0-9a-f]{40}\b")
LABELLED_SHA_RE = re.compile(
    r"^[ \t]*(?P<label>[A-Za-z][A-Za-z0-9 /_-]*?(?:SHA|REVISION|COMMIT|REV)[A-Za-z0-9 /_-]*?)"
    r"[ \t]*:[ \t]*(?P<sha>[0-9a-f]{7,40})\b",
    re.M,
)


class PackageInputError(RuntimeError):
    """The input path is not a release package this checker can audit."""


@dataclass
class Package:
    root: Path | None                 # directory holding the packaged tree
    prefix: str | None                # the archive's single top-level directory
    origin: str                       # "tarball" | "directory"
    input_path: Path
    archive: Path | None = None
    siblings: dict = field(default_factory=dict)   # suffix -> Path
    release_manifest: Path | None = None
    checksums: dict = field(default_factory=dict)  # "sha256"/"sha512" -> Path
    name: str = ""                    # package/archive name used for revision
    cleanup: list = field(default_factory=list)
    named_entries: int = 0
    skipped_links: int = 0

    def path(self, rel: str) -> Path | None:
        if self.root is None:
            return None
        return self.root / rel

    def has(self, rel: str) -> bool:
        p = self.path(rel)
        return bool(p is not None and p.exists())


def _safe_members(tf: tarfile.TarFile) -> tuple[list, int]:
    """Return (extractable regular members, skipped non-regular count)."""
    members = []
    skipped = 0
    for member in tf.getmembers():
        name = member.name
        pure = PurePosixPath(name)
        if pure.is_absolute() or ".." in pure.parts:
            raise PackageInputError(
                "archive member escapes the extraction root: {!r}".format(name)
            )
        if member.issym() or member.islnk() or member.isdev() or member.isfifo():
            skipped += 1
            continue
        if member.isdir() or member.isreg():
            members.append(member)
    return members, skipped


def _extract_archive(archive: Path, workdir: Path) -> tuple[Path, str | None, int, int]:
    """Safely extract a package archive; return (root, prefix, files, links)."""
    with tarfile.open(archive, "r:*") as tf:
        members, skipped = _safe_members(tf)
        total_bytes = sum(m.size for m in members if m.isreg())
        if total_bytes > 512 * 1024 * 1024:
            raise PackageInputError(
                "archive would extract more than 512 MiB; refusing"
            )
        if len(members) > 200000:
            raise PackageInputError("archive declares more than 200000 entries; refusing")
        tf.extractall(workdir, members=members, filter="data")
        names = [m.name for m in members]
    files = sum(1 for m in members if m.isreg())
    tops = {PurePosixPath(n).parts[0] for n in names if PurePosixPath(n).parts}
    prefix = tops.pop() if len(tops) == 1 else None
    root = workdir / prefix if prefix else workdir
    return root, prefix, files, skipped


def _latest_or_first(paths: Sequence[Path]) -> Path:
    return sorted(paths, key=lambda p: p.name)[0]


def load_package(path: Path, workdir: Path) -> tuple[Package, list[str]]:
    """Resolve the artefact under ``path`` into a :class:`Package`."""
    notes: list[str] = []
    if not path.exists():
        raise PackageInputError("{}: no such file or directory".format(path))

    if path.is_file():
        if not any(path.name.endswith(s) for s in ARCHIVE_SUFFIXES):
            raise PackageInputError(
                "{} is not a release archive ({})".format(
                    path, "|".join(s.lstrip(".") for s in ARCHIVE_SUFFIXES)
                )
            )
        return _package_from_archive(path, workdir, notes), notes

    if not path.is_dir():
        raise PackageInputError("{} is neither a file nor a directory".format(path))

    archives = sorted(
        p for p in path.iterdir()
        if p.is_file() and any(p.name.endswith(s) for s in ARCHIVE_SUFFIXES)
    )
    if archives:
        if len(archives) > 1:
            notes.append(
                "{} release archives found in {}; validating {} (pass the "
                "archive path explicitly to choose)".format(
                    len(archives), path, _latest_or_first(archives).name
                )
            )
        return _package_from_archive(_latest_or_first(archives), workdir, notes), notes

    # No archive: treat the directory as an extracted package tree.
    return Package(
        root=path,
        prefix=None,
        origin="directory",
        input_path=path,
        name=path.name,
    ), notes


def _package_from_archive(archive: Path, workdir: Path, notes: list[str]) -> Package:
    root, prefix, files, skipped = _extract_archive(archive, workdir)
    pkg = Package(
        root=root,
        prefix=prefix,
        origin="tarball",
        input_path=archive,
        archive=archive,
        name=archive.name,
        named_entries=files,
        skipped_links=skipped,
    )
    if prefix is None:
        notes.append(
            "archive has no single top-level directory, so the packaged root is "
            "the extraction root itself"
        )
    if skipped:
        notes.append(
            "{} archive member(s) were not regular files/directories and were "
            "not extracted".format(skipped)
        )
    parent = archive.resolve().parent
    stem = archive.name[: -len(next(s for s in ARCHIVE_SUFFIXES
                                     if archive.name.endswith(s)))]
    for suffix in CHECKSUM_SUFFIXES:
        candidate = parent / (archive.name + suffix)
        if candidate.is_file():
            pkg.checksums[suffix.lstrip(".")] = candidate
        pkg.siblings[suffix] = candidate
    # The packaging writes the manifest as "<PACKAGE_NAME>-manifest.txt", where
    # PACKAGE_NAME is the archive name WITHOUT its "-source" suffix.
    candidates = [parent / (stem + MANIFEST_SUFFIX)]
    if stem.endswith("-source"):
        candidates.append(parent / (stem[: -len("-source")] + MANIFEST_SUFFIX))
    found = next((c for c in candidates if c.is_file()), None)
    if found is None:
        globbed = sorted(parent.glob("*manifest*.txt"))
        if len(globbed) == 1:
            found = globbed[0]
            notes.append(
                "the release manifest was located by name pattern ({}) rather "
                "than by the packaging script's naming rule".format(found.name)
            )
    if found is not None:
        pkg.release_manifest = found
    return pkg


# --------------------------------------------------------------------------
# Walking the package
# --------------------------------------------------------------------------

PRUNE_DIR_NAMES = frozenset({
    "node_modules", ".git", "build", "dist", "out", "target", "coverage",
    "__pycache__", "CMakeFiles", ".venv", "venv", ".mypy_cache", ".pytest_cache",
    "cmake-build-debug", "cmake-build-release",
})

#: Credential-shaped FILE names (basename match).  Deliberately anchored: a file
#: called ``AlexaCredentialStore.cpp`` is source, not a credential.
CREDENTIAL_NAME_RES = (
    re.compile(r"^\.env$", re.I),
    re.compile(r"^\.env\.[^.]+$", re.I),
    re.compile(r"^id_(rsa|dsa|ecdsa|ed25519)", re.I),
    re.compile(r"\.(pem|key|p12|pfx|jks|keystore|bks|ppk|kdbx|ovpn|netrc|secret"
               r"|secrets|tfvars|p8|der|x509)$", re.I),
    re.compile(r"^keystore", re.I),
    re.compile(r"^\.(netrc|git-credentials|npmrc|pypirc|pgpass|my\.cnf)$", re.I),
    re.compile(r"^(credentials?|secrets?)(\.[A-Za-z0-9]+)?$", re.I),
    re.compile(r"\.tfstate(\.backup)?$", re.I),
    re.compile(r"^(authorized_keys|known_hosts)$", re.I),
)

#: Env files that are templates, not secrets: REVIEW rather than FAIL.
CREDENTIAL_TEMPLATE_RE = re.compile(
    r"^\.env\.(example|sample|template|dist|defaults?)$", re.I
)

BUILD_ARTIFACT_NAME_RES = (
    re.compile(r"\.(o|obj|a|so|dylib|dll|lib|exe|dSYM|pyc|pyo|class|jar|war)$", re.I),
    re.compile(r"\.(tar|tgz|tar\.gz|zip|7z|rar|whl|egg)$", re.I),
    re.compile(r"^(CMakeCache\.txt|compile_commands\.json|Makefile\.orig)$"),
    re.compile(r"\.(gcda|gcno|profraw|profdata)$", re.I),
)

LOCAL_STATE_NAME_RES = (
    re.compile(r"\.(db|sqlite|sqlite3|dat|mdb|ldb)$", re.I),
    re.compile(r"\.log(\.[0-9]+)?$", re.I),
    re.compile(r"^(core|core\.[0-9]+|\.DS_Store|Thumbs\.db)$"),
    re.compile(r"\.(swp|swo|bak|orig|rej|pid|tmp)$", re.I),
    re.compile(r"^hs_err_pid[0-9]+\.log$"),
)

CAPTURED_MEDIA_RES = (
    re.compile(r"\.(mp4|m4v|mov|avi|mkv|webm|h264|h265|y4m|yuv|raw|flv|mts)$", re.I),
    re.compile(r"\.(wav|aiff|flac|oga|opus|amr)$", re.I),
)

FRAME_DUMP_DIRS = frozenset({"frames", "frame", "captures", "captured", "snapshots",
                             "snapshot", "media", "samples", "recordings", "clips",
                             "dumps", "capture"})
FRAME_DUMP_NAME_RE = re.compile(
    r"(?:^|[_\-/])(?:frame|snap|snapshot|capture|shot|img|image|cam)[_\-]?[0-9]{1,6}\.",
    re.I,
)

#: Files whose name says they record a revision.
REVISION_FILE_RES = (
    re.compile(r"^(VERSION|REVISION|COMMIT|COMMIT_SHA|SOURCE_REVISION|GIT_SHA)"
               r"(\.[A-Za-z0-9]+)?$", re.I),
    re.compile(r"^\.git_archival\.txt$", re.I),
    re.compile(r"^revision\.(txt|json)$", re.I),
)


def _matches(name: str, patterns: Sequence[re.Pattern]) -> bool:
    return any(p.search(name) for p in patterns)


def _forbidden_hit(rel: str, forbidden: Sequence[str]) -> str | None:
    """Return the forbidden token that matches a package-relative path."""
    parts = rel.split("/")
    base = parts[-1]
    for token in forbidden:
        if any(ch in token for ch in "*?["):
            if fnmatch.fnmatch(base, token) or any(
                fnmatch.fnmatch(part, token) for part in parts[:-1]
            ):
                return token
        elif token == base or token in parts[:-1]:
            return token
    return None


def iter_package_entries(root: Path):
    """Yield (rel, is_dir, pruned) for every packaged entry, pruned at dirs.

    ``pruned`` is True for a directory the walker did not descend into, so a
    build output directory is reported once instead of once per file inside it.
    """
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        base = Path(dirpath)
        for name in list(dirnames):
            rel = (base / name).relative_to(root).as_posix()
            if name in PRUNE_DIR_NAMES or name.endswith("_build"):
                dirnames.remove(name)
                yield rel, True, True
            else:
                yield rel, True, False
        for name in filenames:
            rel = (base / name).relative_to(root).as_posix()
            yield rel, False, False


# --------------------------------------------------------------------------
# Check 1 + 3 - licence material and manifest licence policy (delegated)
# --------------------------------------------------------------------------

#: rule ids that belong to checks 1/3 and come from the reused scanner.
REUSED_LICENCE_RULES = (
    "HG-LIC-001", "HG-LIC-002", "HG-LIC-003",   # check 1
    "HG-LIC-010", "HG-LIC-011",                 # check 3
)


def scan_licence_material(pkg: Package, expected: str) -> tuple[list, list[str]]:
    """Run the scanner's own licence scan over the packaged tree.

    ``HG-LIC-001/002/003`` (LICENSE/NOTICE content) belong to check 1;
    ``HG-LIC-010/011`` (manifest licence fields) belong to check 3.  The
    scanner's judge-document sweep (``HG-LIC-020``) is deliberately NOT taken
    here: it belongs to the working-tree scan, and its evidence quotes whole
    document lines, which this checker keeps out of the packaged audit.
    """
    _require()
    notes: list[str] = []
    findings = csc.scan_licensing(pkg.root, csc.collect_targets(pkg.root), expected)
    kept = [f for f in findings if f.rule in REUSED_LICENCE_RULES]
    dropped = [f for f in findings if f.rule not in REUSED_LICENCE_RULES]
    if dropped:
        notes.append(
            "{} licence finding(s) of the working-tree doc sweep (rule {}) were "
            "not reported here; they belong to check_submission_claims.py".format(
                len(dropped), ",".join(sorted({f.rule for f in dropped}))
            )
        )
    # The scanner also reports a missing LICENSE (HG-LIC-001) / NOTICE
    # (HG-LIC-002); check 1 raises its own existence rules for the same files, so
    # the scanner's copy of those two is suppressed rather than double-reported.
    suppressed = 0
    deduped = []
    for f in kept:
        if (f.rule == "HG-LIC-001" and not pkg.has("LICENSE")) or (
            f.rule == "HG-LIC-002" and not pkg.has("NOTICE")
        ):
            suppressed += 1
            continue
        deduped.append(f)
    if suppressed:
        notes.append(
            "{} scanner finding(s) on a missing licence/notice file were folded "
            "into the equivalent HG-PKG-LIC-001/002 finding(s)".format(suppressed)
        )
    return deduped, notes


def check_licence_notice(pkg: Package) -> list:
    """Check 1 - the required licence and notice material exists."""
    findings = []

    if not pkg.has("LICENSE"):
        findings.append(_finding(
            "HG-PKG-LIC-001", SEV_FAIL, "LICENSE", 0,
            "the package carries no LICENSE file, so the licence of the shipped "
            "source cannot be verified",
            "<file absent from package>",
            "Re-run the packaging so LICENSE is included (the packaging script "
            "declares it mandatory); do not add a licence to the artefact by hand.",
        ))
    if not pkg.has("NOTICE"):
        findings.append(_finding(
            "HG-PKG-LIC-002", SEV_FAIL, "NOTICE", 0,
            "the package carries no NOTICE file, so the third-party attribution "
            "material the release is supposed to ship is missing",
            "<file absent from package>",
            "Re-run the packaging so NOTICE is included (the packaging script "
            "declares it mandatory).",
        ))
    else:
        text = (pkg.path("NOTICE") or Path(os.devnull)).read_text(
            encoding="utf-8", errors="replace"
        )
        _require()
        if not csc.ATTRIBUTION_MARKERS.search(text):
            findings.append(_finding(
                "HG-PKG-LIC-004", SEV_REVIEW, "NOTICE", 1,
                "NOTICE carries no third-party/attribution material, so the "
                "package ships no third-party notice content",
                _snippet(text.splitlines()[0] if text.splitlines() else ""),
                "List the bundled third-party components and their licences in "
                "NOTICE (the submission's NOTICE does exactly that).",
            ))
    return findings


RELEASE_MANIFEST_LICENCE_RE = re.compile(
    r"^[ \t]*(?:project[ \t]+)?licen[cs]e[ \t]*:[ \t]*(?P<value>.+?)[ \t]*$",
    re.I | re.M,
)


def check_manifest_licence_policy(pkg: Package, expected: str) -> tuple[list, list[str]]:
    """Check 3 - manifest licence values match the selected policy."""
    findings, notes = scan_licence_material(pkg, expected)

    if pkg.release_manifest is None:
        findings.append(_finding(
            "HG-PKG-LIC-021", SEV_REVIEW, "<release manifest>", 0,
            "no release manifest was found beside the package, so the licence "
            "value the packaging records for the release cannot be compared with "
            "the selected policy ({})".format(expected),
            "<no *-manifest.txt beside the archive>",
            "Validate the packaging output directory (the archive plus its "
            "*-manifest.txt) rather than the bare archive, or pass the manifest "
            "directory as the input.",
        ))
        return findings, notes

    text = pkg.release_manifest.read_text(encoding="utf-8", errors="replace")
    match = RELEASE_MANIFEST_LICENCE_RE.search(text)
    rel_name = pkg.release_manifest.name
    if match is None:
        findings.append(_finding(
            "HG-PKG-LIC-021", SEV_REVIEW, rel_name, 0,
            "the release manifest states no licence value, so the packaged "
            "licence claim cannot be compared with the selected policy ({})".format(
                expected
            ),
            "<no 'License:' field in the release manifest>",
            "Have the packaging record the licence in its manifest (it currently "
            "writes 'License: <name>'), then re-run this checker.",
        ))
        return findings, notes

    value = match.group("value").strip()
    _require()
    if not csc.licence_file_matches(value, expected):
        findings.append(_finding(
            "HG-PKG-LIC-020", SEV_FAIL, rel_name,
            text[: match.start()].count("\n") + 1,
            "the release manifest declares licence '{}', which conflicts with the "
            "selected expected licence ({})".format(value, expected),
            _snippet("License: " + value),
            "Either re-run with --expected-license naming the licence the release "
            "really ships, or correct the recorded licence. This checker never "
            "edits a licence.",
        ))
    return findings, notes


# --------------------------------------------------------------------------
# Check 2 - mandatory and judge-release source/documentation entries
# --------------------------------------------------------------------------

def check_mandatory_files(pkg: Package, rules: PackagingRules) -> list:
    findings = []
    missing = [rel for rel in rules.mandatory if not pkg.has(rel)]
    for rel in missing:
        findings.append(_finding(
            "HG-PKG-SRC-001", SEV_FAIL, rel, 0,
            "a file the packaging script declares mandatory is absent from the "
            "produced package (mandatory list read from {})".format(rules.source),
            "<file absent from package>",
            "Re-run scripts/package_release.sh: its own verification step should "
            "have failed on this, so the artefact is stale or was hand-edited.",
        ))

    for rel, why in JUDGE_RELEASE_ENTRIES:
        if rel in rules.mandatory or pkg.has(rel):
            continue
        findings.append(_finding(
            "HG-PKG-SRC-010", SEV_REVIEW, rel, 0,
            "the judge-release harness consumes '{}' ({}) but the packaging "
            "script does not declare it mandatory and the package does not "
            "carry it".format(rel, why),
            "<file absent from package>",
            "Either add '{}' to MANDATORY_FILES in scripts/package_release.sh, or "
            "package the revision that carries it (it exists on the release "
            "candidate branch).".format(rel),
        ))

    notice = pkg.path("NOTICE")
    if notice is not None and notice.is_file():
        text = notice.read_text(encoding="utf-8", errors="replace")
        for token in sorted(set(NOTICE_BUNDLED_PATH_RE.findall(text))):
            if pkg.has(token):
                continue
            # Only report a missing bundled *directory*, not a file the NOTICE
            # names inside a component it also names.
            parent = token.split("/")[0] + "/" + token.split("/")[1] \
                if len(token.split("/")) > 1 else token
            if pkg.has(parent) or pkg.has(parent + "/"):
                continue
            findings.append(_finding(
                "HG-PKG-SRC-030", SEV_REVIEW, token, 0,
                "NOTICE names the bundled component '{}', but the package does "
                "not carry it, so the attribution and the artefact disagree".format(
                    token
                ),
                "NOTICE: {}".format(token),
                "Ship the bundled component, or correct NOTICE so it lists only "
                "what the package actually contains.",
            ))
    return findings


# --------------------------------------------------------------------------
# Check 4 - secrets and build output (highest value)
# --------------------------------------------------------------------------

MAX_PER_CLASS = 20


def _classify_entries(pkg: Package, rules: PackagingRules):
    """Return (findings, content_candidates, pruned_dirs, stats)."""
    findings = []
    counts: dict[str, int] = {}
    content_candidates: list[tuple[Path, str]] = []
    root = pkg.root
    stats: dict = {"entries": 0, "pruned_dirs": 0, "content_files": 0,
                    "binary_skipped": 0, "large_skipped": 0}
    if root is None:
        return findings, content_candidates, counts, stats

    def emit(rule, severity, rel, message, correction, evidence=None):
        counts[rule] = counts.get(rule, 0) + 1
        if counts[rule] > MAX_PER_CLASS:
            return
        findings.append(_finding(
            rule, severity, rel, 0, message,
            evidence or rel, correction,
        ))

    for rel, is_dir, pruned in iter_package_entries(root):
        stats["entries"] += 1
        if is_dir and pruned:
            stats["pruned_dirs"] += 1
        name = rel.rsplit("/", 1)[-1]
        hit = _forbidden_hit(rel, rules.forbidden)
        if hit is not None and (is_dir or not _matches(name, CREDENTIAL_NAME_RES)):
            emit("HG-PKG-LEAK-002", SEV_FAIL, rel,
                 "'{}' matches the packaging script's own forbidden list (token "
                 "'{}'), so the packaging leak gate was bypassed or its rules "
                 "changed".format(rel, hit),
                 "Exclude it from the package (the packaging script's step 4 "
                 "rejects exactly this list) and re-run the packaging.")
            continue
        if is_dir:
            if is_prune_dir(name) and pruned:
                emit("HG-PKG-LEAK-004", SEV_REVIEW, rel,
                     "build/intermediate directory '{}' is packaged; a source "
                     "release archive is not expected to carry it".format(rel),
                     "Exclude build output from the package.")
            continue
        if _matches(name, CREDENTIAL_NAME_RES):
            if CREDENTIAL_TEMPLATE_RE.match(name):
                emit("HG-PKG-LEAK-007", SEV_REVIEW, rel,
                     "credential-shaped template file '{}' is packaged; it is not "
                     "itself a secret, but confirm it holds only placeholders".format(
                         rel
                     ),
                     "Keep only placeholder values in '{}', or exclude it and "
                     "document the variables in a doc instead.".format(rel))
            else:
                emit("HG-PKG-LEAK-001", SEV_FAIL, rel,
                     "credential-shaped file '{}' is packaged (pattern class "
                     "'credential filename')".format(rel),
                     "Remove the file from the package and rotate anything it "
                     "contained; the packaging must carry no key material.")
            content_candidates.append((root / rel, rel))
            continue
        if _matches(name, BUILD_ARTIFACT_NAME_RES):
            emit("HG-PKG-LEAK-004", SEV_REVIEW, rel,
                 "build artefact/intermediate '{}' is packaged (pattern class "
                 "'build output'); a source release is expected to ship sources "
                 "only".format(rel),
                 "Exclude build output from the package.")
            content_candidates.append((root / rel, rel))
            continue
        if _matches(name, LOCAL_STATE_NAME_RES):
            emit("HG-PKG-LEAK-005", SEV_REVIEW, rel,
                 "local state '{}' is packaged (pattern class 'local state'; "
                 "database/log/core/backup file)".format(rel),
                 "Local runtime state must not ship in a release package.")
            content_candidates.append((root / rel, rel))
            continue
        if _matches(name, CAPTURED_MEDIA_RES) or FRAME_DUMP_NAME_RE.search(rel) or (
            name.lower().endswith((".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"))
            and any(part in FRAME_DUMP_DIRS for part in rel.split("/")[:-1])
        ):
            emit("HG-PKG-LEAK-005", SEV_REVIEW, rel,
                 "captured media '{}' is packaged (pattern class 'captured "
                 "media')".format(rel),
                 "Captured camera/audio material must not ship in the package; if "
                 "a sample is intended, say so in the packaging rules.")
            content_candidates.append((root / rel, rel))
            continue
        # Every remaining regular file is content-scanned: a secret can hide in
        # any packaged file, including one already flagged by name (the name
        # finding says what the file *is*, the content finding says what it
        # carries).
        content_candidates.append((root / rel, rel))

    return findings, content_candidates, counts, stats


def scan_packaged_values(candidates: Sequence[tuple[Path, str]], root: Path,
                         max_bytes: int, include_topology: bool):
    """Scan packaged file *contents* for credential-shaped values.

    Reuses ``check_submission_claims.scan_credentials`` (patterns, placeholder
    filter, redaction fields) rather than duplicating the pattern set.  The
    matched value never leaves this function: only the class, the value length
    and ``sha256[:8]`` are carried forward, exactly as in the submission scanner.

    ``candidates`` are (path, label) pairs; the label is the package-relative
    path, or the file's name for the produced metadata files that sit beside the
    archive (the release manifest and the checksum files are part of the produced
    package and are scanned for the same reason as the tree is).
    """
    _require()
    stream: list[tuple[str, int, str]] = []
    skipped_binary = 0
    skipped_large = 0
    for path, label in sorted(candidates, key=lambda item: str(item[0])):
        try:
            raw = path.read_bytes()
        except OSError:
            continue
        if len(raw) > max_bytes:
            skipped_large += 1
            continue
        if b"\x00" in raw[:8192]:
            skipped_binary += 1
            continue
        text = raw.decode("utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            stream.append((label, lineno, line))

    findings, sensitive = csc.scan_credentials(stream)
    by_line = {(rel, lineno): text for rel, lineno, text in stream}
    if not include_topology:
        topology = {"private-ipv4-topology", "public-ipv4-literal"}
        kept = [f for f in findings if f.klass not in topology]
        excluded = len(findings) - len(kept)
        findings = kept
    else:
        excluded = 0
    return findings, sensitive, by_line, {
        "lines_scanned": len(stream),
        "binary_skipped": skipped_binary,
        "large_skipped": skipped_large,
        "topology_matches_not_reported": excluded,
    }


def is_prune_dir(name: str) -> bool:
    """True for a directory the walker will not descend into."""
    return name in PRUNE_DIR_NAMES or name.endswith("_build")


#: Credential pattern classes (the scanner's topology classes are excluded
#: unless --include-topology is given).
CREDENTIAL_CLASSES = frozenset({
    "wireguard-key", "private-key-header", "aws-access-key-id", "github-token",
    "google-api-key", "secret-assignment",
})

#: Value shapes that are labels, not secrets.  The scanner's own placeholder set
#: is reused first; this extends it with the negative-test labels a release
#: package tends to contain ("invalid-operator-secret", "not-the-secret").
CREDENTIAL_MARKER_RE = re.compile(
    r"(?i)^(?:"
    r"x{3,}|\.{3,}|<[^>]*>|\$\{?[A-Za-z_][A-Za-z0-9_]*\}?|"
    r"invalid(?:[-_][a-z0-9_-]*)?|not[-_]the[-_]secret|not[-_][a-z0-9_-]*|"
    r"wrong(?:[-_][a-z0-9_-]*)?|dummy(?:[-_][a-z0-9_-]*)?|"
    r"fake(?:[-_][a-z0-9_-]*)?|redacted|changeme|placeholder|"
    r"example(?:[-_][a-z0-9_-]*)?|sample(?:[-_][a-z0-9_-]*)?)$"
)

#: An identifier chain (``process.env.X``, ``fs.readFileSync``,
#: ``document.getElementById``): the scanner matched a code expression, not a
#: literal value.
CODE_REFERENCE_RE = re.compile(
    r"[A-Za-z_$][A-Za-z0-9_$]*(?:\.[A-Za-z_$][A-Za-z0-9_$]*)+"
)


def _credential_shape(finding, line: str) -> str:
    """Classify a scanner credential finding: literal | reference | marker.

    The match is re-derived from the scanner's own pattern set (the patterns are
    reused, not re-implemented) so the value's *context* can be inspected: a
    value that is a dotted identifier chain and is not quoted in the source is a
    reference to a variable or function, and a value carrying an explicit
    placeholder/negative-test label is a label.  Both are REVIEW, with the
    reason recorded, rather than a FAIL that would train the reader to ignore
    this check.  Everything else stays FAIL.

    Returns "literal" when the shape cannot be re-derived, because the
    conservative reading of an unclassifiable credential-shaped value is that it
    is one.
    """
    mod = _require()
    pattern = dict(mod.SECRET_PATTERNS).get(finding.klass)
    if pattern is None or not line:
        return "literal"
    for match in pattern.finditer(line):
        value = match.group(1) if match.groups() else match.group(0)
        if not value:
            continue
        if len(value) != finding.value_length or _sha256_8(value) != finding.value_sha256_8:
            continue
        if mod.PLACEHOLDER_RE.match(value) or CREDENTIAL_MARKER_RE.match(value):
            return "marker"
        start = match.start(1) if match.groups() else match.start(0)
        quoted = start > 0 and line[start - 1] in "\"'`"
        if not quoted and CODE_REFERENCE_RE.fullmatch(value):
            return "reference"
        return "literal"
    return "literal"


def check_packaging_hygiene(pkg: Package, rules: PackagingRules,
                            max_scan_bytes: int, include_topology: bool):
    """Check 4 - secrets and build output.  Returns (findings, sensitive, stats)."""
    findings, candidates, counts, stats = _classify_entries(pkg, rules)
    # The produced metadata beside the archive is part of the package too.
    for sibling in (pkg.release_manifest, *pkg.checksums.values()):
        if sibling is not None and sibling.is_file():
            candidates.append((sibling, sibling.name))
    value_findings, sensitive, by_line, scan_stats = scan_packaged_values(
        candidates, pkg.root, max_scan_bytes, include_topology
    )
    # Re-label the reused credential findings into this checker's namespace, and
    # with the scanner's own "ambiguity is REVIEW" posture applied to the value's
    # SHAPE: a dotted identifier chain that is not quoted is a code reference and
    # an explicit placeholder/negative-test label is a label.  Both stay visible,
    # neither is a FAIL.
    relabelled = []
    downgraded = {"reference": 0, "marker": 0}
    for f in value_findings:
        if f.klass in CREDENTIAL_CLASSES:
            shape = _credential_shape(f, by_line.get((f.file, f.line), ""))
        else:
            shape = "literal"
        if shape == "reference":
            severity = SEV_REVIEW
            what = ("Credential-shaped code reference inside a packaged file "
                    "(downgraded to REVIEW: the matched text is an unquoted "
                    "dotted identifier chain, not a literal value)")
        elif shape == "marker":
            severity = SEV_REVIEW
            what = ("Credential-shaped placeholder/negative-test label inside a "
                    "packaged file (downgraded to REVIEW: the matched text is an "
                    "explicit non-secret label)")
        else:
            severity = SEV_FAIL
            what = "Possible credential inside a packaged file"
        if shape != "literal":
            downgraded[shape] = downgraded.get(shape, 0) + 1
        relabelled.append(f.__class__(
            rule="HG-PKG-LEAK-003",
            severity=severity,
            file=f.file,
            line=f.line,
            message=(
                "{}: pattern class '{}' (value withheld; length {}, "
                "sha256[:8] {})".format(
                    what, f.klass, f.value_length, f.value_sha256_8
                )
            ),
            evidence=f.evidence,
            correction=(
                "Review the packaged file: if the value is real, remove it and "
                "rotate it, replacing it with a placeholder or a reference to a "
                "secret store. If it is a fixture or a reference, say so in the "
                "review record so the finding is not re-raised."
            ),
            klass=f.klass,
            value_length=f.value_length,
            value_sha256_8=f.value_sha256_8,
        ))
    findings.extend(relabelled)
    stats.update(scan_stats)
    stats["leak_counts"] = counts
    stats["credential_value_shapes"] = dict(
        downgraded,
        literal=sum(1 for f in relabelled if f.severity == SEV_FAIL),
    )

    # Drift between the packaging script's exclusion list and this checker's copy.
    parsed = tuple(rules.forbidden)
    if parsed != PACKAGING_FORBIDDEN_FALLBACK:
        findings.append(_finding(
            "HG-PKG-LEAK-006", SEV_REVIEW, "<packaging rules>", 0,
            "the packaging script's forbidden-artefact list differs from the copy "
            "this checker was written against, so the two may disagree about what "
            "may be packaged",
            "packaging: {} | checker copy: {}".format(
                ", ".join(parsed), ", ".join(PACKAGING_FORBIDDEN_FALLBACK)
            ),
            "Reconcile the lists: update PACKAGING_FORBIDDEN_FALLBACK in "
            "check_release_package.py to match scripts/package_release.sh.",
        ))
    return findings, sensitive, stats


# --------------------------------------------------------------------------
# Check 5 - the package records its source revision
# --------------------------------------------------------------------------

def _revision_records(pkg: Package) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """Return (full-sha records, short-sha records) with their provenance."""
    full: list[tuple[str, str]] = []
    short: list[tuple[str, str]] = []

    if pkg.release_manifest is not None:
        text = pkg.release_manifest.read_text(encoding="utf-8", errors="replace")
        for match in LABELLED_SHA_RE.finditer(text):
            sha = match.group("sha")
            label = " ".join(match.group("label").split())
            if len(sha) == 40:
                full.append((sha, "{} field in {}".format(label, pkg.release_manifest.name)))
            else:
                short.append((sha, "{} field in {}".format(label, pkg.release_manifest.name)))

    # A revision file inside the package.
    if pkg.root is not None:
        for dirpath, dirnames, filenames in os.walk(pkg.root):
            dirnames[:] = sorted(d for d in dirnames if d not in PRUNE_DIR_NAMES)
            for name in sorted(filenames):
                if not _matches(name, REVISION_FILE_RES):
                    continue
                path = Path(dirpath) / name
                try:
                    text = path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                rel = path.relative_to(pkg.root).as_posix()
                shas = FULL_SHA_RE.findall(text)
                for sha in shas:
                    full.append((sha, "{} inside the package".format(rel)))
                if not shas:
                    token = text.strip().splitlines()[0].strip() if text.strip() else ""
                    if re.fullmatch(r"[0-9a-f]{7,39}", token):
                        short.append((token, "{} inside the package".format(rel)))

    # The archive / package name produced by the packaging script.
    for label, name in (("archive name", pkg.name), ("package directory", pkg.name)):
        match = PACKAGE_NAME_RE.search(name or "")
        if match:
            short.append((match.group("sec"),
                          "security SHA prefix in the {}".format(label)))
            short.append((match.group("prep"),
                          "prep SHA prefix in the {}".format(label)))
    return full, short


def resolve_repo(explicit: str | None) -> tuple[Path | None, bool]:
    """Return (repository, explicitly_requested).

    An explicit ``--repo`` (or ``HG_REPO``) is honoured as given: if it does not
    look like a repository the caller reports that, rather than silently
    corroborating the recorded revision against some other clone found by
    walking up from the current directory.  Without an explicit request the
    repository is guessed from the environment, the working directory and this
    script's own directory - and a guess may only produce a note.
    """
    candidates = [explicit, os.environ.get("HG_REPO")]
    for candidate in filter(None, candidates):
        path = Path(candidate).resolve()
        for probe in [path, *path.parents][:4]:
            if (probe / ".git").exists():
                return probe, True
        return path, True  # honoured as given; usability is reported by the caller
    for candidate in (str(Path.cwd()), str(Path(__file__).resolve().parent)):
        path = Path(candidate).resolve()
        for probe in [path, *path.parents][:4]:
            if (probe / ".git").exists():
                return probe, False
    return None, False


def check_source_revision(pkg: Package, repo: Path | None, repo_explicit: bool = False):
    findings = []
    notes: list[str] = []
    full, short = _revision_records(pkg)
    stats = {"full_sha_records": [r for r, _ in full],
             "short_records": sorted({r for r, _ in short}),
             "repo": str(repo) if repo else None,
             "repo_explicit": repo_explicit}

    if not full and not short:
        findings.append(_finding(
            "HG-PKG-REV-001", SEV_FAIL, pkg.name or "<package>", 0,
            "the package records no source revision anywhere: no release "
            "manifest field, no revision file inside the package and no SHA in "
            "the archive or package name",
            "<no revision record found>",
            "Have the packaging write a manifest carrying the full source "
            "revision next to the archive (scripts/package_release.sh already "
            "emits Security Baseline SHA / Prep SHA fields when it runs to "
            "completion), and re-run this checker against that output.",
        ))
        return findings, notes, stats
    if not full:
        findings.append(_finding(
            "HG-PKG-REV-002", SEV_REVIEW, pkg.name or "<package>", 0,
            "the source revision is recorded only as a truncated prefix ({}); no "
            "full 40-hex revision is carried by the package, so a judge cannot "
            "identify the revision from the artefact alone".format(
                ", ".join(sorted({r for r, _ in short}))
            ),
            "short revision tokens: {}".format(
                ", ".join(sorted({r for r, _ in short}))),
            "Emit the full source revision into the package itself (a tracked "
            "revision file, or a manifest that is shipped inside the archive); "
            "the archive name only carries a 10-hex prefix.",
        ))
    else:
        notes.append(
            "the package records full source revision(s): {}".format(
                ", ".join(sorted({r for r, _ in full}))
            )
        )

    probe = _git(repo, "rev-parse", "--git-dir") if repo is not None else None
    repo_readable = bool(probe is not None and probe.returncode == 0)
    if full and repo is None:
        notes.append(
            "revision existence was NOT corroborated: no git repository was "
            "found (pass --repo DIR to check the recorded SHAs against one)"
        )
    elif full and not repo_readable:
        notes.append(
            "revision existence was NOT corroborated: {} is not a readable git "
            "repository".format(repo)
        )
    elif full:
        for sha, provenance in sorted(set(full)):
            proc = _git(repo, "cat-file", "-e", sha + "^{commit}")
            if proc.returncode != 0:
                if repo_explicit:
                    findings.append(_finding(
                        "HG-PKG-REV-003", SEV_REVIEW, pkg.name or "<package>", 0,
                        "the revision recorded by the package ({} ...) is not a "
                        "commit in the repository given with --repo ({})".format(
                            provenance, repo
                        ),
                        "recorded revision sha256[:8]: {}".format(_sha256_8(sha)),
                        "Re-run the packaging from the revision that is actually "
                        "being published, or point --repo at the repository the "
                        "release came from.",
                    ))
                else:
                    notes.append(
                        "the revision recorded by the package ({} ...) was not "
                        "found in the repository discovered automatically at {}; "
                        "pass --repo to assert which repository the release came "
                        "from".format(provenance, repo)
                    )
            else:
                notes.append(
                    "revision {} ({} ...) exists in {}".format(
                        sha, provenance, repo)
                )

    # Recorded digests must describe the artefact they are recorded beside.
    if pkg.archive is not None and pkg.checksums:
        for algo, path in sorted(pkg.checksums.items()):
            recorded = _recorded_digest(path, pkg.archive.name)
            if recorded is None:
                continue
            actual = _digest_file(pkg.archive, algo)
            if actual is None:
                continue
            if recorded != actual:
                findings.append(_finding(
                    "HG-PKG-REV-004", SEV_FAIL, path.name, 0,
                    "the recorded {} digest does not match the archive it "
                    "describes, so the provenance record and the artefact "
                    "disagree".format(algo.upper()),
                    "recorded {} != computed {} for {}".format(
                        recorded[:16], actual[:16], pkg.archive.name),
                    "Regenerate the checksums from the archive that is actually "
                    "being published (scripts/package_release.sh step 2 does "
                    "exactly that).",
                ))
            else:
                notes.append("{} digest matches the archive".format(algo.upper()))
    return findings, notes, stats


def _git(root: Path, *args: str):
    import subprocess
    try:
        return subprocess.run(["git", "-C", str(root), *args],
                              capture_output=True, text=True, timeout=30, check=False)
    except Exception:  # pragma: no cover - git missing or unusable
        return None


def _recorded_digest(path: Path, archive_name: str) -> str | None:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for line in lines:
        parts = line.split()
        if len(parts) >= 2 and parts[1].lstrip("*") == archive_name:
            return parts[0].lower()
    for line in lines:
        parts = line.split()
        if parts and re.fullmatch(r"[0-9a-fA-F]{64,128}", parts[0]):
            return parts[0].lower()
    return None


def _digest_file(path: Path, algo: str) -> str | None:
    if algo not in ("sha256", "sha512"):
        return None
    digest = hashlib.new(algo)
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


# --------------------------------------------------------------------------
# Check 6 - runtime dependency attribution coverage
# --------------------------------------------------------------------------

LOCKFILE_NAMES = ("package-lock.json", "npm-shrinkwrap.json")

#: Attribution material: NOTICE plus the third-party notice/licence material the
#: package carries.  A component is "attributed" when it is named in one of these.
ATTRIBUTION_FILE_GLOBS = (
    "NOTICE", "NOTICE.*", "NOTICE-*", "*NOTICE", "*NOTICE.md", "*NOTICE.txt",
    "THIRD_PARTY*", "ATTRIBUTION*", "CREDITS*", "COPYING*",
    "LICENSES/**", "licenses/**",
    "third_party/**/NOTICE*", "third_party/**/LICENSE*", "third_party/**/COPYING*",
    "third_party/*/README*", "third_party/*/LICENSE*",
)
ATTRIBUTION_MAX_BYTES = 512 * 1024

#: Licences that must be identified COMPONENT BY COMPONENT.  "Some transitive
#: packages are covered" is not a statement about the MPL: the Mozilla Public
#: License 2.0 conditions its grant on the covered software being identified and
#: its notice retained (s3.4/s3.5), and MPL-2.0 is the only licence in this tree
#: that is not satisfied by a permissive licence-level summary.  A component
#: under one of these licences is therefore FAIL, not REVIEW, when the
#: attribution record does not name it.
PER_PACKAGE_REQUIRED_LICENCES = {
    "MPL-2.0": "MPL-2.0 conditions its grant on the covered component being "
               "identified and its notice retained, so a licence-level summary is "
               "not enough for this component",
}

#: Licence aliases used to recognise a licence mention in the attribution record.
LICENCE_ALIASES = {
    "Apache-2.0": ("apache-2.0", "apache 2.0", "apache license", "apache licence",
                   "apache"),
    "MIT": ("mit license", "mit licence", "the mit license", "expat", "mit"),
    "MPL-2.0": ("mpl-2.0", "mpl 2.0", "mozilla public license", "mpl"),
    "BSD-3-Clause": ("bsd-3-clause", "bsd 3-clause", "3-clause bsd", "new bsd"),
    "BSD-2-Clause": ("bsd-2-clause", "2-clause bsd", "simplified bsd"),
    "0BSD": ("0bsd", "zero-clause bsd", "bsd zero clause"),
    "Unlicense": ("unlicense",),
    "BSL-1.0": ("bsl-1.0", "bsl 1.0", "boost software license"),
    "ISC": ("isc license", "isc licence"),
    "public domain": ("public domain",),
    "CC0-1.0": ("cc0",),
}

#: Scope markers.  A licence mention only covers a component if the mention sits
#: in a context scoped to that component's ecosystem: the MIT line for an
#: npm-installed package is not covered by a line about a CMake FetchContent
#: component, and vice versa.
SCOPE_MARKERS = {
    "npm": ("npm", "node_modules", "lockfile", "package-lock", "package.json",
            "transitive", "javascript", "node.js", "nodejs", "dependencies of the "
            "web gateway"),
    "fetched": ("fetchcontent", "fetched at build time", "fetched", "cmake",
                "build time"),
    "vendored": ("third_party", "vendored", "bundled", "in this repository"),
}
SCOPE_WINDOW = 3
DEP_AGGREGATE_EXAMPLES = 8


@dataclass
class Component:
    ecosystem: str          # npm | fetched | vendored
    name: str
    version: str | None
    licence: str | None
    origin: str
    parent: str | None = None
    dev: bool = False
    pinned: bool = True
    pin_reason: str = ""
    project_dir: str = ""   # directory of the lockfile that produced it

    @property
    def label(self) -> str:
        """``name@version`` when the version is known, else the bare name.

        Two resolved copies of the same package at different versions are two
        components and are labelled apart, so an aggregate never reads
        "tslib, tslib".
        """
        return "{}@{}".format(self.name, self.version) if self.version else self.name


def _read_text_file(path: Path, limit: int = ATTRIBUTION_MAX_BYTES) -> str:
    try:
        if path.stat().st_size > limit:
            return ""
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def collect_attribution(pkg: Package) -> tuple[list[tuple[str, int, str]], list[str]]:
    """Return the attribution record as (rel, lineno, line) plus the files read."""
    if pkg.root is None:
        return [], []
    seen: dict[str, Path] = {}
    for pattern in ATTRIBUTION_FILE_GLOBS:
        for path in sorted(pkg.root.glob(pattern)):
            if path.is_file():
                seen.setdefault(path.relative_to(pkg.root).as_posix(), path)
    lines: list[tuple[str, int, str]] = []
    for rel, path in sorted(seen.items()):
        text = _read_text_file(path)
        if not text:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            lines.append((rel, lineno, line))
    return lines, sorted(seen)


def _component_name_re(name: str) -> re.Pattern:
    """Match a component name allowing '-'/'_'/'/' to stand for each other."""
    parts = [p for p in re.split(r"[-_/]+", name.split("/")[-1]) if p]
    body = r"[-_/]".join(re.escape(part) for part in parts)
    return re.compile(r"(?<![\w@./-])" + body + r"(?![\w./-])", re.I)


def _licence_mentioned(text: str, licence: str | None) -> bool:
    if not licence:
        return False
    lowered = text.lower()
    aliases = LICENCE_ALIASES.get(licence)
    if aliases is None:
        aliases = (" ".join(licence.strip().lower().split()),)
    return any(alias in lowered for alias in aliases)


def _scope_relevant(lines: Sequence[tuple[str, int, str]], index: int,
                    ecosystem: str, project_dir: str,
                    other_project_dirs: Sequence[str]) -> bool:
    """True when the licence mention at ``index`` is scoped to this ecosystem.

    The window is the mentioning line plus the two before it, because an
    attribution list introduces its scope on the line above the entries.  A
    window that names a *different* lockfile's project directory does not cover
    this component: the Alexa section's npm statement is not a statement about
    the root web-gateway tree.
    """
    markers = SCOPE_MARKERS.get(ecosystem, ())
    lo = max(0, index - SCOPE_WINDOW + 1)
    window = " ".join(text for _, _, text in lines[lo:index + 1]).lower()
    if not any(marker in window for marker in markers):
        return False
    for other in other_project_dirs:
        if other and other != project_dir and other.rstrip("/") in window:
            return False
    return True


def _lockfile_paths(pkg: Package) -> list[str]:
    if pkg.root is None:
        return []
    found = []
    for name in LOCKFILE_NAMES:
        for path in sorted(pkg.root.rglob(name)):
            if path.is_file():
                found.append(path.relative_to(pkg.root).as_posix())
    return found


def collect_npm_components(pkg: Package, notes: list[str]) -> tuple[list[Component],
                                                                   list]:
    """Read runtime npm components out of every lockfile in the package."""
    findings = []
    components: list[Component] = []
    root = pkg.root
    if root is None:
        return components, findings
    lockfiles = _lockfile_paths(pkg)
    declared_npm = False
    for manifest in sorted(root.rglob("package.json")):
        try:
            data = json.loads(manifest.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and (data.get("dependencies")
                                       or data.get("optionalDependencies")):
            declared_npm = True

    if not lockfiles:
        if declared_npm:
            findings.append(_finding(
                "HG-PKG-DEP-006", SEV_REVIEW, "package.json", 0,
                "the package declares npm runtime dependencies but carries no "
                "lockfile, so the runtime dependency set cannot be checked against "
                "the attribution record",
                "<no package-lock.json / npm-shrinkwrap.json in the package>",
                "Ship the lockfile with the package so the dependency set is "
                "reproducible and attributable.",
            ))
        return components, findings

    for rel in lockfiles:
        path = root / rel
        project_dir = str(PurePosixPath(rel).parent)
        if project_dir == ".":
            project_dir = ""
        try:
            data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError) as exc:
            findings.append(_finding(
                "HG-PKG-DEP-006", SEV_REVIEW, rel, 0,
                "the lockfile could not be read ({}), so runtime dependency "
                "attribution cannot be checked".format(type(exc).__name__),
                "<unreadable lockfile>",
                "Fix or regenerate the lockfile; a package whose dependency set "
                "cannot be read cannot be attributed.",
            ))
            continue
        if not isinstance(data, dict):
            continue
        version = data.get("lockfileVersion")
        packages = data.get("packages")
        if not isinstance(packages, dict):
            findings.append(_finding(
                "HG-PKG-DEP-004", SEV_REVIEW, rel, 0,
                "lockfileVersion {} has no 'packages' map, so per-package licences, "
                "resolved URLs and integrity hashes cannot be verified".format(
                    version),
                "lockfileVersion: {}".format(version),
                "Regenerate the lockfile with npm 7+ (lockfileVersion 2 or 3), "
                "which records per-package licence, resolved and integrity fields.",
            ))
            continue
        for key in sorted(packages):
            entry = packages[key]
            if not key or not isinstance(entry, dict):
                continue  # the root entry is the project itself, not a component
            if entry.get("dev") is True:
                continue  # dev-only: not a runtime dependency
            name = key.split("node_modules/")[-1]
            licence = entry.get("license") or entry.get("licence")
            component = Component(
                ecosystem="npm",
                name=name,
                version=entry.get("version"),
                licence=str(licence) if isinstance(licence, str) else None,
                origin=rel,
                parent=_npm_parent(packages, name),
                pinned=bool(entry.get("integrity")),
                pin_reason="no integrity hash recorded for this package entry"
                if not entry.get("integrity") else "",
                project_dir=project_dir,
            )
            if not entry.get("resolved") and not entry.get("link"):
                component.pinned = False
                component.pin_reason = component.pin_reason or (
                    "no resolved URL recorded for this package entry")
            components.append(component)
        for key, spec in sorted((data.get("packages", {}).get("", {}) or {}).items()):
            if key not in ("dependencies", "optionalDependencies"):
                continue
            if not isinstance(spec, dict):
                continue
            for dep, constraint in sorted(spec.items()):
                if isinstance(constraint, str) and constraint.strip() in (
                    "*", "latest", "x", ""
                ):
                    findings.append(_finding(
                        "HG-PKG-DEP-004", SEV_REVIEW, rel, 0,
                        "runtime dependency '{}' is declared as an unconstrained "
                        "range ('{}'), so the resolved version is whatever the "
                        "registry serves at install time".format(dep, constraint),
                        "{}: \"{}\": \"{}\"".format(rel, dep, constraint),
                        "Pin the dependency (an exact version or a caret/tilde "
                        "range recorded in the lockfile) so the attributed "
                        "component is reproducible.",
                    ))

    unpinned = [c for c in components if not c.pinned]
    for component in unpinned[:DEP_AGGREGATE_EXAMPLES]:
        findings.append(_finding(
            "HG-PKG-DEP-004", SEV_REVIEW, component.origin, 0,
            "runtime dependency '{}' is not pinned: {}".format(
                component.name, component.pin_reason),
            "{}@{} in {}".format(component.name, component.version or "?",
                                 component.origin),
            "Regenerate the lockfile so the entry carries a resolved URL and an "
            "integrity hash.",
        ))
    if len(unpinned) > DEP_AGGREGATE_EXAMPLES:
        notes.append("{} further unpinned runtime dependency entries were not "
                     "listed individually".format(len(unpinned) - DEP_AGGREGATE_EXAMPLES))
    return components, findings


def _npm_parent(packages: dict, name: str) -> str | None:
    for key in sorted(packages):
        entry = packages.get(key)
        if key == "" or not isinstance(entry, dict):
            continue
        for field in ("dependencies", "optionalDependencies", "peerDependencies"):
            deps = entry.get(field)
            if isinstance(deps, dict) and name in deps:
                return key.split("node_modules/")[-1] or "the project itself"
    return None


FETCH_DECLARE_RE = re.compile(
    r"FetchContent_Declare\s*\((?P<body>.*?)\)", re.S
)
THIRD_PARTY_COMPONENT_RE = re.compile(r"^third_party/([^/]+)/?$")


def collect_cpp_components(pkg: Package) -> list[Component]:
    """Components the C++ build actually ships: vendored and FetchContent ones."""
    components: list[Component] = []
    if pkg.root is None:
        return components

    third_party = pkg.root / "third_party"
    if third_party.is_dir():
        for child in sorted(third_party.iterdir()):
            if not child.is_dir():
                continue
            components.append(Component(
                ecosystem="vendored",
                name=child.name,
                version=_vendored_version(child),
                licence=None,
                origin="third_party/{}".format(child.name),
            ))

    for cmake in sorted(pkg.root.rglob("CMakeLists.txt")):
        text = _read_text_file(cmake, 256 * 1024)
        for match in FETCH_DECLARE_RE.finditer(text):
            body = " ".join(match.group("body").split())
            name_match = re.match(r"([A-Za-z0-9_.-]+)", body)
            if not name_match:
                continue
            name = name_match.group(1)
            tag = re.search(r"GIT_TAG\s+(\S+)", body)
            components.append(Component(
                ecosystem="fetched",
                name=name,
                version=(tag.group(1).lstrip("v") if tag else None),
                licence=None,
                origin=cmake.relative_to(pkg.root).as_posix(),
            ))
    return components


def _read_head(path: Path, limit: int) -> str:
    """Read at most ``limit`` bytes from the head of a file (best effort)."""
    try:
        with path.open("rb") as handle:
            return handle.read(limit).decode("utf-8", "replace")
    except OSError:
        return ""


def _vendored_version(directory: Path) -> str | None:
    """Best-effort version of a vendored third-party directory."""
    # SQLite declares its version in the amalgamation.  A vendored tree may ship
    # the header alone, or - as this repository does - define SQLITE_VERSION
    # only inside sqlite3.c, so both are searched.  The read is head-only
    # because sqlite3.c is ~9 MiB and the define sits near the top of it.
    for name in ("sqlite3.h", "sqlite3.c"):
        for header in sorted(directory.rglob(name)):
            if not header.is_file():
                continue
            match = re.search(r'#define SQLITE_VERSION\s+"([0-9.]+)"',
                              _read_head(header, 256 * 1024))
            if match:
                return match.group(1)
    for candidate in sorted(directory.rglob("catch.hpp")) + sorted(
        directory.rglob("catch_all.hpp")
    ):
        text = _read_text_file(candidate, 512 * 1024)
        major = re.search(r"CATCH_VERSION_MAJOR\s+(\d+)", text)
        minor = re.search(r"CATCH_VERSION_MINOR\s+(\d+)", text)
        patch = re.search(r"CATCH_VERSION_PATCH\s+(\d+)", text)
        if major and minor and patch:
            return "{}.{}.{}".format(major.group(1), minor.group(1), patch.group(1))
    return None


def check_dependency_attribution(pkg: Package):
    """Check 6 - runtime dependency attribution coverage.

    Cross-checks the runtime dependencies the package resolves (npm lockfile
    entries that are not dev-only, plus the vendored and FetchContent C++
    components the build actually uses) against the attribution record (NOTICE
    and the third-party notice/licence material the package carries).

    Severity model, stated because it is a judgement call:
      * a component under a licence that requires per-component identification
        (MPL-2.0) and is not named                       -> FAIL (HG-PKG-DEP-001)
      * a runtime component not named, whose licence is not
        declared either in a context scoped to its ecosystem -> FAIL
        (HG-PKG-DEP-002, aggregated per licence so 40 missing MIT packages do
        not bury the report)
      * a component covered only by a licence-level declaration in a
        scope-relevant context                          -> REVIEW (HG-PKG-DEP-003)
      * pinning cannot be established (no integrity/resolved, wildcard range,
        lockfileVersion < 2)                            -> REVIEW (HG-PKG-DEP-004)
      * the record names a component but states no licence for it
                                                       -> REVIEW (HG-PKG-DEP-005)
      * no lockfile / unreadable lockfile               -> REVIEW (HG-PKG-DEP-006)
      * no attribution record at all                    -> REVIEW (HG-PKG-DEP-007)
      * the lockfile records no licence                 -> REVIEW (HG-PKG-DEP-008)
    """
    findings = []
    notes: list[str] = []
    attribution, attribution_files = collect_attribution(pkg)
    stats: dict = {
        "attribution_files": attribution_files,
        "npm_components": 0,
        "cpp_components": 0,
        "uncovered_by_licence": {},
        "per_package_required_missing": [],
    }

    if not attribution_files:
        findings.append(_finding(
            "HG-PKG-DEP-007", SEV_REVIEW, "NOTICE", 0,
            "the package carries no attribution record (no NOTICE or third-party "
            "notice/licence material), so no runtime dependency can be shown to be "
            "attributed",
            "<no attribution file found>",
            "Ship NOTICE (the packaging declares it mandatory) listing every "
            "runtime dependency or the licence under which it is bundled.",
        ))
        return findings, notes, stats

    components, dep_findings = collect_npm_components(pkg, notes)
    findings.extend(dep_findings)
    components.extend(collect_cpp_components(pkg))
    stats["npm_components"] = sum(1 for c in components if c.ecosystem == "npm")
    stats["cpp_components"] = len(components) - stats["npm_components"]

    other_dirs = sorted({c.project_dir for c in components if c.ecosystem == "npm"}
                        - {""})
    covered: list[Component] = []
    collective: list[tuple[Component, str]] = []
    uncovered: list[Component] = []
    per_package_missing: list[Component] = []
    unlicensed: list[Component] = []

    for component in components:
        matches = [
            index for index, (_, _, text) in enumerate(attribution)
            if _component_name_re(component.name).search(text)
        ]
        if not matches:
            if component.licence in PER_PACKAGE_REQUIRED_LICENCES:
                per_package_missing.append(component)
            else:
                scoped = [
                    index for index, (_, _, text) in enumerate(attribution)
                    if _licence_mentioned(text, component.licence)
                    and _scope_relevant(attribution, index, component.ecosystem,
                                        component.project_dir, other_dirs)
                ]
                if scoped and component.licence:
                    collective.append((component, component.licence))
                else:
                    uncovered.append(component)
            continue
        if not any(_licence_mentioned(attribution[index][2], component.licence)
                   for index in matches):
            if component.licence:
                unlicensed.append(component)
        covered.append(component)

    for component in per_package_missing:
        requirement = PER_PACKAGE_REQUIRED_LICENCES.get(component.licence or "", "")
        findings.append(_finding(
            "HG-PKG-DEP-001", SEV_FAIL, "NOTICE", 0,
            "runtime component '{}'{} is under {} and is NOT identified in the "
            "attribution record: {}".format(
                component.name,
                "@" + component.version if component.version else "",
                component.licence, requirement,
            ),
            "component: {} ({}), resolved as {} in {}; attribution record searched: "
            "{}".format(
                component.name,
                "transitive via {}".format(component.parent) if component.parent
                else "direct",
                component.version or "version not recorded",
                component.origin,
                ", ".join(attribution_files),
            ),
            "Name '{}' explicitly in NOTICE with its {} licence and where its "
            "source can be obtained; do not rely on a general statement that some "
            "transitive packages are covered.".format(component.name,
                                                      component.licence),
        ))
        stats["per_package_required_missing"].append(component.name)

    grouped: dict[str, list[Component]] = {}
    for component in uncovered:
        grouped.setdefault(component.licence or "<licence not recorded>", []).append(
            component)
    for licence in sorted(grouped):
        group = grouped[licence]
        labels = sorted({c.label for c in group})
        examples = ", ".join(labels[:DEP_AGGREGATE_EXAMPLES])
        findings.append(_finding(
            "HG-PKG-DEP-002", SEV_FAIL, "NOTICE", 0,
            "{} runtime component(s) under {} are not named in the attribution "
            "record, and no statement scoped to their ecosystem declares that "
            "licence: {}".format(len(group), licence, examples),
            "{} component(s) from {}: {}{}".format(
                len(group),
                ", ".join(sorted({c.origin for c in group})),
                examples,
                " (+{} more)".format(len(labels) - DEP_AGGREGATE_EXAMPLES)
                if len(labels) > DEP_AGGREGATE_EXAMPLES else "",
            ),
            "Add a section to NOTICE covering these components: either name them "
            "with their licences, or state the licence under which the whole "
            "runtime dependency set is bundled so the statement is scoped to it.",
        ))
        stats["uncovered_by_licence"][licence] = len(group)

    for component, licence in sorted(collective, key=lambda item: item[0].name):
        findings.append(_finding(
            "HG-PKG-DEP-003", SEV_REVIEW, "NOTICE", 0,
            "runtime component '{}' is covered only by a licence-level "
            "declaration of {} in a context scoped to its ecosystem, not by name".format(
                component.label, licence),
            "{} ({}), component {} not named in {}".format(
                component.label, component.origin,
                component.label, ", ".join(attribution_files)),
            "Name '{}' explicitly so a reader can map component to licence "
            "without inferring it.".format(component.name),
        ))

    for component in unlicensed:
        findings.append(_finding(
            "HG-PKG-DEP-005", SEV_REVIEW, "NOTICE", 0,
            "the attribution record names '{}' but states no licence for it, so "
            "the reader cannot tell what terms apply".format(component.label),
            "component: {} ({}), licence in the record: none found near the "
            "mention".format(component.label, component.licence or "unknown"),
            "State the licence next to the component name in the attribution "
            "record.",
        ))

    for component in components:
        if component.licence is None and component.ecosystem == "npm":
            findings.append(_finding(
                "HG-PKG-DEP-008", SEV_REVIEW, component.origin, 0,
                "the lockfile records no licence for runtime component '{}', so "
                "its attribution obligations cannot be derived".format(
                    component.label),
                "{} in {}".format(component.label, component.origin),
                "Regenerate the lockfile with a registry that publishes the "
                "licence field, and attribute the component explicitly.",
            ))

    # Components the attribution record names but for which the build declares a
    # version the record does not state: recorded as a note, not a finding,
    # because stating a version is good practice rather than a licence term.
    unversioned = sorted(
        c.name for c in components
        if c.version and any(
            _component_name_re(c.name).search(text) for _, _, text in attribution
        )
    )
    if unversioned:
        notes.append(
            "the attribution record names {} component(s) but states no version "
            "for them: {}".format(len(unversioned), ", ".join(unversioned))
        )
    stats["covered"] = sorted(c.label for c in covered)
    notes.append(
        "{} runtime component(s) checked against {} attribution file(s); {} named "
        "in the record, {} covered by a scoped licence-level declaration, {} not "
        "covered".format(
            len(components), len(attribution_files), len(covered),
            len(collective), len(uncovered) + len(per_package_missing),
        )
    )
    return findings, notes, stats


# --------------------------------------------------------------------------
# Report + orchestration
# --------------------------------------------------------------------------

def _sorted(findings: Iterable) -> list:
    try:
        return _require()._sort_findings(findings)
    except Exception:
        return sorted(findings, key=lambda f: (f.severity != SEV_FAIL, f.rule, f.file))


@dataclass
class Report:
    input_path: str
    origin: str = "?"
    package_root: str = "?"
    prefix: str | None = None
    expected_licence: str = ""
    packaging_rules: dict = field(default_factory=dict)
    findings: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    stats: dict = field(default_factory=dict)

    def counts(self) -> dict:
        return {
            SEV_FAIL: sum(1 for f in self.findings if f.severity == SEV_FAIL),
            SEV_REVIEW: sum(1 for f in self.findings if f.severity == SEV_REVIEW),
        }

    def exit_code(self) -> int:
        counts = self.counts()
        if counts[SEV_FAIL]:
            return EXIT_FAIL
        if counts[SEV_REVIEW]:
            return EXIT_REVIEW
        return EXIT_OK

    def check_status(self, number: int) -> str:
        group = [f for f in self.findings if RULE_CHECK.get(f.rule, 0) == number]
        if any(f.severity == SEV_FAIL for f in group):
            return "FAIL"
        return "REVIEW" if group else "PASS"

    def to_dict(self) -> dict:
        return {
            "program": PROGRAM,
            "input": self.input_path,
            "origin": self.origin,
            "package_root": self.package_root,
            "prefix": self.prefix,
            "expected_licence": self.expected_licence,
            "packaging_rules": self.packaging_rules,
            "check_names": {str(n): CHECK_NAMES[n] for n in sorted(CHECK_NAMES)},
            "checks": {
                str(n): {
                    "name": CHECK_NAMES[n],
                    "status": self.check_status(n),
                    "findings": sum(
                        1 for f in self.findings if RULE_CHECK.get(f.rule, 0) == n
                    ),
                }
                for n in sorted(CHECK_NAMES)
            },
            "counts": self.counts(),
            "total": len(self.findings),
            "findings": [
                dict(f.to_dict(), check=RULE_CHECK.get(f.rule, 0))
                for f in self.findings
            ],
            "notes": self.notes,
            "stats": self.stats,
        }


def run_audit(path: Path, expected_licence: str | None = None,
              package_script: Path | None = None, repo_arg: str | None = None,
              severity_min: str = SEV_REVIEW, include_topology: bool = False,
              max_scan_bytes: int = 2 * 1024 * 1024,
              secret_allowlist: Path | None = None) -> Report:
    _require()
    expected = csc.resolve_expected_licence(expected_licence)
    rules = load_packaging_rules(
        package_script if package_script is not None else default_package_script()
    )
    allowlist_path = resolve_secret_allowlist(secret_allowlist)
    allowlist_keys, allowlist_notes = load_secret_allowlist(allowlist_path)

    workdir = Path(tempfile.mkdtemp(prefix="hgpkg-audit-"))
    report = Report(input_path=str(path), expected_licence=expected)
    notes = list(rules.notes) + list(allowlist_notes)
    try:
        pkg, load_notes = load_package(path, workdir)
        notes.extend(load_notes)
        report.origin = pkg.origin
        report.prefix = pkg.prefix
        report.package_root = str(pkg.root) if pkg.root is not None else "?"
        report.packaging_rules = {
            "source": rules.source,
            "mandatory_count": len(rules.mandatory),
            "forbidden": list(rules.forbidden),
        }
        report.stats["archive"] = pkg.archive.name if pkg.archive else None
        report.stats["archive_entries"] = pkg.named_entries
        report.stats["release_manifest"] = (
            pkg.release_manifest.name if pkg.release_manifest else None
        )

        findings = []
        findings += check_licence_notice(pkg)
        findings += check_mandatory_files(pkg, rules)
        check3, check3_notes = check_manifest_licence_policy(pkg, expected)
        findings += check3
        notes.extend(check3_notes)
        leak, sensitive, leak_stats = check_packaging_hygiene(
            pkg, rules, max_scan_bytes, include_topology
        )
        findings += leak
        report.stats["packaging"] = leak_stats
        shapes = leak_stats.get("credential_value_shapes") or {}
        if shapes:
            notes.append(
                "credential-shaped matches inside packaged files: {} literal "
                "(FAIL), {} code reference and {} placeholder/negative-test label "
                "(REVIEW, with the reason recorded on each finding)".format(
                    shapes.get("literal", 0), shapes.get("reference", 0),
                    shapes.get("marker", 0),
                )
            )
        repo, repo_explicit = resolve_repo(repo_arg)
        rev, rev_notes, rev_stats = check_source_revision(pkg, repo, repo_explicit)
        findings += rev
        notes.extend(rev_notes)
        report.stats["revision"] = rev_stats
        dep, dep_notes, dep_stats = check_dependency_attribution(pkg)
        findings += dep
        notes.extend(dep_notes)
        report.stats["dependencies"] = dep_stats

        # The reviewed secret allow-list is applied last, on the assembled
        # findings: it downgrades the exact findings it names from FAIL to
        # REVIEW and changes nothing else.  A pass that matches no entry is not
        # affected, so the check is not widened by its presence.
        findings, allowlist_applied = apply_secret_allowlist(findings,
                                                             allowlist_keys)
        report.stats["secret_allowlist"] = {
            "path": str(allowlist_path) if allowlist_path else None,
            "format": SECRET_ALLOWLIST_FORMAT,
            "entries": len(allowlist_keys),
            "applied": allowlist_applied,
        }
        if allowlist_applied:
            notes.append(
                "{} credential finding(s) were allow-listed for review by {}: "
                "reported as REVIEW (never suppressed), value withheld".format(
                    allowlist_applied, allowlist_path)
            )

        findings = _sorted(f for f in findings if _passes_severity(f, severity_min))
        findings = _sanitize(findings, sensitive)
        # The scanner's guard, run last, over the masked findings.
        csc.enforce_redaction(findings, sensitive)
        report.findings = findings
        if severity_min == SEV_FAIL:
            notes.append("severity-min=FAIL: REVIEW findings were filtered out")
        if not include_topology:
            notes.append(
                "address/topology pattern classes (private IPv4, public IPv4 "
                "literals) are reported by check_submission_claims.py, not here; "
                "pass --include-topology to include them in this audit"
            )
        if leak_stats.get("topology_matches_not_reported"):
            notes.append(
                "{} address-shaped match(es) inside packaged files were found but "
                "not reported (topology classes disabled by default)".format(
                    leak_stats["topology_matches_not_reported"]
                )
            )
        report.notes = notes
        return report
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def _passes_severity(finding, severity_min: str) -> bool:
    if severity_min == SEV_FAIL:
        return finding.severity == SEV_FAIL
    return True


def exit_code_for(result: dict) -> int:
    """Exit code for an :func:`audit_package` result (same contract as the CLI)."""
    counts = result.get("counts") or {}
    if counts.get(SEV_FAIL):
        return EXIT_FAIL
    if counts.get(SEV_REVIEW):
        return EXIT_REVIEW
    return EXIT_OK


def audit_package(package_path, *, expected_licence: str | None = None,
                  package_script=None, repo: str | None = None,
                  include_topology: bool = False,
                  max_scan_bytes: int = 2 * 1024 * 1024,
                  severity_min: str = SEV_REVIEW) -> dict:
    """Importable entry point: audit a package and return the JSON-serialisable
    report dict.  No printing, no ``sys.exit``, no writes outside a private
    temporary directory.

    The reviewed secret allow-list (see the module docstring) is resolved from
    the ``HG_SECRET_ALLOWLIST`` environment variable or from
    ``scripts/release-secret-allowlist.json`` beside the checker; this signature
    is a documented contract, so the allow-list path is deliberately NOT an
    argument here (the CLI takes ``--secret-allowlist`` for that).

    Raises ``ScannerUnavailable`` when ``check_submission_claims.py`` cannot be
    imported, and ``PackageInputError`` when the path is not an auditable
    release package; both are defined in this module.
    """
    report = run_audit(
        Path(package_path),
        expected_licence=expected_licence,
        package_script=Path(package_script) if package_script else None,
        repo_arg=repo,
        severity_min=severity_min,
        include_topology=include_topology,
        max_scan_bytes=max_scan_bytes,
    )
    return report.to_dict()


def render_text(report: Report) -> str:
    counts = report.counts()
    out = ["{} - offline produced-release-package audit".format(PROGRAM)]
    out.append("input: {}".format(report.input_path))
    out.append("origin: {} (package root: {})".format(report.origin, report.package_root))
    if report.prefix:
        out.append("archive top-level directory: {}".format(report.prefix))
    out.append("expected licence: {}".format(report.expected_licence))
    rules = report.packaging_rules
    out.append(
        "packaging rules: {} ({} mandatory file(s), {} forbidden token(s))".format(
            rules.get("source", "?"), rules.get("mandatory_count", "?"),
            len(rules.get("forbidden", []) or []),
        )
    )
    out.append("")
    out.append("checks:")
    for number in sorted(CHECK_NAMES):
        group = [f for f in report.findings if RULE_CHECK.get(f.rule, 0) == number]
        out.append("  {}. {:<26} {:<6} ({} finding(s))".format(
            number, CHECK_NAMES[number], report.check_status(number), len(group)
        ))
    out.append("")
    out.append("findings by severity: FAIL={} REVIEW={} (total {})".format(
        counts[SEV_FAIL], counts[SEV_REVIEW], len(report.findings)
    ))
    out.append("")
    if not report.findings:
        out.append("no findings")
    for severity in (SEV_FAIL, SEV_REVIEW):
        group = [f for f in report.findings if f.severity == severity]
        if not group:
            continue
        out.append("=== {} ({}) ===".format(severity, len(group)))
        for finding in group:
            out.append("[{}] {} check={} {}:{}".format(
                finding.rule, finding.severity,
                RULE_CHECK.get(finding.rule, 0), finding.file, finding.line,
            ))
            out.append("    {}".format(finding.message))
            if finding.klass is not None:
                out.append("    pattern_class={} value_length={} value_sha256_8={}"
                           .format(finding.klass, finding.value_length,
                                   finding.value_sha256_8))
            out.append("    evidence: {}".format(finding.evidence))
            out.append("    fix: {}".format(finding.correction))
        out.append("")
    if report.notes:
        out.append("=== notes ===")
        for note in report.notes:
            out.append("- {}".format(note))
    return "\n".join(out)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROGRAM,
        description="Offline audit of a produced HomeGuardian release package "
                    "(directory or tarball).",
    )
    parser.add_argument("package", nargs="?", default=".",
                        help="produced package: a *-source.tar.gz, or the output "
                             "directory holding the archive and its manifest")
    parser.add_argument("--json", action="store_true",
                        help="emit the audit as JSON instead of text")
    parser.add_argument("--severity-min", choices=SEVERITIES, default=SEV_REVIEW,
                        help="minimum severity to report (default: REVIEW)")
    parser.add_argument("--expected-license", default=None, metavar="LICENCE",
                        help="licence the release is expected to carry (Apache-2.0 "
                             "or MIT).  Default: HG_EXPECTED_LICENSE, else the "
                             "scanner's DEFAULT_EXPECTED_LICENCE ({}).".format(
                                 "Apache-2.0"))
    parser.add_argument("--package-script", default=None, metavar="FILE",
                        help="scripts/package_release.sh to read MANDATORY_FILES "
                             "and the forbidden-artefact list from verbatim")
    parser.add_argument("--repo", default=None, metavar="DIR",
                        help="repository used to corroborate the revision the "
                             "package records")
    parser.add_argument("--include-topology", action="store_true",
                        help="also report private/public IPv4 literals found "
                             "inside packaged files (default: off; the "
                             "submission scanner covers those)")
    parser.add_argument("--max-scan-bytes", type=int, default=2 * 1024 * 1024,
                        help="skip packaged files larger than this for the content "
                             "scan (default: 2097152)")
    parser.add_argument("--secret-allowlist", default=None, metavar="FILE",
                        help="reviewed JSON allow-list of synthetic credential "
                             "fixtures, keyed by file, line, pattern class and "
                             "value sha256[:8]. An entry downgrades the finding "
                             "it names to REVIEW; it never suppresses it, and a "
                             "changed value at that location returns to FAIL. "
                             "Default: the HG_SECRET_ALLOWLIST variable, else "
                             "release-secret-allowlist.json beside this script "
                             "(absent by default, so nothing is allow-listed)")
    parser.add_argument("--quiet", action="store_true",
                        help="print nothing; only the exit code matters")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report = run_audit(
            Path(args.package),
            expected_licence=args.expected_license,
            package_script=Path(args.package_script) if args.package_script else None,
            repo_arg=args.repo,
            severity_min=args.severity_min,
            include_topology=args.include_topology,
            max_scan_bytes=args.max_scan_bytes,
            secret_allowlist=Path(args.secret_allowlist)
            if args.secret_allowlist else None,
        )
    except ScannerUnavailable as exc:
        print("INTERNAL ERROR: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL
    except PackageInputError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL
    except (OSError, tarfile.TarError) as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL
    except Exception as exc:  # the redaction guard lands here, and must be loud
        print("INTERNAL ERROR: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL

    if not args.quiet:
        if args.json:
            print(json.dumps(report.to_dict(), indent=2, sort_keys=True))
        else:
            print(render_text(report))
    return report.exit_code()


if __name__ == "__main__":
    sys.exit(main())
