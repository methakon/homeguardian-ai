#!/usr/bin/env python3
"""check_submission_claims.py - offline compliance scanner for a submission tree.

Purpose
-------
Before a hackathon submission is published, the prosaic artefacts (release
README, Devpost description, video script, licence files, packaged metadata)
are the only thing a judge reads.  This scanner checks those artefacts for the
failure modes that cost credibility:

  * LICENSING          - wrong / missing / contradictory licence statements.
  * CONTRADICTORY CLAIMS - the same subject asserted and negated across docs
                         (e.g. "remote MCP works" here, "remote MCP unverified"
                         in the package notes).
  * STALE REVISION REFERENCES - commit SHAs that do not exist, or that exist
                         but are not ancestors of HEAD, plus version/date
                         statements that disagree with the packaged metadata.
  * POSSIBLE PUBLISHED CREDENTIALS - key material, token shapes, secret
                         assignments, private-range topology addresses and bare
                         public IPv4 literals.  The matched value is NEVER
                         printed or serialised; only the pattern class, the
                         value length and the first 8 hex of its SHA-256 are
                         reported, so a human can correlate two reports without
                         the scanner becoming the disclosure.
  * PHYSICAL-TEST PRECISION - statements that couple the phone / Mi A3 with
                         MCP, remote access or VPN (which imply physical testing
                         that may not have happened) are separated from
                         statements about physical camera capture (which was
                         verified on real hardware) so the two are not merged.

Design constraints (deliberate)
-------------------------------
  * Python 3 standard library only - no third-party imports, no network calls.
  * Deterministic: findings are sorted, no timestamps in the output body.
  * Read-only: the scanner never writes to the tree it scans.
  * False-positive conscious: several checks are *deliberately* REVIEW rather
    than FAIL, and the SHA check exempts tokens that appear in a non-revision
    context (TLS fingerprints, content digests, device ids).  See
    `SHA_CONTEXT_EXEMPT` and `docstring` of `scan_revision_references` for the
    exact limits of what is verifiable offline.

Exit codes
----------
  0  no FAIL and no REVIEW findings
  1  at least one FAIL finding
  2  at least one REVIEW finding and no FAIL
  3  internal error (redaction guard tripped or unexpected failure)

Usage
-----
  python3 scripts/check_submission_claims.py [tree] [--json]
        [--paths FILE ...] [--severity-min FAIL|REVIEW]
        [--doc-scope submission|all] [--quiet]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

PROGRAM = "check_submission_claims"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_REVIEW = 2
EXIT_INTERNAL = 3

SEV_FAIL = "FAIL"
SEV_REVIEW = "REVIEW"
SEVERITIES = (SEV_FAIL, SEV_REVIEW)


# --------------------------------------------------------------------------
# Target artefacts
# --------------------------------------------------------------------------

#: Release README (judge-facing entry point).
RELEASE_README = "README.md"

#: Devpost description / demonstration guide.
DEVPOST_DOCS = (
    "docs/DEVPOST_SUBMISSION.md",
    "docs/DEVPOST_DEMONSTRATION_GUIDE.md",
)

#: Video script.
VIDEO_SCRIPT = "docs/SUBMISSION_VIDEO_SCRIPT.md"

#: Packaged metadata (what the submission package claims about itself).
PACKAGE_DOCS = (
    "docs/SUBMISSION_PACKAGE.md",
    "docs/PROOF_REGISTER.md",
)

#: Licence artefacts.
LICENCE_FILES = ("LICENSE", "NOTICE")

#: Version files, checked verbatim if present (+ any VERSION* / version.json).
VERSION_FILE_GLOBS = ("VERSION", "VERSION.txt", "version.txt", "version.json")

#: Judge-facing doc set: where statements about the *submission* live.  This is
#: the default scope for licence-phrasing, date/version and packaging checks.
SUBMISSION_DOC_SET = (
    RELEASE_README,
    *DEVPOST_DOCS,
    VIDEO_SCRIPT,
    *PACKAGE_DOCS,
)

DOC_SUFFIXES = (".md",)
MANIFEST_NAMES = ("package.json",)

#: Directories never descended into (build output, VCS, vendored deps).
SKIP_DIRS = frozenset(
    {
        ".git",
        ".hg",
        ".svn",
        "node_modules",
        "build",
        "dist",
        "out",
        "target",
        "coverage",
        ".venv",
        "venv",
        "__pycache__",
        ".mypy_cache",
        ".pytest_cache",
    }
)
SKIP_DIR_SUFFIXES = ("_build",)


def _is_skipped_dir(name: str) -> bool:
    if name in SKIP_DIRS:
        return True
    return any(name.endswith(suffix) for suffix in SKIP_DIR_SUFFIXES)


# --------------------------------------------------------------------------
# Findings
# --------------------------------------------------------------------------


class RedactionGuardError(RuntimeError):
    """Raised when a finding would disclose a matched sensitive value.

    The message deliberately contains the *length* and the rule id only - never
    the value itself, otherwise the guard would leak what it is protecting.
    """


@dataclass(frozen=True)
class Finding:
    rule: str
    severity: str
    file: str
    line: int
    message: str
    evidence: str
    correction: str
    klass: str | None = None  # pattern class (credentials only)
    value_length: int | None = None  # matched value length (credentials only)
    value_sha256_8: str | None = None  # truncated digest (credentials only)

    def to_dict(self) -> dict:
        data = {
            "rule": self.rule,
            "severity": self.severity,
            "file": self.file,
            "line": self.line,
            "message": self.message,
            "evidence": self.evidence,
            "correction": self.correction,
        }
        if self.klass is not None:
            data["pattern_class"] = self.klass
        if self.value_length is not None:
            data["value_length"] = self.value_length
        if self.value_sha256_8 is not None:
            data["value_sha256_8"] = self.value_sha256_8
        return data

    def sort_key(self) -> tuple:
        sev = 0 if self.severity == SEV_FAIL else 1
        return (sev, self.rule, self.file, self.line, self.message)


def _sort_findings(findings: Iterable[Finding]) -> list[Finding]:
    return sorted(findings, key=Finding.sort_key)


def enforce_redaction(
    findings: Sequence[Finding], sensitive_values: Iterable[str]
) -> None:
    """Fail loudly if any finding's rendered text embeds a sensitive value.

    This is the belt-and-braces guard demanded by rule 4: even if a future
    change to the redaction code accidentally inlined the matched text into a
    message, evidence or correction string, the scanner refuses to print it.
    `sensitive_values` must contain every raw value the credential scanner
    matched (including private/public IPv4 literals).
    """
    needle_pool = [v for v in sensitive_values if v and len(v) >= 4]
    for finding in findings:
        blob = "\n".join(
            (
                finding.rule,
                finding.message or "",
                finding.evidence or "",
                finding.correction or "",
                finding.klass or "",
            )
        )
        for value in needle_pool:
            if value in blob:
                raise RedactionGuardError(
                    "redaction guard tripped: rule {} would disclose a matched "
                    "value (length {}) - refusing to print".format(
                        finding.rule, len(value)
                    )
                )


def _sha256_8(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", "replace")).hexdigest()[:8]


def _snippet(text: str, width: int = 120) -> str:
    """Collapse whitespace and truncate - evidence must never be huge."""
    flat = " ".join(text.split())
    if len(flat) <= width:
        return flat
    return flat[: width - 1] + "\u2026"


# --------------------------------------------------------------------------
# File walking
# --------------------------------------------------------------------------


@dataclass
class ScanTarget:
    rel: str  # path relative to root, POSIX style
    abspath: Path
    kind: str  # doc | manifest | licence | version


def _rel(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def _classify(rel: str) -> str | None:
    name = rel.rsplit("/", 1)[-1]
    if name in MANIFEST_NAMES:
        return "manifest"
    if rel in LICENCE_FILES or name in ("LICENSE", "NOTICE", "COPYING"):
        return "licence"
    if name in VERSION_FILE_GLOBS or (name.startswith("VERSION") and name.count(".") <= 1):
        return "version"
    if rel.endswith(DOC_SUFFIXES):
        return "doc"
    return None


def collect_targets(root: Path, paths: Sequence[str] | None = None) -> list[ScanTarget]:
    """Collect scannable files under `root` (or restricted to `paths`)."""
    found: dict[str, ScanTarget] = {}

    def add(path: Path) -> None:
        kind = _classify(_rel(root, path))
        if kind is None:
            return
        rel = _rel(root, path)
        found.setdefault(rel, ScanTarget(rel, path, kind))

    if paths:
        for item in paths:
            candidate = Path(item)
            if not candidate.is_absolute():
                candidate = root / item
            if candidate.is_dir():
                for dirpath, dirnames, filenames in os.walk(candidate):
                    dirnames[:] = sorted(d for d in dirnames if not _is_skipped_dir(d))
                    for filename in sorted(filenames):
                        add(Path(dirpath) / filename)
            elif candidate.is_file():
                add(candidate)
        return sorted(found.values(), key=lambda t: t.rel)

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not _is_skipped_dir(d))
        for filename in sorted(filenames):
            add(Path(dirpath) / filename)
    return sorted(found.values(), key=lambda t: t.rel)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _lines(path: Path) -> list[str]:
    try:
        return _read_text(path).splitlines()
    except OSError:
        return []


# --------------------------------------------------------------------------
# Rule 1 - licensing
# --------------------------------------------------------------------------

#: The licence this project is approved to ship under.
APPROVED_LICENCE = "Apache-2.0"

APACHE_MARKERS = (
    "apache license",
    "apache-2.0",
    "apache 2.0",
    "version 2.0, january 2004",
)

COMPETING_LICENCE_RE = re.compile(
    r"\b(MIT License|MIT Licence|licensed under the MIT|GNU General Public License"
    r"|GPL-?[23](?:\.0)?|LGPL|BSD-?[23]-Clause|Mozilla Public License|MPL-?2"
    r"|Common Development and Distribution|CC0|WTFPL|Unlicense)\b",
    re.IGNORECASE,
)

#: Attribution markers: a competing licence named here belongs to a third party,
#: which is legitimate (NOTICE exists precisely for that).
ATTRIBUTION_MARKERS = re.compile(
    r"(third[- ]party|third[- ]parties|component|dependency|dependencies|plugin"
    r"|upstream|vendored|separate|extern|includes software|https?://|github\.com/)",
    re.IGNORECASE,
)

#: A project-level licence assertion, which is reported as FAIL, not REVIEW.
PROJECT_LICENCE_ASSERTION_RE = re.compile(
    r"(this (?:project|repository|product|software|work) is (?:licensed|released|distributed)"
    r"|(?:our|the) licen[cs]e is|released under the)",
    re.IGNORECASE,
)


def scan_licensing(root: Path, targets: Sequence[ScanTarget]) -> list[Finding]:
    findings: list[Finding] = []
    by_rel = {t.rel: t for t in targets}

    # --- LICENSE itself -------------------------------------------------
    licence_target = by_rel.get("LICENSE")
    licence_is_apache = False
    if licence_target is None:
        findings.append(
            Finding(
                rule="HG-LIC-001",
                severity=SEV_FAIL,
                file="LICENSE",
                line=0,
                message="LICENSE file not found; the approved licence ({}) cannot be "
                "verified".format(APPROVED_LICENCE),
                evidence="<file absent>",
                correction="Add the Apache License 2.0 text as LICENSE (this project's "
                "approved licence).",
            )
        )
    else:
        text = _read_text(licence_target.abspath)
        lowered = text.lower()
        licence_is_apache = any(marker in lowered for marker in APACHE_MARKERS)
        if not licence_is_apache:
            competitor = COMPETING_LICENCE_RE.search(text)
            what = competitor.group(0) if competitor else "an unrecognised licence text"
            findings.append(
                Finding(
                    rule="HG-LIC-001",
                    severity=SEV_FAIL,
                    file=licence_target.rel,
                    line=_first_match_line(licence_target.abspath, COMPETING_LICENCE_RE)
                    or 1,
                    message="LICENSE is not the approved {} licence (found: {})".format(
                        APPROVED_LICENCE, what
                    ),
                    evidence=_snippet(_first_matching_line(licence_target.abspath,
                                                           COMPETING_LICENCE_RE)
                                      or "Apache-2.0 markers not found"),
                    correction="Replace LICENSE with the Apache License 2.0 text, or get "
                    "the licence change approved before publishing.",
                )
            )

    # --- NOTICE ---------------------------------------------------------
    notice_target = by_rel.get("NOTICE")
    if notice_target is None:
        findings.append(
            Finding(
                rule="HG-LIC-002",
                severity=SEV_REVIEW,
                file="NOTICE",
                line=0,
                message="NOTICE file not found; third-party attributions cannot be "
                "checked against the licence",
                evidence="<file absent>",
                correction="Add NOTICE listing the bundled third-party components and "
                "their licences.",
            )
        )
    elif licence_is_apache:
        notice_lower = _read_text(notice_target.abspath).lower()
        if not any(marker in notice_lower for marker in APACHE_MARKERS):
            findings.append(
                Finding(
                    rule="HG-LIC-003",
                    severity=SEV_REVIEW,
                    file=notice_target.rel,
                    line=1,
                    message="NOTICE does not state the project licence ({}), so it cannot "
                    "be shown consistent with LICENSE".format(APPROVED_LICENCE),
                    evidence=_snippet(_lines(notice_target.abspath)[0]
                                      if _lines(notice_target.abspath) else ""),
                    correction="State the project licence in NOTICE and keep the "
                    "third-party list separate from it.",
                )
            )

    # --- package.json licence fields ------------------------------------
    for target in targets:
        if target.kind != "manifest":
            continue
        raw = _read_text(target.abspath)
        try:
            data = json.loads(raw)
        except (ValueError, TypeError):
            findings.append(
                Finding(
                    rule="HG-LIC-010",
                    severity=SEV_REVIEW,
                    file=target.rel,
                    line=1,
                    message="manifest is not valid JSON; licence field cannot be read",
                    evidence=_snippet(raw),
                    correction="Fix the JSON so the packaged licence field is machine-"
                    "readable.",
                )
            )
            continue
        licence = data.get("license") if isinstance(data, dict) else None
        if licence is None:
            findings.append(
                Finding(
                    rule="HG-LIC-010",
                    severity=SEV_REVIEW,
                    file=target.rel,
                    line=_line_of_key(target.abspath, '"license"') or 1,
                    message="manifest declares no license field",
                    evidence=_snippet(_line_at(target.abspath,
                                               _line_of_key(target.abspath, '"name"') or 1)
                                      or raw),
                    correction='Add "license": "{}" to {}.'.format(
                        APPROVED_LICENCE, target.rel
                    ),
                )
            )
        elif not _licence_matches(str(licence)):
            findings.append(
                Finding(
                    rule="HG-LIC-011",
                    severity=SEV_FAIL,
                    file=target.rel,
                    line=_line_of_key(target.abspath, '"license"') or 1,
                    message="manifest license '{}' conflicts with LICENSE ({})".format(
                        licence, APPROVED_LICENCE if licence_is_apache else "missing/other"
                    ),
                    evidence=_snippet(_line_at(target.abspath,
                                               _line_of_key(target.abspath, '"license"') or 1)
                                      or ""),
                    correction='Set "license": "{}" (or the licence actually shipped).'.format(
                        APPROVED_LICENCE
                    ),
                )
            )

    # --- competing licence text in judge-facing docs --------------------
    for rel in SUBMISSION_DOC_SET:
        target = by_rel.get(rel)
        if target is None or target.kind not in ("doc", "licence"):
            continue
        for lineno, text in enumerate(_lines(target.abspath), 1):
            match = COMPETING_LICENCE_RE.search(text)
            if not match:
                continue
            if ATTRIBUTION_MARKERS.search(text):
                continue  # third-party attribution, not a claim about this work
            project_level = bool(PROJECT_LICENCE_ASSERTION_RE.search(text))
            findings.append(
                Finding(
                    rule="HG-LIC-020",
                    severity=SEV_FAIL if project_level else SEV_REVIEW,
                    file=rel,
                    line=lineno,
                    message="judge-facing document mentions a competing licence ('{}'); "
                    "the approved licence is {}".format(match.group(0), APPROVED_LICENCE),
                    evidence=_snippet(text),
                    correction="Keep the submission documents on the approved licence and "
                    "move third-party licences into NOTICE.",
                )
            )

    return findings


def _licence_matches(value: str) -> bool:
    lowered = value.strip().lower()
    if lowered in ("apache-2.0", "apache 2.0", "apache"):
        return True
    if "apache" in lowered and "2" in lowered:
        return True
    return False


def _first_match_line(path: Path, pattern: re.Pattern) -> int:
    for lineno, text in enumerate(_lines(path), 1):
        if pattern.search(text):
            return lineno
    return 0


def _first_matching_line(path: Path, pattern: re.Pattern) -> str:
    for text in _lines(path):
        if pattern.search(text):
            return text
    return ""


def _line_of_key(path: Path, key: str) -> int:
    for lineno, text in enumerate(_lines(path), 1):
        if key in text:
            return lineno
    return 0


def _line_at(path: Path, lineno: int) -> str:
    lines = _lines(path)
    if 1 <= lineno <= len(lines):
        return lines[lineno - 1]
    return ""


# --------------------------------------------------------------------------
# Rule 2 - contradictory claims
# --------------------------------------------------------------------------

POSITIVE_IMPLEMENTED = re.compile(
    r"\b(implemented|implementation complete|built(?![-\w])|is deployed|deployed|shipped"
    r"|(?<![-\w])enabled|integrated|integration complete|in place|is live|operational"
    r"|exists|is running|service running)\b",
    re.IGNORECASE,
)
POSITIVE_TESTED = re.compile(
    r"\b(verified|verification pass(?:ed)?|tested|passed|proven|measured|confirmed"
    r"|acceptance (?:pass(?:ed)?|record))\b",
    re.IGNORECASE,
)
#: Negation model: only *existence/verification* negations count, so a scope
#: negation such as "not required by the default demo" is not mistaken for
#: "not implemented".
NEGATED = re.compile(
    r"("
    r"\bno\b(?:\s+[\w'-]+){0,3}\s*"
    r"(?:server|endpoint|integration|deployment|implementation|device|camera|listener"
    r"|runtime|mcp|vpn|tunnel|service|support|identity|account|deploy|image|sensor"
    r"|microphone|hardware|evidence|proof)"
    r"|\bnot\b(?:\s+[\w'-]+){0,2}\s*"
    r"(?:implemented|built|deployed|integrated|available|present|verified|tested"
    r"|proven|real|live|connected|reachable|supported|in place|claimed|exercised"
    r"|evidence|proof|installed)"
    r"|\b(?:never|absent|absence|unverified|unproven|unimplemented|unsupported|blocked"
    r"|pending|missing|without|none|neither|cannot|can't)\b"
    r"|\bdo not (?:say|claim|state|assert|describe|present|recommend)\b"
    r"|\bnot yet\b|\bdoes not exist\b|\bno longer\b"
    r")",
    re.IGNORECASE,
)
PLANNED = re.compile(
    r"\b(planned|plan|plans|roadmap|proposed|proposal|intended|will be|to be built"
    r"|future work|not yet built|blocked pending|simulated|simulation|mock)\b",
    re.IGNORECASE,
)
#: Lines that quote requirements/rules rather than claim project state.
NORMATIVE = re.compile(
    r"(\bofficial update\b|\btrack:\b|\brubric\b|\brequirement[s]?\b|\bjudging criteria\b"
    r"|\bmust (?:build|include|provide)\b|\bthe brief\b)",
    re.IGNORECASE,
)

CLAIM_IMPLEMENTED = "IMPLEMENTED"
CLAIM_TESTED = "TESTED"
CLAIM_PLANNED = "PLANNED"
CLAIM_NEGATED = "NEGATED"
CLAIM_UNCLASSIFIED = "UNCLASSIFIED"
CLAIM_POSITIVE = (CLAIM_IMPLEMENTED, CLAIM_TESTED)

@dataclass(frozen=True)
class Subject:
    label: str
    patterns: tuple[re.Pattern, ...]
    #: Named aspects.  A statement is only compared against statements of the
    #: same aspect: "Alexa custom skill prototype implemented" must not
    #: contradict "no real Alexa+ device integration", and "physical camera
    #: capture verified" must not contradict "physical device transport not
    #: exercised" - conflating those is the failure this scanner exists to catch.
    aspects: tuple[tuple[str, re.Pattern], ...] = ()

    def aspect_names(self, text: str) -> list[str]:
        matched = [name for name, pattern in self.aspects if pattern.search(text)]
        return matched or ["generic"]


SUBJECTS: dict[str, Subject] = {
    "vpn_wireguard": Subject(
        "VPN/WireGuard",
        (re.compile(r"\bwireguard\b|\bvpn\b|\bwg0\b|\btunnel\b", re.IGNORECASE),),
    ),
    "remote_access": Subject(
        "remote access",
        (
            re.compile(
                r"remote access|remote-access|remote dashboard|public ingress"
                r"|externally reachable|from the internet",
                re.IGNORECASE,
            ),
        ),
    ),
    "remote_mcp": Subject(
        "remote MCP reachability",
        (
            re.compile(
                r"remote mcp|mcp reachab\w*|mcp (?:endpoint|server)[^.]{0,40}reachab\w*"
                r"|reachab\w+[^.]{0,20}mcp",
                re.IGNORECASE,
            ),
        ),
    ),
    "phone_hosted": Subject(
        "Mi A3 / phone-hosted service",
        (
            re.compile(
                r"\bmi ?a3\b|mi-a3|phone-hosted|phone hosted|hosted on (?:the )?phone"
                r"|service on the phone|android node",
                re.IGNORECASE,
            ),
        ),
    ),
    "v380_camera": Subject("V380 camera", (re.compile(r"\bv380\b", re.IGNORECASE),)),
    "physical_verification": Subject(
        "physical hardware verification",
        (
            re.compile(
                r"physical(?:ly)? (?:device|hardware|camera|test|capture|acceptance)"
                r"|tested on physical|hardware verification|on real hardware",
                re.IGNORECASE,
            ),
        ),
        aspects=(
            ("camera", re.compile(r"camera|capture|microphone|\bmic\b|frame", re.IGNORECASE)),
            (
                "device",
                re.compile(
                    r"\bdevice\b|hardware|\bphone\b|handset|\bapk\b|android node|\bnode\b",
                    re.IGNORECASE,
                ),
            ),
        ),
    ),
    "alexa_integration": Subject(
        "Alexa integration",
        (
            re.compile(
                r"alexa\+?[^.]{0,30}integration|alexa\+?\b|real alexa", re.IGNORECASE
            ),
        ),
        aspects=(
            (
                "skill",
                re.compile(
                    r"\bskill\b|ask (?:toolkit|cli)|invocation name|lambda|developer console"
                    r"|simulator panel",
                    re.IGNORECASE,
                ),
            ),
            (
                "platform",
                re.compile(
                    r"alexa\+|device integration|\bavs\b|account linking|login with amazon"
                    r"|\blwa\b|alexa os|amazon runtime|live alexa",
                    re.IGNORECASE,
                ),
            ),
        ),
    ),
}


@dataclass
class ClaimHit:
    subject: str
    aspect: str
    file: str
    line: int
    klass: str
    text: str


def classify_claim(text: str) -> str:
    """Conservative single-statement classifier.

    Order matters: an explicit negation wins over every positive marker, because
    "the MCP server is not implemented" must never read as a positive claim.
    """
    if NEGATED.search(text):
        return CLAIM_NEGATED
    if PLANNED.search(text):
        return CLAIM_PLANNED
    if POSITIVE_TESTED.search(text):
        return CLAIM_TESTED
    if POSITIVE_IMPLEMENTED.search(text):
        return CLAIM_IMPLEMENTED
    return CLAIM_UNCLASSIFIED


def collect_claims(
    doc_lines: Sequence[tuple[str, int, str]],
) -> tuple[list[ClaimHit], int]:
    """Return (hits, normative_lines_skipped).

    One hit per (subject, aspect) the statement belongs to; fenced code blocks
    and table rows are skipped because they are usually command/log excerpts
    rather than prose claims.
    """
    hits: list[ClaimHit] = []
    skipped = 0
    for rel, lineno, text in doc_lines:
        stripped = text.strip()
        if not stripped:
            continue
        if stripped.startswith("```") or stripped.startswith("|"):
            continue
        matched_subjects = [
            key
            for key, subject in SUBJECTS.items()
            if any(pattern.search(text) for pattern in subject.patterns)
        ]
        if not matched_subjects:
            continue
        if NORMATIVE.search(text) and stripped.startswith((">", "*", "-", "|")):
            skipped += 1
            continue
        klass = classify_claim(text)
        if klass == CLAIM_UNCLASSIFIED:
            continue
        for key in matched_subjects:
            for aspect in SUBJECTS[key].aspect_names(text):
                hits.append(ClaimHit(key, aspect, rel, lineno, klass, text))
    return hits, skipped


def stitch_statements(
    doc_lines: Sequence[tuple[str, int, str]]
) -> list[tuple[str, int, str]]:
    """Join markdown-wrapped continuation lines into logical statements.

    Line-level keyword classification reads "are not\nimplemented and are
    awaiting approval" as the positive claim "implemented" - the classic
    wrapped-prose false positive.  Joining continuations onto the fragment that
    starts the sentence removes that class of error; headings, bullets, table
    rows, blockquotes and fenced blocks still start a new statement.
    """
    stitched: list[tuple[str, int, str]] = []
    current: list | None = None
    for rel, lineno, text in doc_lines:
        stripped = text.strip()
        if current is not None and current[0] != rel:
            stitched.append((current[0], current[1], " ".join(current[2])))
            current = None
        starts_new = (
            not stripped
            or stripped.startswith(("#", "-", "*", ">", "|", "```", "~~~"))
            or re.match(r"^\d+[.)]\s", stripped) is not None
        )
        if starts_new:
            if current is not None:
                stitched.append((current[0], current[1], " ".join(current[2])))
                current = None
            if not stripped:
                continue
            current = [rel, lineno, [stripped]]
        elif current is None:
            current = [rel, lineno, [stripped]]
        else:
            current[2].append(stripped)
    if current is not None:
        stitched.append((current[0], current[1], " ".join(current[2])))
    return stitched


def scan_claims(
    doc_lines: Sequence[tuple[str, int, str]], root: Path | None = None
) -> tuple[list[Finding], int]:
    hits, skipped = collect_claims(doc_lines)
    findings: list[Finding] = []
    stored_dates: dict[str, str] = {}

    def file_date(rel: str) -> str:
        """Last-commit date of the file containing a statement (offline, from git).

        Used to tell a live contradiction from a superseded statement: this
        project keeps a chronological engineering log, so an earlier, undated
        "not implemented" next to a later "verified" is history, not a lie.
        """
        if root is None:
            return ""
        if rel not in stored_dates:
            stored_dates[rel] = _git(root, "log", "-1", "--format=%cs", "--", rel) \
                .stdout.strip()
        return stored_dates[rel]

    for subject_id, subject in SUBJECTS.items():
        subject_hits = [h for h in hits if h.subject == subject_id]
        aspects = sorted({h.aspect for h in subject_hits})
        for aspect in aspects:
            scoped = [h for h in subject_hits if h.aspect == aspect]
            positives = [h for h in scoped if h.klass in CLAIM_POSITIVE]
            negatives = [h for h in scoped if h.klass == CLAIM_NEGATED]
            qualifier = "" if aspect == "generic" else " [{}]".format(aspect)
            if positives and negatives:
                pos = positives[0]
                # Prefer a negated statement from a different file: a
                # contradiction between two documents is what matters.
                neg = next(
                    (n for n in negatives if n.file != pos.file), negatives[0]
                )
                pos_date = file_date(pos.file)
                neg_date = file_date(neg.file)
                superseded = bool(
                    pos_date and neg_date and neg_date < pos_date
                )
                findings.append(
                    Finding(
                        rule="HG-CLAIM-003" if superseded else "HG-CLAIM-001",
                        severity=SEV_REVIEW if superseded else SEV_FAIL,
                        file=pos.file,
                        line=pos.line,
                        message=(
                            "subject '{}{}' is asserted ({}) in one document and negated "
                            "({}) in another; the negated statement is in an older file "
                            "({} vs {}), so it may simply be superseded history"
                            if superseded
                            else "subject '{}{}' is both asserted ({}) and negated ({}) "
                            "across the documents ({} asserted / {} negated statement(s))"
                        ).format(
                            subject.label,
                            qualifier,
                            pos.klass,
                            neg.klass,
                            neg_date,
                            pos_date,
                            len(positives),
                            len(negatives),
                        ),
                        evidence="ASSERTED {}:{} '{}' | NEGATED {}:{} '{}'".format(
                            pos.file,
                            pos.line,
                            _snippet(pos.text, 70),
                            neg.file,
                            neg.line,
                            _snippet(neg.text, 70),
                        ),
                        correction="Reconcile the two statements: say plainly which part "
                        "is implemented, which is simulated and which is not built yet. "
                        "Date an older statement so the progression is visible.",
                    )
                )
            elif positives:
                pos = positives[0]
                findings.append(
                    Finding(
                        rule="HG-CLAIM-002",
                        severity=SEV_REVIEW,
                        file=pos.file,
                        line=pos.line,
                        message="subject '{}{}' is asserted ({}) with no accompanying "
                        "negation/limitation anywhere in the scanned documents".format(
                            subject.label, qualifier, pos.klass
                        ),
                        evidence=_snippet(pos.text),
                        correction="State the limitation next to the claim (what is "
                        "simulated, what was not tested) or remove the claim.",
                    )
                )
    return findings, skipped


# --------------------------------------------------------------------------
# Rule 3 - stale revision references
# --------------------------------------------------------------------------

SHA_RE = re.compile(r"\b[0-9a-f]{7,40}\b")
VERSION_RE = re.compile(r"\bv?\d+\.\d+(?:\.\d+)?\b")
DATE_RE = re.compile(r"\b(20\d{2})-(\d{2})-(\d{2})\b")

#: A hex token in one of these contexts is not a revision reference.  Offline
#: there is no way to tell a 40-hex content digest from a commit id other than
#: by the words around it, so this exemption is explicit and reportable.
SHA_CONTEXT_EXEMPT = re.compile(
    r"(sha256|sha-256|sha1|sha-1|sha512|md5|digest|checksum|fingerprint|cert(?:ificate)? "
    r"hash|\boid\b|catalog sha|blob|node_id|hg-[0-9a-f]|uuid)",
    re.IGNORECASE,
)

#: Lines that present a hex token as a revision reference.  A non-existent token
#: in this context is a FAIL; a bare hex token with no such context is only
#: REVIEW, because offline it may equally be a digest, a catalogue hash or a
#: device id ("ignore obvious non-SHAs" cannot be decided from the token alone).
REVISION_CONTEXT = re.compile(
    r"(\bcommit\b|\bsha\b|\bsha-1\b|\bsha256\b|\brevision\b|\brev\b|\bhead\b|\bmerge\b"
    r"|\bbase\b|\btag\b|\bcheckout\b|\bgit\b|\bbranch\b|\bcit(?:e|ed|ing)\b"
    r"|\bancestor\b|\bhistory\b|\bpushe?d?\b)",
    re.IGNORECASE,
)

#: A line that itself says the token is invalid/non-existent: report it, but as
#: the documentation wart it is rather than as a fabricated reference.
SELF_DOCUMENTED_INVALID = re.compile(
    r"(not a valid|invalid|nonexistent|non-existent|does not exist|one digit short"
    r"|wrong|typo|mis-?cited|incorrect)",
    re.IGNORECASE,
)
DEADLINE_CONTEXT = re.compile(
    r"(deadline|due|closes|submission window|target|by \w+day|opens)", re.IGNORECASE
)

REV_EXEMPT_NOTE = "sha-context-exempt"


def is_sha_candidate(token: str) -> bool:
    if not SHA_RE.fullmatch(token):
        return False
    if token.isdigit():
        return False  # all-digit strings (issue counters, byte counts)
    if len(token) < 7:
        return False
    return True


def scan_revision_references(
    root: Path, docs: Sequence[tuple[str, int, str]]
) -> tuple[list[Finding], dict]:
    """Check hex tokens against the git object database.

    Limits (deliberate, and why nothing here is FAIL-by-guess):
      * `git cat-file -e <sha>^{commit}` only proves the object exists *in this
        clone*; a SHA from another branch that was never fetched is reported the
        same way as a fabricated one.  A missing SHA is therefore FAIL only
        because the artefact is judge-facing and unverifiable as written.
      * The ancestor check (`git merge-base --is-ancestor`) needs HEAD; if the
        tree is not a git repository the check is skipped and reported as a note
        rather than silently passing.
      * Tokens in digest/fingerprint/device-id contexts are skipped (reported in
        `exempt`), because offline they are indistinguishable from commit ids.
    """
    findings: list[Finding] = []
    stats = {"considered": 0, "exempt": 0, "checked": 0, "git": True}
    git_ok = _git(root, "rev-parse", "--git-dir").returncode == 0
    stats["git"] = git_ok
    seen: dict[str, list[tuple[str, int, str]]] = {}
    for rel, lineno, text in docs:
        if SHA_CONTEXT_EXEMPT.search(text):
            for token in SHA_RE.findall(text):
                if is_sha_candidate(token):
                    stats["considered"] += 1
                    stats["exempt"] += 1
            continue
        for token in SHA_RE.findall(text):
            if not is_sha_candidate(token):
                continue
            stats["considered"] += 1
            seen.setdefault(token, []).append((rel, lineno, text))

    if not git_ok:
        return findings, stats

    max_locations = 6
    for token, occurrences in sorted(seen.items()):
        stats["checked"] += 1
        exists = _git(root, "cat-file", "-e", token + "^{commit}").returncode == 0
        locations = occurrences[:max_locations]
        overflow = len(occurrences) - len(locations)
        if not exists:
            for rel, lineno, text in locations:
                is_revision = bool(REVISION_CONTEXT.search(text))
                self_documented = bool(SELF_DOCUMENTED_INVALID.search(text))
                if is_revision and not self_documented:
                    findings.append(
                        Finding(
                            rule="HG-REV-001",
                            severity=SEV_FAIL,
                            file=rel,
                            line=lineno,
                            message="revision reference '{}' is cited as a revision but does "
                            "not exist as a commit in this repository ({} occurrence(s) in "
                            "total)".format(token, len(occurrences)),
                            evidence=_snippet(text),
                            correction="Replace the reference with the real commit SHA, or "
                            "drop it from the judge-facing document.",
                        )
                    )
                elif self_documented:
                    findings.append(
                        Finding(
                            rule="HG-REV-003",
                            severity=SEV_REVIEW,
                            file=rel,
                            line=lineno,
                            message="revision reference '{}' does not exist, and the same "
                            "line records that it is wrong; the stale reference still "
                            "ships".format(token),
                            evidence=_snippet(text),
                            correction="Correct or remove the known-bad reference instead of "
                            "leaving the explanation next to it.",
                        )
                    )
                else:
                    findings.append(
                        Finding(
                            rule="HG-REV-003",
                            severity=SEV_REVIEW,
                            file=rel,
                            line=lineno,
                            message="hex token '{}' does not exist as a commit and is not "
                            "presented as a revision on this line, so it may be a digest, "
                            "catalogue hash or identifier".format(token),
                            evidence=_snippet(text),
                            correction="If this is a revision reference, name it as one and "
                            "use a real SHA; otherwise make clear what the value identifies.",
                        )
                    )
            if overflow > 0:
                findings.append(
                    Finding(
                        rule="HG-REV-003",
                        severity=SEV_REVIEW,
                        file=locations[0][0],
                        line=locations[0][1],
                        message="{} further location(s) cite the same non-existent token "
                        "'{}'".format(overflow, token),
                        evidence="[remaining locations elided]",
                        correction="Fix every occurrence.",
                    )
                )
            continue
        if _git(root, "merge-base", "--is-ancestor", token, "HEAD").returncode != 0:
            for rel, lineno, text in locations:
                findings.append(
                    Finding(
                        rule="HG-REV-002",
                        severity=SEV_REVIEW,
                        file=rel,
                        line=lineno,
                        message="revision reference '{}' exists but is not an ancestor of HEAD "
                        "(not part of the published history)".format(token),
                        evidence=_snippet(text),
                        correction="Point the document at a commit that is reachable from the "
                        "published branch, or label the reference as unpushed work.",
                    )
                )
    return findings, stats


def scan_metadata_currency(
    root: Path, docs: Sequence[tuple[str, int, str]], manifests: Sequence[ScanTarget]
) -> list[Finding]:
    """Version/date statements in the packaged metadata.

    Offline this can only be a consistency check, so both checks are REVIEW.
    """
    findings: list[Finding] = []

    # 1. version claimed for a named package vs the manifest's own version
    manifest_versions: dict[str, tuple[str, str]] = {}
    for target in manifests:
        try:
            data = json.loads(_read_text(target.abspath))
        except (ValueError, TypeError):
            continue
        if isinstance(data, dict) and isinstance(data.get("name"), str) and isinstance(
            data.get("version"), str
        ):
            manifest_versions[data["name"]] = (data["version"], target.rel)

    for rel, lineno, text in docs:
        if rel not in SUBMISSION_DOC_SET:
            continue
        for name, (version, manifest_rel) in sorted(manifest_versions.items()):
            if name not in text:
                continue
            claimed = VERSION_RE.findall(text)
            if not claimed:
                continue
            for token in claimed:
                if token.lstrip("v") != version:
                    findings.append(
                        Finding(
                            rule="HG-REV-010",
                            severity=SEV_REVIEW,
                            file=rel,
                            line=lineno,
                            message="document states version '{}' for package '{}' but {} "
                            "declares '{}'".format(token, name, manifest_rel, version),
                            evidence=_snippet(text),
                            correction="Make the packaged metadata and the documents state "
                            "the same version.",
                        )
                    )
                    break

    # 2. future-dated statements in judge-facing documents
    head_date = _git(root, "log", "-1", "--format=%cs").stdout.strip()
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", head_date or ""):
        for rel, lineno, text in docs:
            if rel not in SUBMISSION_DOC_SET:
                continue
            for year, month, day in DATE_RE.findall(text):
                stamp = "{}-{}-{}".format(year, month, day)
                if stamp <= head_date or DEADLINE_CONTEXT.search(text):
                    continue
                findings.append(
                    Finding(
                        rule="HG-REV-011",
                        severity=SEV_REVIEW,
                        file=rel,
                        line=lineno,
                        message="document states date {} which is later than the tip commit "
                        "date {}; the statement cannot be backed by the published "
                        "history".format(stamp, head_date),
                        evidence=_snippet(text),
                        correction="Reword projected dates as projections, or record the "
                        "actual date once it happens.",
                    )
                )
    return findings


def _git(root: Path, *args: str) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            ["git", "-C", str(root), *args],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return subprocess.CompletedProcess(args, returncode=127, stdout="", stderr="")


# --------------------------------------------------------------------------
# Rule 4 - possible published credentials
# --------------------------------------------------------------------------

#: Ordered: specific key shapes first, generic assignments after, topology last.
SECRET_PATTERNS: tuple[tuple[str, re.Pattern], ...] = (
    (
        # 43 base64 chars + '=' padding.  A trailing \b can never match after
        # '=', so lookarounds are required or the pattern never fires.
        "wireguard-key",
        re.compile(r"(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{43}=(?![A-Za-z0-9+/=])"),
    ),
    (
        "private-key-header",
        re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY-----"),
    ),
    ("aws-access-key-id", re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("github-token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    (
        "secret-assignment",
        re.compile(
            r"(?i)\b(?:api[_-]?key|apikey|secret|client[_-]?secret|password|passwd"
            r"|private[_-]?key|access[_-]?token|auth[_-]?token|bearer)\b\s*[:=]\s*"
            r"[\"']?([A-Za-z0-9_\-\.\+/]{12,})"
        ),
    ),
    (
        "private-ipv4-topology",
        re.compile(
            r"\b(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}"
            r"|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b"
        ),
    ),
    (
        "public-ipv4-literal",
        re.compile(
            r"\b(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
            r"(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}\b"
        ),
    ),
)

#: Values that are obviously placeholders, not secrets.
PLACEHOLDER_RE = re.compile(
    r"^(?:x{4,}|\.{3,}|<[^>]*>|\$\{?[A-Za-z_]+\}?|[A-Z_]{4,}|(?:redacted|example|sample"
    r"|placeholder|changeme|none|null|true|false|empty|todo))$",
    re.IGNORECASE,
)

#: IPv4 literals that are never a disclosure (unspecified / loopback / broadcast).
IP_EXEMPT = frozenset({"0.0.0.0", "127.0.0.1", "255.255.255.255"})

CREDENTIAL_RULE_IDS = {
    "wireguard-key": "HG-CRED-001",
    "private-key-header": "HG-CRED-002",
    "aws-access-key-id": "HG-CRED-003",
    "github-token": "HG-CRED-004",
    "google-api-key": "HG-CRED-005",
    "secret-assignment": "HG-CRED-006",
    "private-ipv4-topology": "HG-CRED-007",
    "public-ipv4-literal": "HG-CRED-008",
}

CREDENTIAL_CLASS_CORRECTION = (
    "Rotate the value, remove it from the repository, and replace it with a "
    "placeholder or a reference to a secret store."
)
TOPOLOGY_CLASS_CORRECTION = (
    "Replace the address with a placeholder (an RFC 5737 documentation range or a "
    "named placeholder such as <vpn-address>) if the document is judge-facing."
)


def _is_private_ip(value: str) -> bool:
    first = value.split(".", 1)[0]
    return first in ("10", "192", "172")


def scan_credentials(
    docs: Sequence[tuple[str, int, str]]
) -> tuple[list[Finding], list[str]]:
    """Report secret-shaped content without ever carrying the value forward.

    Returns (findings, sensitive_values).  `sensitive_values` holds the raw
    matches so the caller can run `enforce_redaction` over the finished report -
    the raw values never enter a Finding.
    """
    findings: list[Finding] = []
    sensitive: list[str] = []
    seen: set[tuple] = set()

    for rel, lineno, text in docs:
        for index, (klass, pattern) in enumerate(SECRET_PATTERNS):
            for match in pattern.finditer(text):
                value = match.group(1) if match.groups() else match.group(0)
                if not value:
                    continue
                if klass == "public-ipv4-literal":
                    if value in IP_EXEMPT or _is_private_ip(value):
                        continue  # handled/ignored by the private-IPv4 rule
                    # Skip version-like strings that merely look like dotted quads.
                    if value.startswith(("1.2.3", "0.0.", "1.0.0")):
                        continue
                elif klass == "secret-assignment":
                    if PLACEHOLDER_RE.match(value):
                        continue
                sensitive.append(value)
                digest = _sha256_8(value)
                key = (klass, rel, lineno, digest)
                if key in seen:
                    continue
                seen.add(key)
                is_secret = klass not in ("private-ipv4-topology", "public-ipv4-literal")
                rule_id = CREDENTIAL_RULE_IDS[klass]
                if is_secret:
                    severity = SEV_FAIL
                    message = (
                        "possible published credential: pattern class '{}' matched "
                        "(value withheld; length {}, sha256[:8] {})".format(
                            klass, len(value), digest
                        )
                    )
                    correction = CREDENTIAL_CLASS_CORRECTION
                else:
                    severity = SEV_REVIEW
                    message = (
                        "network address disclosed in document text: pattern class '{}' "
                        "(value withheld; length {}, sha256[:8] {})".format(
                            klass, len(value), digest
                        )
                    )
                    correction = TOPOLOGY_CLASS_CORRECTION
                findings.append(
                    Finding(
                        rule=rule_id,
                        severity=severity,
                        file=rel,
                        line=lineno,
                        message=message,
                        evidence="[redacted: class={} len={} sha256[:8]={}]".format(
                            klass, len(value), digest
                        ),
                        correction=correction,
                        klass=klass,
                        value_length=len(value),
                        value_sha256_8=digest,
                    )
                )
    return findings, sensitive


# --------------------------------------------------------------------------
# Rule 5 - physical-test precision
# --------------------------------------------------------------------------

PHONE_TERM_RE = re.compile(
    r"\bmi ?a3\b|mi-a3|\bphone\b|android node|the handset", re.IGNORECASE
)
REMOTE_COUPLING_RE = re.compile(
    r"\bmcp\b|remote access|remote reachab\w*|public ingress|\bvpn\b|wireguard",
    re.IGNORECASE,
)
IMPLIES_TESTING_RE = re.compile(
    r"\b(works?|working|tested|verified|proven|demonstrated|acceptance|live|passes"
    r"|passing|reachable|hosted|running|exposed|connected)\b",
    re.IGNORECASE,
)
CAMERA_CAPTURE_RE = re.compile(
    r"physical camera|camera capture|capture(?:d|s)? on (?:the )?(?:physical|real)"
    r"|real camera|v4l2 capture",
    re.IGNORECASE,
)
PHYSICAL_CONFIRMED_RE = re.compile(
    r"\b(verified|tested|captured|passed|proven|confirmed|works?|green|acceptance)\b",
    re.IGNORECASE,
)


def scan_physical_precision(
    docs: Sequence[tuple[str, int, str]]
) -> list[Finding]:
    findings: list[Finding] = []
    for rel, lineno, text in docs:
        stripped = text.strip()
        if not stripped or stripped.startswith(("```", "|")):
            continue
        # (a) phone coupled with remote/MCP/VPN in a way that implies testing
        if (
            PHONE_TERM_RE.search(text)
            and REMOTE_COUPLING_RE.search(text)
            and IMPLIES_TESTING_RE.search(text)
            and not NEGATED.search(text)
        ):
            findings.append(
                Finding(
                    rule="HG-PHYS-001",
                    severity=SEV_REVIEW,
                    file=rel,
                    line=lineno,
                    message="statement couples the phone/Mi A3 with MCP, remote access or "
                    "VPN while implying a test that a physical device would have to "
                    "perform",
                    evidence=_snippet(text),
                    correction="Say explicitly whether this was exercised on the physical "
                    "phone (with the evidence) or only through a loopback/mock path.",
                )
            )
        # (b) physical camera capture claims - kept separate so the two are not
        #     conflated: this class *has* been verified on real hardware.
        if (
            CAMERA_CAPTURE_RE.search(text)
            and PHYSICAL_CONFIRMED_RE.search(text)
            and not NEGATED.search(text)
        ):
            findings.append(
                Finding(
                    rule="HG-PHYS-002",
                    severity=SEV_REVIEW,
                    file=rel,
                    line=lineno,
                    message="statement reports physical camera capture; this class is "
                    "hardware-verified and must not be merged with phone-hosted or remote "
                    "claims elsewhere",
                    evidence=_snippet(text),
                    correction="Keep this claim scoped to camera capture and do not let it "
                    "imply the phone/VPN/remote paths were likewise verified.",
                )
            )
    return findings


# --------------------------------------------------------------------------
# Orchestration + output
# --------------------------------------------------------------------------


@dataclass
class Report:
    root: str
    findings: list[Finding] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    stats: dict = field(default_factory=dict)
    sensitive_values: list[str] = field(default_factory=list)

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

    def to_dict(self) -> dict:
        return {
            "program": PROGRAM,
            "root": self.root,
            "counts": self.counts(),
            "total": len(self.findings),
            "findings": [f.to_dict() for f in self.findings],
            "notes": self.notes,
            "stats": self.stats,
        }


def _doc_line_stream(
    root: Path, targets: Sequence[ScanTarget]
) -> list[tuple[str, int, str]]:
    stream: list[tuple[str, int, str]] = []
    for target in targets:
        if target.kind not in ("doc", "licence"):
            continue
        for lineno, text in enumerate(_lines(target.abspath), 1):
            stream.append((target.rel, lineno, text))
    return stream


def run_scan(
    root: Path,
    paths: Sequence[str] | None = None,
    severity_min: str = SEV_REVIEW,
    doc_scope: str = "all",
) -> Report:
    targets = collect_targets(root, paths)
    report = Report(root=str(root))
    report.stats["files_scanned"] = len(targets)
    report.stats["docs_scanned"] = sum(
        1 for t in targets if t.kind in ("doc", "licence")
    )
    report.stats["manifests_scanned"] = sum(1 for t in targets if t.kind == "manifest")

    doc_lines = _doc_line_stream(root, targets)
    if doc_scope == "submission":
        allowed = set(SUBMISSION_DOC_SET)
        doc_lines = [d for d in doc_lines if d[0] in allowed]
        report.stats["doc_scope"] = "submission"
    else:
        report.stats["doc_scope"] = "all"

    findings: list[Finding] = []

    # Rule 1
    findings += scan_licensing(root, targets)
    # Rule 2 (sentence-stitched: wrapped prose must not look like a claim)
    claim_findings, claim_skipped = scan_claims(stitch_statements(doc_lines), root)
    findings += claim_findings
    report.stats["claim_lines_skipped_normative"] = claim_skipped
    # Rule 3
    rev_findings, rev_stats = scan_revision_references(root, doc_lines)
    findings += rev_findings
    report.stats["revision"] = rev_stats
    findings += scan_metadata_currency(
        root, doc_lines, [t for t in targets if t.kind == "manifest"]
    )
    # Rule 4
    cred_findings, sensitive = scan_credentials(doc_lines)
    findings += cred_findings
    report.sensitive_values = sensitive
    # Rule 5
    findings += scan_physical_precision(doc_lines)

    # Target-absence notes (informational; never a finding).
    for rel in (RELEASE_README, *DEVPOST_DOCS, VIDEO_SCRIPT, *PACKAGE_DOCS):
        if _classify(rel) is not None and not (root / rel).exists():
            report.notes.append("target artefact not present in this tree: {}".format(rel))
    if not rev_stats["git"]:
        report.notes.append(
            "not a git repository: revision existence/ancestry checks were skipped"
        )
    if rev_stats["exempt"]:
        report.notes.append(
            "{} hex token occurrence(s) exempted as non-revision context "
            "(digest/fingerprint/device-id)".format(rev_stats["exempt"])
        )
    if severity_min == SEV_FAIL:
        report.notes.append("severity-min=FAIL: REVIEW findings filtered from the report")

    filtered = [f for f in findings if _passes_severity(f, severity_min)]
    report.findings = _sort_findings(filtered)
    # Mandatory guard: refuse to emit anything that embeds a matched value.
    enforce_redaction(report.findings, report.sensitive_values)
    return report


def _passes_severity(finding: Finding, severity_min: str) -> bool:
    if severity_min == SEV_FAIL:
        return finding.severity == SEV_FAIL
    return True


def render_text(report: Report) -> str:
    counts = report.counts()
    out: list[str] = []
    out.append("{} - offline submission compliance scan".format(PROGRAM))
    out.append("root: {}".format(report.root))
    out.append(
        "files scanned: {files_scanned} ({docs_scanned} doc/licence, "
        "{manifests_scanned} manifest)".format(**report.stats)
    )
    rev = report.stats.get("revision", {})
    out.append(
        "revision tokens: {considered} considered, {exempt} exempt (non-revision "
        "context), {checked} checked against git".format(**rev)
        if rev
        else "revision tokens: not checked"
    )
    out.append(
        "findings by severity: FAIL={} REVIEW={} (total {})".format(
            counts[SEV_FAIL], counts[SEV_REVIEW], len(report.findings)
        )
    )
    out.append("")
    if not report.findings:
        out.append("no findings")
    for severity in (SEV_FAIL, SEV_REVIEW):
        group = [f for f in report.findings if f.severity == severity]
        if not group:
            continue
        out.append("=== {} ({}) ===".format(severity, len(group)))
        for finding in group:
            out.append("[{}] {} {}:{}".format(
                finding.rule, finding.severity, finding.file, finding.line
            ))
            out.append("    {}".format(finding.message))
            if finding.klass is not None:
                out.append(
                    "    pattern_class={} value_length={} value_sha256_8={}".format(
                        finding.klass, finding.value_length, finding.value_sha256_8
                    )
                )
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
        description="Offline compliance scanner for a hackathon submission tree.",
    )
    parser.add_argument(
        "tree",
        nargs="?",
        default=".",
        help="submission tree to scan (default: current directory)",
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        default=None,
        metavar="PATH",
        help="restrict the scan to these files/directories (relative to the tree)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the findings as JSON instead of the human-readable report",
    )
    parser.add_argument(
        "--severity-min",
        choices=(SEV_FAIL, SEV_REVIEW),
        default=SEV_REVIEW,
        help="minimum severity to report (default: REVIEW)",
    )
    parser.add_argument(
        "--doc-scope",
        choices=("all", "submission"),
        default="all",
        help="scan all docs, or only the judge-facing submission doc set",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="print nothing; only the exit code matters",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    root = Path(args.tree)
    if not root.is_dir():
        print("error: {} is not a directory".format(root), file=sys.stderr)
        return EXIT_INTERNAL
    try:
        report = run_scan(
            root,
            paths=args.paths,
            severity_min=args.severity_min,
            doc_scope=args.doc_scope,
        )
    except RedactionGuardError as exc:
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
