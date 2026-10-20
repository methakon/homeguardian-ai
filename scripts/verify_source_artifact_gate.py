#!/usr/bin/env python3
"""verify_source_artifact_gate.py - source-artifact assurance gate.

Purpose
-------
A single, offline, dependency-free check that takes ONE produced source
artifact (the ``*-source.tar.gz`` release archive, the output directory that
holds it together with its ``.sha256`` / ``-manifest.txt`` companions, or an
extracted package tree) and answers one question with one exit code:

    is this artifact fit to be handed over as a *source* artifact?

``scripts/check_release_package.py`` answers "is this artifact acceptable on its
own terms"; ``scripts/check_release_manifest.py`` answers "has this artifact
changed since the baseline was recorded".  This gate is the thin admission
layer over both: it does NOT re-implement any scanner.  It CALLS
``check_release_package.audit_package()`` and
``check_release_manifest.verify_manifest()`` and applies the six admission
rules below to their results, escalating the specific conditions a
source-artifact gate must refuse to a hard FAIL.

The six checks
--------------
  A  LICENCE PRESENT.  The artifact carries a ``LICENSE`` (the checker's
     ``HG-PKG-LIC-001``), and the gate re-confirms the licence file is really
     present as a package entry rather than trusting the absence of a finding.
  B  NOTICE PRESENT.  The artifact carries a ``NOTICE`` (``HG-PKG-LIC-002``).
     A NOTICE with no attribution content is REVIEW (``HG-PKG-LIC-004``): the
     required file exists, so it is not a FAIL, but it is not a pass either.
  C  SOURCE REVISION RECORDED.  The artifact records the revision it was cut
     from (``stats.revision.full_sha_records``).  No record at all is FAIL; a
     record that is only a truncated prefix is REVIEW; a full record that the
     repository named with ``--repo`` does not contain is FAIL.
  D  MANIFEST DIGEST CONSISTENCY.  Any recorded checksum beside the archive
     must match the archive (``HG-PKG-REV-004``), and, when a recorded manifest
     is available (``--manifest``, else a sibling ``*-manifest.txt``), the
     artifact is re-verified against it.  With no digest record to compare, D is
     REVIEW - it is never silently a pass.
  E  NO UNEXPECTED CRITICAL FILES.  A source artifact must not carry
     key/credential files (``HG-PKG-LEAK-001``), anything the packaging script's
     own exclusion list forbids (``HG-PKG-LEAK-002``), build output or
     ``node_modules`` (``HG-PKG-LEAK-004``), or local state and captured media
     (``HG-PKG-LEAK-005``).  The underlying checker reports some of these as
     REVIEW because a *general* audit cannot assume the artifact is a source
     release; this gate can, so it escalates them to FAIL.  A credential-shaped
     template (``HG-PKG-LEAK-007``) stays REVIEW.
  F  NO UNEXPLAINED SECRET-SCANNER FINDINGS.  Every credential-shaped value
     finding (``HG-PKG-LEAK-003``) must be either a genuine FAIL or named by an
     entry in the reviewed allow-list.  That single rule is what makes an
     un-reviewed heuristic downgrade - a real secret that some marker pattern
     merely *looks* like a placeholder - a gate FAIL rather than a quiet REVIEW.
     The check also refuses to pass when a finding was silently swallowed (the
     credential-shape counters must reconcile with the findings that were
     reported) or when the allow-list carries a dangling entry that matches no
     finding.  An allow-listed finding is REVIEW, never a suppression:
     ``exit 2`` is not a pass.

Answering "what counts as critical" without duplicating a scanner
-----------------------------------------------------------------
Every path/value classification above is the reused checker's own
(``CREDENTIAL_NAME_RES``, the packaging script's forbidden list,
``BUILD_ARTIFACT_NAME_RES``, ``LOCAL_STATE_NAME_RES``, ``CAPTURED_MEDIA_RES``,
``SECRET_PATTERNS``).  This gate adds only the ADMISSION POLICY - which of
those findings block a source hand-over - and never a second pattern set.

Redaction discipline
--------------------
Reused verbatim: a matched value is never printed.  Gate findings carry the
pattern class, the value length and ``sha256[:8]`` of the value, and nothing
else; findings copied out of the underlying audit are the already-masked,
already-redaction-guarded objects.  This script authors no message from a
matched value.

Exit codes
----------
  0  PASS    - all six checks PASS
  1  FAIL    - at least one check FAIL
  2  REVIEW  - no check FAIL, at least one check REVIEW  (NOT a pass)
  3  INTERNAL- the artifact could not be read, or the reused checkers are
              unavailable (a missing sibling scanner)

Usage
-----
  python3 scripts/verify_source_artifact_gate.py ARTIFACT [--json]
        [--manifest BASELINE.json] [--expected-license Apache-2.0]
        [--package-script FILE] [--secret-allowlist FILE] [--repo DIR]
        [--quiet]

Wiring it into scripts/verify_judge_release.sh (release branch - owner's call)
----------------------------------------------------------------------------
One line, placed after the packaging has produced the artifact and before the
final verdict summary.  Point ARTIFACT_DIR at the packaging output directory
(the archive with its .sha256 / .sha512 / -manifest.txt companions):

  python3 "$REPO_ROOT/scripts/verify_source_artifact_gate.py" "${ARTIFACT_DIR:-$REPO_ROOT/dist}" --package-script "$REPO_ROOT/scripts/package_release.sh" || { log_fail "source-artifact assurance gate did not pass (exit $?; 2 = REVIEW, which is NOT a pass)"; exit 1; }

Two things that line depends on, both of which are why the `|| { ...; exit 1; }`
half is not optional:

  * the harness's `log_fail` only increments `FAIL_COUNT`, and the script's
    final verdict is `VERDICT: PASS` + `exit 0` unconditionally (the other
    failure paths in the harness all call `exit 1` explicitly), so a gate that
    only called `log_fail` would be inert - the harness must exit non-zero;
  * exit 2 is a REVIEW and NOT a pass, so it has to fail the harness too.

For another harness (stable interface, no printing, no side effects)
--------------------------------------------------------------------
  import sys
  sys.path.insert(0, "<repo>/scripts")
  from verify_source_artifact_gate import verify_source_artifact, exit_code_for

  result = verify_source_artifact("/path/to/artifact.tar.gz")
  code = exit_code_for(result)          # 0 / 1 / 2, same contract as the CLI

``verify_source_artifact()`` prints nothing, writes nothing outside the private
temporary directories the reused checkers create, never calls ``sys.exit``, and
raises ``GateInputError`` on an unreadable artifact or unavailable checker.

Two ways this can fail that are worth knowing about
---------------------------------------------------
 * ``ScannerUnavailable`` propagates as ``GateInputError``; the gate exits 3.
   A missing checker must never read as a pass.
 * If the artifact cannot be loaded at all, the report is still returned (when
   ``lenient``) with A-F all FAIL or REVIEW as applicable - see
   ``GateInputError`` handling in :func:`verify_source_artifact`.

History, licensing and the packaging are NOT touched by this script: it is a
read-only admission check for an already-produced artifact.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Iterable, Sequence

# --------------------------------------------------------------------------
# Reuse, do not duplicate: the sibling checkers
# --------------------------------------------------------------------------

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

try:
    import check_release_package as crp
except Exception as exc:  # pragma: no cover - import-time failure
    crp = None
    _CRP_IMPORT_ERROR = exc
else:
    _CRP_IMPORT_ERROR = None

try:
    import check_release_manifest as crm
except Exception as exc:  # pragma: no cover - import-time failure
    crm = None
    _CRM_IMPORT_ERROR = exc
else:
    _CRM_IMPORT_ERROR = None


PROGRAM = "verify_source_artifact_gate"
REPORT_FORMAT = 1

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_REVIEW = 2
EXIT_INTERNAL = 3

SEV_OK = "PASS"
SEV_FAIL = "FAIL"
SEV_REVIEW = "REVIEW"
STATUSES = (SEV_OK, SEV_FAIL, SEV_REVIEW)


class GateInputError(RuntimeError):
    """The artifact cannot be gated, or a reused checker is unavailable."""


#: The six admission checks, in report order.
GATE_CHECKS: tuple[tuple[str, str], ...] = (
    ("A", "licence-present"),
    ("B", "notice-present"),
    ("C", "source-revision-recorded"),
    ("D", "manifest-digest-consistency"),
    ("E", "no-unexpected-critical-files"),
    ("F", "secret-findings-explained"),
)

CHECK_NAMES = {key: name for key, name in GATE_CHECKS}


def _check_sort_key(check: str) -> int:
    order = [key for key, _ in GATE_CHECKS]
    return order.index(check) if check in order else len(order)


# --------------------------------------------------------------------------
# Admission policy: which reused findings block a source hand-over
# --------------------------------------------------------------------------

#: Reused LEAK rules that a SOURCE artifact must not exhibit at all.  The
#: general audit reports some of these as REVIEW; a source hand-over refuses
#: them, which is the whole point of a gate.
CRITICAL_LEAK_RULES: dict[str, str] = {
    "HG-PKG-LEAK-001": "credential-shaped file (key material) is packaged",
    "HG-PKG-LEAK-002": "a file the packaging script's own exclusion list "
                       "forbids is packaged",
    "HG-PKG-LEAK-004": "build output / intermediates are packaged",
    "HG-PKG-LEAK-005": "local state or captured media is packaged",
}

#: Reused LEAK rules that stay REVIEW at gate level.
SOFT_LEAK_RULES: dict[str, str] = {
    "HG-PKG-LEAK-006": "the packaging exclusion list and the checker's copy "
                       "have drifted",
    "HG-PKG-LEAK-007": "a credential-shaped TEMPLATE file is packaged; it is "
                       "not itself a secret, confirm it holds placeholders",
}

#: The credential VALUE rule: the one check F is about.
CREDENTIAL_VALUE_RULE = "HG-PKG-LEAK-003"

#: Reused check_release_manifest rule ids, split by what they mean for check D
#: ("manifest DIGEST consistency").  The digest and file-set rules drive D's
#: verdict; a baseline whose recorded revision CONTRADICTS the artifact does
#: too; a revision recorded on only one side is an incompleteness check C owns.
BASELINE_DIGEST_RULES = frozenset({
    "HG-MF-001",  # recorded file is missing from the package (FAIL)
    "HG-MF-002",  # a file is present that the baseline does not record (REVIEW)
    "HG-MF-003",  # recorded content changed (FAIL)
    "HG-MF-006",  # recorded archive digest does not match the archive (FAIL)
    "HG-MF-007",  # the manifest lists no per-file digests, so the check could
                  # not be performed (REVIEW)
    "HG-MF-008",  # recorded and actual file counts disagree (REVIEW)
})
BASELINE_REVISION_CONTRADICTION_RULES = frozenset({"HG-MF-004"})
BASELINE_NOTE_ONLY_RULES = frozenset({"HG-MF-005"})

#: Reused rule id -> the gate check that owns it.  Anything not listed is left
#: to the underlying checkers and reported in ``underlying`` + notes.
RULE_TO_CHECK: dict[str, str] = {
    "HG-PKG-LIC-001": "A",
    "HG-PKG-LIC-002": "B",
    "HG-PKG-LIC-004": "B",
    "HG-PKG-REV-001": "C",
    "HG-PKG-REV-002": "C",
    "HG-PKG-REV-003": "C",
    "HG-PKG-REV-004": "D",
}
for _rule in CRITICAL_LEAK_RULES:
    RULE_TO_CHECK[_rule] = "E"
for _rule in SOFT_LEAK_RULES:
    RULE_TO_CHECK[_rule] = "E"
RULE_TO_CHECK[CREDENTIAL_VALUE_RULE] = "F"

#: Basenames that satisfy the required LICENSE / NOTICE presence check.  The
#: checker requires the exact top-level names; these tolerate the conventional
#: suffixed spellings when cross-confirming the file really is packaged.
LICENCE_BASENAMES = frozenset({
    "license", "licence", "license.txt", "licence.txt", "license.md",
    "licence.md", "copying", "copying.txt",
})
NOTICE_BASENAMES = frozenset({
    "notice", "notice.txt", "notice.md", "notices", "notices.txt",
})

MANIFEST_SUFFIXES = ("-manifest.txt", "-manifest.json", ".manifest.json")


# --------------------------------------------------------------------------
# Findings
# --------------------------------------------------------------------------

def _gate_finding(rule: str, severity: str, check: str, subject: str,
                  message: str, correction: str, *, path: str = "",
                  evidence: str = "", pattern_class: str | None = None,
                  value_length: int | None = None,
                  value_sha256_8: str | None = None) -> dict:
    """One gate finding.

    ``pattern_class`` / ``value_length`` / ``value_sha256_8`` are the ONLY
    credential detail this tool carries: never the value, never a snippet of it.
    """
    finding = {
        "rule": rule,
        "severity": severity,
        "check": check,
        "check_name": CHECK_NAMES.get(check, "unclassified"),
        "path": path or subject,
        "message": message,
        "correction": correction,
    }
    if evidence:
        finding["evidence"] = evidence
    if pattern_class is not None:
        finding["pattern_class"] = pattern_class
        finding["value_length"] = value_length
        finding["value_sha256_8"] = value_sha256_8
    return finding


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def _walk_entries(root: Path | None) -> list[str]:
    """Package-relative posix paths of every regular file under ``root``.

    Returns an empty list when ``root`` is not a readable directory: the reused
    audit extracts a tarball into a private temporary directory and removes it
    before returning, so for an archive input the extracted tree is gone by the
    time the gate sees the result.  Callers must therefore treat "not walkable"
    as "no independent cross-check available", never as "absent".
    """
    out: list[str] = []
    if root is None:
        return out
    root = Path(root)
    if not root.is_dir():
        return out
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            p = Path(dirpath) / name
            try:
                rel = p.relative_to(root).as_posix()
            except ValueError:  # pragma: no cover - defensive
                continue
            out.append(rel)
    return out


def _find_by_basename(entries: Iterable[str], names: frozenset) -> str | None:
    """First entry whose basename is in ``names`` (preferring the shallowest)."""
    for rel in sorted(entries, key=lambda r: (r.count("/"), r)):
        if rel.rsplit("/", 1)[-1].lower() in names:
            return rel
    return None


def _finding_credential_key(finding: dict) -> tuple:
    """The reviewed-allow-list key for a credential finding.

    Mirrors ``check_release_package.apply_secret_allowlist``: all four of file,
    line, pattern class and value ``sha256[:8]`` must agree.
    """
    return (
        finding.get("file", ""),
        int(finding.get("line") or 0),
        finding.get("pattern_class") or "",
        (finding.get("value_sha256_8") or "").lower(),
    )


def _copy_reused(finding: dict, check: str) -> dict:
    """Copy a reused checker finding into the gate report.

    The copied dict is the underlying object's already-masked, already-guarded
    content: ``message`` has been through the checker's ``_sanitize`` and the
    scanner's ``enforce_redaction``, and no value is carried on any field.
    """
    out = {
        "rule": finding.get("rule", "?"),
        "severity": finding.get("severity", SEV_REVIEW),
        "check": check,
        "check_name": CHECK_NAMES.get(check, "unclassified"),
        "path": finding.get("file", ""),
        "line": finding.get("line", 0),
        "message": finding.get("message", ""),
    }
    if finding.get("correction"):
        out["correction"] = finding["correction"]
    if finding.get("pattern_class") is not None:
        out["pattern_class"] = finding.get("pattern_class")
        out["value_length"] = finding.get("value_length")
        out["value_sha256_8"] = finding.get("value_sha256_8")
    return out


def _resolve_allowlist(explicit):
    """The allow-list the reused audit resolves: flag, then env, then sibling."""
    if crp is None:
        return None
    return crp.resolve_secret_allowlist(explicit)


def _load_allowlist(path):
    if crp is None or path is None:
        return {}, []
    return crp.load_secret_allowlist(path)


def _discover_manifest(artifact: Path) -> Path | None:
    """A recorded manifest beside the artifact, when the caller named none."""
    artifact = Path(artifact)
    directory = artifact if artifact.is_dir() else artifact.parent
    if not directory.is_dir():
        return None
    candidates = sorted(
        p for p in directory.iterdir()
        if p.is_file() and p.name.endswith(MANIFEST_SUFFIXES)
    )
    if not candidates:
        return None
    stem = artifact.name.split("-source.tar")[0]
    matching = [p for p in candidates if stem in p.name]
    return matching[0] if matching else candidates[0]


# --------------------------------------------------------------------------
# The six checks
# --------------------------------------------------------------------------

def _check_a_licence(audit: dict, root: Path | None) -> tuple[str, str, list, dict]:
    findings = []
    reused = [f for f in audit.get("findings", [])
              if f.get("rule") == "HG-PKG-LIC-001"]
    entries = _walk_entries(root)
    walkable = bool(entries) or (root is not None and Path(root).is_dir())
    present = _find_by_basename(entries, LICENCE_BASENAMES)
    evidence = {
        "licence_entry": present,
        "entries_walked": len(entries),
        "independently_walked": walkable,
        "confirmed_by": None,
    }
    if reused:
        for f in reused:
            findings.append(_copy_reused(f, "A"))
        evidence["confirmed_by"] = "LIC-001 finding"
        return SEV_FAIL, "LICENSE is absent from the artifact", findings, evidence
    if walkable and present is None:
        # The checker raised no finding, but the gate can independently see the
        # tree and cannot find the file: do not report a pass on a presence
        # check that contradicts what is on disk.
        findings.append(_gate_finding(
            "HG-GATE-A-002", SEV_FAIL, "A", "LICENSE",
            "the underlying audit raised no missing-LICENSE finding, but no "
            "licence-named file could be found in the package tree, so the "
            "required-licence check could not be confirmed",
            "Re-run the packaging so LICENSE is included, then re-gate.",
            path="LICENSE",
        ))
        evidence["confirmed_by"] = "independent walk contradicted the audit"
        return SEV_FAIL, "LICENSE presence could not be confirmed", findings, \
            evidence
    if present is not None:
        evidence["confirmed_by"] = "package entry {}".format(present)
        return SEV_OK, "LICENSE present at {}".format(present), findings, evidence
    # Archive input: the extracted tree is gone, so the checker's own check 1
    # raising no HG-PKG-LIC-001 is the positive confirmation.
    evidence["confirmed_by"] = "check 1 raised no HG-PKG-LIC-001"
    return SEV_OK, "LICENSE present (check 1 raised no HG-PKG-LIC-001)", \
        findings, evidence


def _check_b_notice(audit: dict, root: Path | None) -> tuple[str, str, list, dict]:
    findings = []
    missing = [f for f in audit.get("findings", [])
               if f.get("rule") == "HG-PKG-LIC-002"]
    soft = [f for f in audit.get("findings", [])
            if f.get("rule") == "HG-PKG-LIC-004"]
    entries = _walk_entries(root)
    walkable = bool(entries) or (root is not None and Path(root).is_dir())
    present = _find_by_basename(entries, NOTICE_BASENAMES)
    evidence = {
        "notice_entry": present,
        "entries_walked": len(entries),
        "independently_walked": walkable,
        "attribution_content": None,
        "confirmed_by": None,
    }
    if missing:
        for f in missing:
            findings.append(_copy_reused(f, "B"))
        evidence["confirmed_by"] = "LIC-002 finding"
        return SEV_FAIL, "NOTICE is absent from the artifact", findings, evidence
    if walkable and present is None:
        findings.append(_gate_finding(
            "HG-GATE-B-002", SEV_FAIL, "B", "NOTICE",
            "the underlying audit raised no missing-NOTICE finding, but no "
            "notice-named file could be found in the package tree, so the "
            "required-notice check could not be confirmed",
            "Re-run the packaging so NOTICE is included, then re-gate.",
            path="NOTICE",
        ))
        evidence["confirmed_by"] = "independent walk contradicted the audit"
        return SEV_FAIL, "NOTICE presence could not be confirmed", findings, \
            evidence
    for f in soft:
        findings.append(_copy_reused(f, "B"))
    evidence["attribution_content"] = not soft
    evidence["confirmed_by"] = (
        "package entry {}".format(present) if present is not None
        else "check 1 raised no HG-PKG-LIC-002")
    if soft:
        return SEV_REVIEW, ("NOTICE present{}, but it carries no attribution "
                            "content".format(
                                " at {}".format(present) if present else "")), \
            findings, evidence
    return SEV_OK, "NOTICE present{}".format(
        " at {}".format(present) if present else ""), findings, evidence


def _check_c_revision(audit: dict) -> tuple[str, str, list, dict]:
    findings = []
    rev = ((audit.get("stats") or {}).get("revision") or {})
    full = rev.get("full_sha_records") or []
    short = rev.get("short_records") or []
    present_rules = {f.get("rule") for f in audit.get("findings", [])}
    evidence = {
        "full_sha_records": sorted(set(full)),
        "full_sha_record_count": len(set(full)),
        "short_records": sorted(set(short)),
    }
    for f in audit.get("findings", []):
        if f.get("rule") in ("HG-PKG-REV-001", "HG-PKG-REV-002",
                             "HG-PKG-REV-003"):
            findings.append(_copy_reused(f, "C"))
    if "HG-PKG-REV-003" in present_rules:
        return SEV_FAIL, ("the recorded revision is not a commit in the "
                          "repository given with --repo"), findings, evidence
    if not full and not short:
        if not findings:
            findings.append(_gate_finding(
                "HG-GATE-C-001", SEV_FAIL, "C", "<artifact>",
                "the artifact records no source revision anywhere: no manifest "
                "field, no revision file and no SHA in its name",
                "Have the packaging write the full source revision into the "
                "artifact, then re-gate.",
            ))
        return SEV_FAIL, "no source revision is recorded", findings, evidence
    if not full:
        if not findings:
            findings.append(_gate_finding(
                "HG-GATE-C-002", SEV_REVIEW, "C", "<artifact>",
                "the source revision is recorded only as a truncated prefix, so "
                "the artifact alone cannot identify the revision",
                "Emit the full 40-hex revision into the artifact.",
            ))
        return SEV_REVIEW, ("only a truncated revision prefix is recorded "
                            "({} record(s))".format(len(short))), findings, evidence
    return SEV_OK, "{} full source revision record(s)".format(len(set(full))), \
        findings, evidence


def _check_d_manifest(audit: dict, artifact: Path, manifest: Path | None
                      ) -> tuple[str, str, list, dict]:
    findings = []
    evidence: dict = {"recorded_checksum_findings": 0, "checksums_verified": [],
                      "baseline": None}
    statuses: list[str] = []

    # D.1 - recorded checksums beside the archive must match the archive.
    local = [f for f in audit.get("findings", [])
             if f.get("rule") == "HG-PKG-REV-004"]
    evidence["recorded_checksum_findings"] = len(local)
    for f in local:
        findings.append(_copy_reused(f, "D"))
    if local:
        statuses.append(SEV_FAIL)

    # D.1b - an *agreeing* recorded checksum is a verified digest record, so it
    # must not fall through to "nothing to check".  The comparison reuses the
    # reused checker's own digest helpers rather than re-implementing them.
    archive_name = (audit.get("stats") or {}).get("archive")
    if archive_name and crp is not None:
        directory = artifact if artifact.is_dir() else artifact.parent
        archive_file = directory / archive_name
        if archive_file.is_file():
            for algo in [s.lstrip(".") for s in crp.CHECKSUM_SUFFIXES]:
                recorded_file = directory / "{}.{}".format(archive_name, algo)
                if not recorded_file.is_file():
                    continue
                recorded = crp._recorded_digest(recorded_file, archive_name)
                actual = crp._digest_file(archive_file, algo)
                if not recorded or not actual:
                    continue
                if recorded == actual:
                    evidence["checksums_verified"].append(algo)
                else:
                    findings.append(_gate_finding(
                        "HG-GATE-D-001", SEV_FAIL, "D", archive_name,
                        "the recorded {} digest in {} does not match the "
                        "archive it describes".format(algo.upper(),
                                                      recorded_file.name),
                        "Regenerate the checksums from the archive that is "
                        "actually being published, then re-gate.",
                        path=recorded_file.name,
                    ))
                    statuses.append(SEV_FAIL)
            if evidence["checksums_verified"]:
                statuses.append(SEV_OK)

    # D.2 - the recorded baseline, when there is one, must still verify.
    manifest = manifest or _discover_manifest(artifact)
    if manifest is not None:
        try:
            verified = crm.verify_manifest(artifact, manifest)
        except Exception as exc:
            findings.append(_gate_finding(
                "HG-GATE-D-004", SEV_REVIEW, "D", str(manifest),
                "a recorded manifest was found, but it could not be verified "
                "({}: {})".format(type(exc).__name__, exc),
                "Re-record the baseline from the artifact that is being "
                "released, then re-gate.",
                path=Path(manifest).name,
            ))
            statuses.append(SEV_REVIEW)
        else:
            counts = verified.get("counts") or {}
            baseline_findings = verified.get("findings", [])
            evidence["baseline"] = {
                "manifest": str(manifest),
                "manifest_format": verified.get("manifest_format"),
                "summary": (verified.get("summary") or {}).get("counts"),
                "checks": verified.get("checks"),
                "text_mode_limits": (verified.get("checks") or {}).get(
                    "text_mode_limits"),
            }
            for f in baseline_findings:
                severity = f.get("severity", SEV_REVIEW)
                findings.append(_gate_finding(
                    f.get("rule", "HG-MF-000"), severity, "D",
                    str(manifest), f.get("message", ""),
                    f.get("correction", ""), path=f.get("path", ""),
                ))
            # D's own dimension is the manifest's digest and file-set
            # consistency.  A baseline whose recorded source revision
            # CONTRADICTS the artifact (HG-MF-004) is a provenance failure and
            # fails D too; HG-MF-005 (a revision recorded on only one side) is
            # an incompleteness that check C owns, so it is surfaced as a note
            # instead of driving D's verdict.
            driving = [f for f in baseline_findings
                       if f.get("rule") in BASELINE_DIGEST_RULES
                       or f.get("rule") in BASELINE_REVISION_CONTRADICTION_RULES]
            note_only = [f for f in baseline_findings
                         if f.get("rule") in BASELINE_NOTE_ONLY_RULES]
            for f in note_only:
                evidence.setdefault("baseline_notes", []).append(
                    "{}: {}".format(f.get("rule"), (f.get("message") or "")[:160]))
            checks = verified.get("checks") or {}
            if any(f.get("severity") == SEV_FAIL for f in driving) or \
                    counts.get(SEV_FAIL):
                statuses.append(SEV_FAIL)
            elif any(f.get("severity") == SEV_REVIEW for f in driving):
                statuses.append(SEV_REVIEW)
            elif checks.get("digests") == SEV_FAIL:
                statuses.append(SEV_FAIL)
            elif checks.get("digests") == "PASS" and checks.get("files") == "PASS":
                statuses.append(SEV_OK)
            else:
                statuses.append(SEV_REVIEW)
    else:
        evidence["baseline"] = None

    # D.3 - no digest record at all is a REVIEW, never a pass.
    if not statuses:
        findings.append(_gate_finding(
            "HG-GATE-D-004", SEV_REVIEW, "D", "<artifact>",
            "no digest record could be checked: the artifact carries no "
            "recorded checksum beside an archive and no recorded manifest was "
            "supplied or found, so digest consistency was not verified",
            "Validate the packaging output directory (the archive with its "
            ".sha256 and -manifest.txt), or pass --manifest for a recorded "
            "baseline, then re-gate.",
        ))
        statuses.append(SEV_REVIEW)

    if SEV_FAIL in statuses:
        return SEV_FAIL, "a recorded digest disagrees with the artifact", \
            findings, evidence
    if SEV_REVIEW in statuses:
        return SEV_REVIEW, "digest consistency could not be fully verified", \
            findings, evidence
    verified_desc = ", ".join(evidence["checksums_verified"]) or "baseline only"
    return SEV_OK, "recorded digests agree with the artifact ({})".format(
        verified_desc), findings, evidence


def _check_e_critical(audit: dict) -> tuple[str, str, list, dict]:
    findings = []
    stats = ((audit.get("stats") or {}).get("packaging") or {})
    reused = audit.get("findings", [])
    hard = [f for f in reused if f.get("rule") in CRITICAL_LEAK_RULES]
    soft = [f for f in reused if f.get("rule") in SOFT_LEAK_RULES]
    for f in hard:
        findings.append(_copy_reused(f, "E"))
    for f in soft:
        findings.append(_copy_reused(f, "E"))

    evidence = {
        "entries": stats.get("entries", 0),
        "content_files_reported_by_checker": stats.get("content_files", 0),
        "lines_scanned": stats.get("lines_scanned", 0),
        "pruned_dirs": stats.get("pruned_dirs", 0),
        "critical_findings": len(hard),
        "soft_findings": len(soft),
    }
    # Non-vacuity: a scan that did not run must not read as a pass.  The
    # signal is entries classified plus content lines actually scanned;
    # ``content_files`` is initialised but never maintained by the reused
    # checker, so it is reported but NOT used to decide anything.
    if not evidence["entries"] or not evidence["lines_scanned"]:
        findings.append(_gate_finding(
            "HG-GATE-E-002", SEV_REVIEW, "E", "<artifact>",
            "the packaging scan did not classify any packaged content ({} "
            "entr{}, {} line(s) scanned), so 'no unexpected critical files' "
            "was not actually checked".format(
                evidence["entries"],
                "y" if evidence["entries"] == 1 else "ies",
                evidence["lines_scanned"]),
            "Point the gate at the packaging output directory (the archive "
            "with its companions) or an extracted package tree, then re-gate.",
        ))
        return SEV_REVIEW, "the packaging scan did not run", findings, evidence
    if hard:
        return SEV_FAIL, ("{} unexpected critical file(s) are packaged".format(
            len(hard))), findings, evidence
    if soft:
        return SEV_REVIEW, ("{} item(s) need review: {}".format(
            len(soft), ", ".join(sorted({f.get("rule", "?") for f in soft}))) ), \
            findings, evidence
    return SEV_OK, "no unexpected critical files are packaged", findings, evidence


def _check_f_secrets(audit: dict, allowlist_arg) -> tuple[str, str, list, dict]:
    findings = []
    reused = audit.get("findings", [])
    stats = ((audit.get("stats") or {}).get("packaging") or {})
    shapes = stats.get("credential_value_shapes") or {}
    value_findings = [f for f in reused
                      if f.get("rule") == CREDENTIAL_VALUE_RULE]

    allowlist_path = _resolve_allowlist(allowlist_arg)
    keys, allowlist_notes = _load_allowlist(allowlist_path)
    matched_keys = set()
    for f in value_findings:
        key = _finding_credential_key(f)
        if key in keys:
            matched_keys.add(key)
    dangling = sorted(set(keys) - matched_keys)

    evidence = {
        "allowlist_path": str(allowlist_path) if allowlist_path else None,
        "allowlist_entries": len(keys),
        "allowlist_matched": len(matched_keys),
        "allowlist_dangling": len(dangling),
        "allowlist_applied_by_audit": (
            (audit.get("stats") or {}).get("secret_allowlist") or {}
        ).get("applied"),
        "credential_value_findings": len(value_findings),
        "shapes": {k: shapes.get(k, 0)
                   for k in ("literal", "reference", "marker")},
        "allowlist_notes": allowlist_notes,
    }

    # F.4 - nothing silently swallowed: the shape counters the scanner produced
    # must reconcile with the findings that were actually reported.
    shaped_total = sum(shapes.get(k, 0) for k in ("literal", "reference", "marker"))
    evidence["shaped_total"] = shaped_total
    if shaped_total != len(value_findings):
        findings.append(_gate_finding(
            "HG-GATE-F-004", SEV_FAIL, "F", "<secrets>",
            "the credential-shape counters ({}) do not reconcile with the "
            "credential value findings that were reported ({}), so a finding "
            "was silently dropped or reclassified".format(
                shaped_total, len(value_findings)),
            "Investigate the audit's secret handling before releasing: a "
            "credential finding that reaches no report is not a pass.",
        ))

    genuine = 0
    unexplained = 0
    allowlisted = 0
    for f in value_findings:
        key = _finding_credential_key(f)
        if key in keys:
            allowlisted += 1
            findings.append(_copy_reused(f, "F"))
            continue
        if f.get("severity") == SEV_FAIL:
            genuine += 1
            findings.append(_copy_reused(f, "F"))
        else:
            # A value the underlying checker DOWNGRADED on a heuristic (a
            # placeholder/negative-test label, or a code reference) with no
            # reviewed allow-list entry to explain it.  That is exactly how a
            # real secret escapes as a quiet REVIEW, so at gate level it fails.
            unexplained += 1
            findings.append(_gate_finding(
                "HG-GATE-F-002", SEV_FAIL, "F",
                f.get("file", "<file>"),
                "credential-shaped value was downgraded to {} by a heuristic "
                "(no reviewed allow-list entry names it), so it is unexplained "
                "and cannot be waived".format(f.get("severity")),
                "Either remove/rotate the value, or - if it is a reviewed "
                "synthetic fixture - add an allow-list entry with all four of "
                "{}.".format(", ".join(crp.ALLOWLIST_KEYS) if crp else
                             "file, line, pattern_class, value_sha256_8"),
                path=f.get("file", ""),
                pattern_class=f.get("pattern_class"),
                value_length=f.get("value_length"),
                value_sha256_8=f.get("value_sha256_8"),
            ))
            findings[-1]["line"] = f.get("line", 0)

    for key in dangling:
        findings.append(_gate_finding(
            "HG-GATE-F-003", SEV_FAIL, "F", key[0],
            "the reviewed allow-list carries an entry that matches no "
            "credential finding in this artifact (a dangling entry), so it can "
            "only be covering a value that has changed or is gone",
            "Remove the stale entry, or re-review the artifact and record the "
            "entry that matches the value actually present.",
            path=key[0],
            pattern_class=key[2],
            value_sha256_8=key[3],
        ))

    evidence["genuine"] = genuine
    evidence["unexplained_downgrades"] = unexplained
    evidence["allowlisted"] = allowlisted

    blocking = bool(dangling) or genuine or unexplained or (
        shaped_total != len(value_findings))
    if blocking:
        return SEV_FAIL, (
            "{} genuine credential finding(s), {} unexplained downgrade(s), "
            "{} dangling allow-list entry/entries".format(
                genuine, unexplained, len(dangling))
        ), findings, evidence
    if allowlisted:
        return SEV_REVIEW, ("{} credential finding(s) allow-listed for review "
                            "(not suppressed)".format(allowlisted)), findings, \
            evidence
    return SEV_OK, "no credential-shaped value findings", findings, evidence


def _overall(checks: dict) -> str:
    statuses = {c["status"] for c in checks.values()}
    if SEV_FAIL in statuses:
        return SEV_FAIL
    if SEV_REVIEW in statuses:
        return SEV_REVIEW
    return SEV_OK


def verify_source_artifact(artifact, *, manifest=None, expected_licence=None,
                          package_script=None, secret_allowlist=None,
                          repo=None, max_scan_bytes: int = 2 * 1024 * 1024
                          ) -> dict:
    """Gate one artifact.  Returns a report dict; never prints, never exits.

    Raises :class:`GateInputError` when the artifact cannot be read or a reused
    checker is unavailable - both of which must never read as a pass.
    """
    if _CRP_IMPORT_ERROR is not None or crp is None:
        raise GateInputError(
            "check_release_package.py is unavailable ({}), so the artifact "
            "cannot be gated".format(_CRP_IMPORT_ERROR))
    if _CRM_IMPORT_ERROR is not None or crm is None:
        raise GateInputError(
            "check_release_manifest.py is unavailable ({}), so the artifact "
            "cannot be gated".format(_CRM_IMPORT_ERROR))

    artifact_path = Path(artifact)
    if not artifact_path.exists():
        raise GateInputError("{}: no such file or directory".format(artifact))

    try:
        audit = crp.audit_package(
            artifact_path,
            expected_licence=expected_licence,
            package_script=package_script,
            repo=repo,
            include_topology=False,
            max_scan_bytes=max_scan_bytes,
            severity_min=SEV_REVIEW,
        )
    except crp.ScannerUnavailable as exc:
        raise GateInputError(
            "a scanner the reused checker needs is unavailable: {}".format(exc)
        ) from None
    except crp.PackageInputError as exc:
        raise GateInputError(str(exc)) from None

    root = Path(audit["package_root"]) if audit.get("package_root") else None

    checks: dict[str, dict] = {}
    for key, _name in GATE_CHECKS:
        checks[key] = {"check": key, "name": CHECK_NAMES[key], "status": SEV_OK,
                       "detail": "", "findings": [], "evidence": {}}

    def record(key: str, result) -> None:
        status, detail, finding_list, evidence = result
        checks[key]["status"] = status
        checks[key]["detail"] = detail
        checks[key]["findings"] = finding_list
        checks[key]["evidence"] = evidence

    record("A", _check_a_licence(audit, root))
    record("B", _check_b_notice(audit, root))
    record("C", _check_c_revision(audit))
    record("D", _check_d_manifest(
        audit, artifact_path, Path(manifest) if manifest else None))
    record("E", _check_e_critical(audit))
    record("F", _check_f_secrets(audit, secret_allowlist))

    findings: list[dict] = []
    for key, _name in GATE_CHECKS:
        findings.extend(checks[key]["findings"])
    findings.sort(key=lambda f: (f.get("severity") != SEV_FAIL,
                                 _check_sort_key(f.get("check", "")),
                                 f.get("rule", ""), f.get("path", "")))

    # Anything the underlying audit reported that no gate check owns is
    # surfaced, so nothing is hidden behind the gate's six rules.
    unmapped = {}
    for f in audit.get("findings", []):
        if f.get("rule") in RULE_TO_CHECK:
            continue
        unmapped.setdefault(f.get("rule", "?"), 0)
        unmapped[f.get("rule", "?")] += 1

    notes = [
        "checks A-F are a SOURCE-ARTIFACT admission policy over the reused "
        "checkers ({} and {}); this gate implements no scanner of its own".format(
            "check_release_package.audit_package", "check_release_manifest"
            ".verify_manifest"),
        "EXIT 2 (REVIEW) IS NOT A PASS: a REVIEW means at least one of the six "
        "checks could not be confirmed and the artifact needs a human decision",
    ]
    if unmapped:
        notes.append(
            "findings the reused audit reported that no gate check owns (left "
            "to the caller's own audit run): {}".format(
                ", ".join("{} x{}".format(rule, count)
                          for rule, count in sorted(unmapped.items()))))
    if audit.get("packaging_rules", {}).get("source"):
        notes.append("packaging rules read from {}".format(
            audit["packaging_rules"]["source"]))

    status = _overall(checks)
    counts = {SEV_OK: 0, SEV_REVIEW: 0, SEV_FAIL: 0}
    for key, _name in GATE_CHECKS:
        counts[checks[key]["status"]] += 1

    return {
        "program": PROGRAM,
        "format": REPORT_FORMAT,
        "input": str(artifact),
        "artifact": {
            "origin": audit.get("origin"),
            "package_root": audit.get("package_root"),
            "prefix": audit.get("prefix"),
            "archive": (audit.get("stats") or {}).get("archive"),
        },
        "check_names": {key: CHECK_NAMES[key] for key, _ in GATE_CHECKS},
        "checks": checks,
        "status": status,
        "check_counts": counts,
        "underlying": {
            "audit": {
                "program": audit.get("program"),
                "counts": audit.get("counts"),
                "total": audit.get("total"),
                "checks": audit.get("checks"),
                "stats": audit.get("stats"),
            },
            "unowned_findings": unmapped,
        },
        "findings": findings,
        "total": len(findings),
        "notes": notes,
    }


def exit_code_for(result: dict) -> int:
    """Exit code for a :func:`verify_source_artifact` result.

    ``PASS`` -> 0, ``FAIL`` -> 1, ``REVIEW`` -> 2 (NOT a pass), anything else
    (an unreadable artifact or a missing checker) -> 3.
    """
    status = (result or {}).get("status")
    if status == SEV_OK:
        return EXIT_OK
    if status == SEV_FAIL:
        return EXIT_FAIL
    if status == SEV_REVIEW:
        return EXIT_REVIEW
    return EXIT_INTERNAL


def render_text(result: dict) -> str:
    lines = [
        "verify_source_artifact_gate: {}".format(result.get("input")),
        "artifact: {} (origin {}, prefix {})".format(
            result.get("artifact", {}).get("archive") or "<directory tree>",
            result.get("artifact", {}).get("origin"),
            result.get("artifact", {}).get("prefix")),
        "",
    ]
    for key, name in GATE_CHECKS:
        check = result["checks"][key]
        lines.append("[{}] {} {} - {}".format(
            check["status"], key, name, check["detail"]))
    counts = result.get("check_counts") or {}
    lines.append("")
    lines.append("checks: {} PASS / {} REVIEW / {} FAIL | overall {} -> exit {}".format(
        counts.get(SEV_OK, 0), counts.get(SEV_REVIEW, 0), counts.get(SEV_FAIL, 0),
        result.get("status"), exit_code_for(result)))
    if result.get("findings"):
        lines.append("")
        for f in result["findings"]:
            lines.append("  {} {} {} {}: {}".format(
                f.get("severity"), f.get("rule"), f.get("check"),
                f.get("path") or "-", f.get("message")))
            if f.get("pattern_class") is not None:
                lines.append(
                    "      (pattern class '{}', value withheld; length {}, "
                    "sha256[:8] {})".format(
                        f.get("pattern_class"), f.get("value_length"),
                        f.get("value_sha256_8")))
    if result.get("notes"):
        lines.append("")
        for note in result["notes"]:
            lines.append("note: {}".format(note))
    return "\n".join(lines)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROGRAM,
        description="Gate one produced source artifact (archive, output "
                    "directory or extracted tree) on six admission checks; "
                    "exit 0 PASS, 1 FAIL, 2 REVIEW (not a pass), 3 internal.",
    )
    parser.add_argument("artifact", help="the artifact to gate: a release "
                        "archive, the output directory that holds it, or an "
                        "extracted package tree")
    parser.add_argument("--manifest", default=None, metavar="BASELINE",
                        help="a recorded manifest to verify the artifact "
                             "against (default: a sibling *-manifest.txt or "
                             "*.json when one exists)")
    parser.add_argument("--expected-license", default=None, metavar="LICENCE",
                        help="the licence the artifact is expected to record")
    parser.add_argument("--package-script", default=None, metavar="FILE",
                        help="read MANDATORY_FILES and the exclusion list from "
                             "this packaging script instead of the default")
    parser.add_argument("--secret-allowlist", default=None, metavar="FILE",
                        help="the reviewed secret allow-list naming synthetic "
                             "credential fixtures by file, line, pattern class "
                             "and value sha256[:8]")
    parser.add_argument("--repo", default=None, metavar="DIR",
                        help="corroborate the recorded revision against this "
                             "git repository")
    parser.add_argument("--json", action="store_true",
                        help="emit the machine-readable report")
    parser.add_argument("--quiet", action="store_true",
                        help="suppress stdout; the exit code still reports")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        result = verify_source_artifact(
            args.artifact,
            manifest=args.manifest,
            expected_licence=args.expected_license,
            package_script=args.package_script,
            secret_allowlist=args.secret_allowlist,
            repo=args.repo,
        )
    except GateInputError as exc:
        if not args.quiet:
            sys.stderr.write("{}: {}\n".format(PROGRAM, exc))
        return EXIT_INTERNAL
    if not args.quiet:
        if args.json:
            sys.stdout.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
        else:
            sys.stdout.write(render_text(result) + "\n")
    return exit_code_for(result)


if __name__ == "__main__":  # pragma: no cover - CLI entry point
    sys.exit(main())
