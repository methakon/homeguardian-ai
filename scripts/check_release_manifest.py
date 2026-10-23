#!/usr/bin/env python3
"""check_release_manifest.py - release-MANIFEST regression checker.

Purpose
-------
``check_release_package.py`` audits one produced package on its own terms: are
the mandatory files there, is the licence right, are secrets packaged, is the
dependency set attributed.  It answers "is this artefact acceptable in
absolute terms".

This checker answers a different question: **has this artefact changed since the
manifest was recorded?**  A recorded manifest is a baseline - a list of the files
the package contained, what each one hashed to, and which source revision the
package was cut from.  Re-verifying a package against that baseline detects the
regressions a release manager actually gets bitten by:

  * a file the manifest records is gone from the package;
  * a file appeared in the package that the manifest does not record;
  * a file's content changed (recorded sha256 or size differs);
  * the package was cut from a different source revision than the manifest
    records;
  * the archive beside the manifest no longer matches the digest the manifest
    records, so the provenance record and the artefact disagree.

What is a "recorded manifest" here
----------------------------------
Two forms are read, selected with ``--manifest-format`` (default ``auto``):

``json``
    The baseline this tool writes with ``--record``: a machine-readable record
    of every file, its size and its sha256, plus the source revisions and the
    archive digests.  This is the form that can detect *all four* classes of
    regression.

``text``
    The ``*-manifest.txt`` that ``scripts/package_release.sh`` emits.  It is a
    human-readable summary, so it only records the archive digests, the source
    revisions and the file count.  Text mode therefore cannot detect a renamed
    or edited file; it checks the digests, the revisions and the file count, and
    says so in ``checks.text_mode_limits`` rather than pretending otherwise.

``auto``
    ``json`` when the manifest parses as JSON, ``text`` otherwise.

Recording a baseline
--------------------
    python3 scripts/check_release_manifest.py --record PACKAGE -o baseline.json

is the intended workflow: record once from the artefact that was actually
released, commit the baseline, then re-verify any later candidate against it.

Importable API
--------------
    import sys
    sys.path.insert(0, "<repo>/scripts")
    from check_release_manifest import verify_manifest, record_manifest, exit_code_for

    baseline = record_manifest("/path/to/package")
    result = verify_manifest("/path/to/package", "/path/to/baseline.json")
    code = exit_code_for(result)          # 0 / 1 / 2

Neither function prints, writes outside a private temporary directory, or calls
``sys.exit``.  ``record_manifest`` returns the baseline dict; ``verify_manifest``
returns a JSON-serialisable report dict documented in ``REPORT_SCHEMA`` below.
The CLI is a thin wrapper over these.

Package loading
---------------
Directory and tarball inputs are resolved by ``check_release_package.load_package``
so the two tools agree on what "a package" is; that module is imported, not
re-implemented.  A ``ScannerUnavailable`` error is raised if it is missing.

Rule ids
--------
``HG-MF-001`` file recorded but missing (FAIL)
``HG-MF-002`` file present but not recorded (REVIEW)
``HG-MF-003`` recorded content changed - sha256 or size differs (FAIL)
``HG-MF-004`` source revision differs from the recorded one (FAIL)
``HG-MF-005`` a source revision is recorded on only one side (REVIEW)
``HG-MF-006`` recorded archive digest does not match the archive (FAIL)
``HG-MF-007`` a check could not be performed against this manifest (unreadable
              baseline, or the human-readable form records no per-file data)
              (REVIEW)
``HG-MF-008`` recorded file count and actual file count disagree (REVIEW)

Exit codes
----------
  0  no FAIL and no REVIEW findings
  1  at least one FAIL finding
  2  at least one REVIEW finding and no FAIL
  3  internal error (unreadable input, missing dependency)

REPORT_SCHEMA
-------------
  {
    "program": "check_release_manifest",   # str, constant
    "format": 1,                           # int, report format version
    "package": "<path as given>",          # str
    "manifest": "<path as given>",         # str
    "manifest_format": "json"|"text",      # str
    "origin": "tarball"|"directory",       # str
    "package_root": "<abs path>",          # str
    "package_name": "<archive or dir name>",
    "summary": {
        "recorded_files": int, "present_files": int,
        "missing": int, "unexpected": int, "changed": int,
        "counts": {"FAIL": int, "REVIEW": int}, "total": int
    },
    "source": {                            # recorded vs actual revisions
        "recorded_revision": str|null, "recorded_security": str|null,
        "recorded_prep": str|null, "actual_revision": str|null,
        "package_name_prefixes": [str, ...]
    },
    "archive": {"recorded_sha256": str|null, "actual_sha256": str|null,
                "recorded_sha512": str|null, "actual_sha512": str|null},
    "checks": {"files": str, "digests": str, "revision": str,
               "text_mode_limits": str},
    "counts": {"FAIL": int, "REVIEW": int},
    "total": int,
    "findings": [
        {"rule": str, "severity": "FAIL"|"REVIEW", "path": str,
         "recorded_sha256": str|null, "actual_sha256": str|null,
         "recorded_size": int|null, "actual_size": int|null,
         "message": str, "correction": str}
    ],
    "notes": [str, ...]
  }

Nothing in this report contains file *contents*: only paths, sizes and digests.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

PROGRAM = "check_release_manifest"
REPORT_FORMAT = 1
BASELINE_FORMAT = 1

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_REVIEW = 2
EXIT_INTERNAL = 3

SEV_FAIL = "FAIL"
SEV_REVIEW = "REVIEW"
SEVERITIES = (SEV_FAIL, SEV_REVIEW)

RULE_SEVERITY = {
    "HG-MF-001": SEV_FAIL,
    "HG-MF-002": SEV_REVIEW,
    "HG-MF-003": SEV_FAIL,
    "HG-MF-004": SEV_FAIL,
    "HG-MF-005": SEV_REVIEW,
    "HG-MF-006": SEV_FAIL,
    "HG-MF-007": SEV_REVIEW,
    "HG-MF-008": SEV_REVIEW,
}

CHECK_NAMES = {
    "files": "recorded-file-set",
    "digests": "archive-digests",
    "revision": "source-revision",
}

HASH_CHUNK = 1 << 20

#: The text manifest ``scripts/package_release.sh`` emits.
TEXT_FIELDS = {
    "security_baseline": re.compile(
        r"^[ \t]*Security[ \t]+Baseline[ \t]+SHA[ \t]*:[ \t]*(?P<v>[0-9a-f]{7,40})\b",
        re.M),
    "prep": re.compile(
        r"^[ \t]*Prep[ \t]*/[ \t]*Docs[ \t]+SHA[ \t]*:[ \t]*(?P<v>[0-9a-f]{7,40})\b",
        re.M),
    "sha256": re.compile(
        r"^[ \t]*SHA-256[ \t]*:[ \t]*(?P<v>[0-9a-f]{64})\b", re.M),
    "sha512": re.compile(
        r"^[ \t]*SHA-512[ \t]*:[ \t]*(?P<v>[0-9a-f]{128})\b", re.M),
    "archive": re.compile(
        r"^[ \t]*Archive[ \t]+File[ \t]*:[ \t]*(?P<v>\S+)[ \t]*$", re.M),
    "total_files": re.compile(
        r"^[ \t]*Total[ \t]+Files[ \t]*:[ \t]*(?P<v>[0-9]+)\b", re.M),
    "revision": re.compile(
        r"^[ \t]*(?:Source|Release|Prep|Commit|Revision)[ A-Za-z/_-]*SHA[ \t]*:"
        r"[ \t]*(?P<v>[0-9a-f]{7,40})\b", re.M | re.I),
}

#: ``homeguardian-ai-1.0.0-sec-<10 hex>-prep-<10 hex>`` - the name the packaging
#: script builds.  The two prefixes are the only revision record a bare archive
#: carries.
PACKAGE_NAME_RE = re.compile(
    r"(?P<slug>[A-Za-z0-9._-]+?)-sec-(?P<sec>[0-9a-f]{7,40})-prep-(?P<prep>[0-9a-f]{7,40})"
)
FULL_SHA_RE = re.compile(r"\b[0-9a-f]{40}\b")


class ManifestInputError(RuntimeError):
    """A package or manifest path this checker cannot read."""


# --------------------------------------------------------------------------
# Reuse of the package loader (imported, not re-implemented)
# --------------------------------------------------------------------------

def _import_package_checker():
    try:
        import check_release_package as module  # type: ignore
        return module
    except ImportError:
        here = Path(__file__).resolve().parent
        if str(here) not in sys.path:
            sys.path.insert(0, str(here))
        import check_release_package as module  # type: ignore
        return module


try:
    crp = _import_package_checker()
except Exception as _exc:  # pragma: no cover - exercised via main()
    crp = None
    _CRP_ERROR = _exc
else:
    _CRP_ERROR = None


def _require_package_checker():
    if crp is None:
        raise ManifestInputError(
            "check_release_package.py is required for package loading (it is "
            "imported, not duplicated): {}".format(_CRP_ERROR)
        )
    return crp


def _sha256_file(path: Path) -> str | None:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(HASH_CHUNK), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def _sha512_file(path: Path) -> str | None:
    digest = hashlib.sha512()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(HASH_CHUNK), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


@dataclass
class FileRecord:
    path: str
    size: int
    sha256: str


@dataclass
class Baseline:
    """A recorded manifest, from either a JSON baseline or a text manifest."""

    fmt: str
    files: dict[str, FileRecord] = field(default_factory=dict)
    revision: str | None = None
    security_baseline: str | None = None
    prep: str | None = None
    archive_name: str | None = None
    archive_sha256: str | None = None
    archive_sha512: str | None = None
    declared_file_count: int | None = None
    notes: list[str] = field(default_factory=list)


def record_manifest(package_path, *, archive_digests: bool = True) -> dict:
    """Record a baseline manifest for a package.  Returns a JSON-ready dict."""
    module = _require_package_checker()
    workdir = Path(tempfile.mkdtemp(prefix="hgmf-record-"))
    try:
        try:
            pkg, _notes = module.load_package(Path(package_path), workdir)
        except module.PackageInputError as exc:
            raise ManifestInputError(str(exc)) from None
        if pkg.root is None:
            raise ManifestInputError("{}: no package tree to record".format(
                package_path))

        files = []
        for rel, is_dir, pruned in module.iter_package_entries(pkg.root):
            if is_dir:
                continue
            path = pkg.root / rel
            sha = _sha256_file(path)
            if sha is None:
                continue
            try:
                size = path.stat().st_size
            except OSError:  # pragma: no cover - raced removal
                continue
            files.append({"path": rel, "size": size, "sha256": sha})
        files.sort(key=lambda item: item["path"])

        baseline = {
            "program": PROGRAM,
            "format": BASELINE_FORMAT,
            "recorded_at": None,
            "package": {
                "name": pkg.name,
                "origin": pkg.origin,
                "prefix": pkg.prefix,
                "file_count": len(files),
            },
            "source": _source_record(pkg),
            "archive": None,
            "files": files,
        }
        if archive_digests and pkg.archive is not None:
            baseline["archive"] = {
                "name": pkg.archive.name,
                "size": pkg.archive.stat().st_size,
                "sha256": _sha256_file(pkg.archive),
                "sha512": _sha512_file(pkg.archive),
            }
        return baseline
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def _source_record(pkg) -> dict:
    """The revision record of a Package: manifest/text fields and name prefixes."""
    record = {"revision": None, "security_baseline": None, "prep": None,
              "name_prefixes": {}}
    if pkg.release_manifest is not None:
        try:
            text = pkg.release_manifest.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        match = TEXT_FIELDS["security_baseline"].search(text)
        if match:
            record["security_baseline"] = match.group("v")
        match = TEXT_FIELDS["prep"].search(text)
        if match:
            record["prep"] = match.group("v")
        match = TEXT_FIELDS["revision"].search(text)
        if match:
            record["revision"] = match.group("v")
        if record["revision"] is None and record["prep"]:
            record["revision"] = record["prep"]
    name_match = PACKAGE_NAME_RE.search(pkg.name or "")
    if name_match:
        record["name_prefixes"] = {"sec": name_match.group("sec"),
                                   "prep": name_match.group("prep")}
    if record["security_baseline"] is None and record["name_prefixes"].get("sec"):
        record["security_baseline"] = record["name_prefixes"]["sec"]
    if record["prep"] is None and record["name_prefixes"].get("prep"):
        record["prep"] = record["name_prefixes"]["prep"]
    if record["revision"] is None:
        record["revision"] = record["prep"] or record["security_baseline"]
    return record


def read_baseline(path: Path) -> Baseline:
    """Read a recorded manifest in either supported form."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ManifestInputError("{}: {}".format(path, exc)) from None
    stripped = text.lstrip()
    if stripped.startswith("{"):
        return _read_json_baseline(path, text)
    return _read_text_baseline(path, text)


def _read_json_baseline(path: Path, text: str) -> Baseline:
    try:
        data = json.loads(text)
    except ValueError as exc:
        raise ManifestInputError("{}: not valid JSON ({})".format(path, exc)) from None
    if not isinstance(data, dict) or "files" not in data:
        raise ManifestInputError(
            "{}: JSON baseline carries no 'files' list, so no file can be "
            "verified against it".format(path)
        )
    baseline = Baseline(fmt="json")
    for entry in data.get("files") or []:
        if not isinstance(entry, dict) or "path" not in entry:
            continue
        baseline.files[str(entry["path"])] = FileRecord(
            path=str(entry["path"]),
            size=int(entry.get("size") or 0),
            sha256=str(entry.get("sha256") or ""),
        )
    source = data.get("source") or {}
    baseline.revision = source.get("revision")
    baseline.security_baseline = source.get("security_baseline")
    baseline.prep = source.get("prep")
    archive = data.get("archive") or {}
    baseline.archive_name = archive.get("name")
    baseline.archive_sha256 = archive.get("sha256")
    baseline.archive_sha512 = archive.get("sha512")
    package = data.get("package") or {}
    if package.get("file_count") is not None:
        baseline.declared_file_count = int(package["file_count"])
    if not baseline.files:
        raise ManifestInputError(
            "{}: the baseline records zero files, so there is nothing to "
            "verify".format(path)
        )
    return baseline


def _read_text_baseline(path: Path, text: str) -> Baseline:
    baseline = Baseline(fmt="text")
    found = {}
    for field_name, pattern in TEXT_FIELDS.items():
        match = pattern.search(text)
        if match:
            found[field_name] = match.group("v")
    if not found:
        raise ManifestInputError(
            "{}: not a JSON baseline and no recognisable manifest field "
            "(Security Baseline SHA / SHA-256 / Total Files) was found".format(path)
        )
    baseline.security_baseline = found.get("security_baseline")
    baseline.prep = found.get("prep")
    baseline.revision = found.get("revision") or found.get("prep")
    baseline.archive_name = found.get("archive")
    baseline.archive_sha256 = found.get("sha256")
    baseline.archive_sha512 = found.get("sha512")
    if found.get("total_files") is not None:
        baseline.declared_file_count = int(found["total_files"])
    baseline.notes.append(
        "the recorded manifest is the human-readable form, so it records only "
        "the archive digests, the source revisions and the file count; it "
        "cannot detect a renamed or edited file"
    )
    return baseline


# --------------------------------------------------------------------------
# Verification
# --------------------------------------------------------------------------

def _finding(rule: str, path: str, message: str, correction: str,
             *, recorded_sha256: str | None = None,
             actual_sha256: str | None = None,
             recorded_size: int | None = None,
             actual_size: int | None = None) -> dict:
    return {
        "rule": rule,
        "severity": RULE_SEVERITY[rule],
        "path": path,
        "recorded_sha256": recorded_sha256,
        "actual_sha256": actual_sha256,
        "recorded_size": recorded_size,
        "actual_size": actual_size,
        "message": message,
        "correction": correction,
    }


def _actual_files(root: Path, module) -> dict[str, tuple[int, str]]:
    """Return {rel: (size, sha256)} for every packaged regular file."""
    found: dict[str, tuple[int, str]] = {}
    for rel, is_dir, _pruned in module.iter_package_entries(root):
        if is_dir:
            continue
        path = root / rel
        sha = _sha256_file(path)
        if sha is None:
            continue
        try:
            size = path.stat().st_size
        except OSError:  # pragma: no cover
            continue
        found[rel] = (size, sha)
    return found


def _revision_matches(recorded: str | None, actual: str | None) -> bool | None:
    """True/False when both sides exist, None when the comparison is open."""
    if not recorded or not actual:
        return None
    recorded = recorded.lower()
    actual = actual.lower()
    return recorded == actual or recorded.startswith(actual) or actual.startswith(recorded)


def verify_manifest(package_path, manifest_path, *,
                    unexpected_severity: str = SEV_REVIEW,
                    check_archive_digests: bool = True) -> dict:
    """Verify a package against a recorded manifest.  Returns a report dict.

    Never prints, never calls ``sys.exit``, and writes nothing outside a private
    temporary directory.  Raises ``ManifestInputError`` when the package or the
    manifest cannot be read.
    """
    module = _require_package_checker()
    if unexpected_severity not in SEVERITIES:
        raise ManifestInputError(
            "unexpected_severity must be one of {}".format(", ".join(SEVERITIES))
        )
    manifest_file = Path(manifest_path)
    if not manifest_file.is_file():
        raise ManifestInputError("{}: no such manifest".format(manifest_file))
    baseline = read_baseline(manifest_file)

    workdir = Path(tempfile.mkdtemp(prefix="hgmf-verify-"))
    try:
        try:
            pkg, load_notes = module.load_package(Path(package_path), workdir)
        except module.PackageInputError as exc:
            raise ManifestInputError(str(exc)) from None
        if pkg.root is None:
            raise ManifestInputError("{}: no package tree".format(package_path))

        findings: list[dict] = []
        notes = list(baseline.notes) + list(load_notes)

        actual = _actual_files(pkg.root, module)
        recorded_paths = set(baseline.files)
        actual_paths = set(actual)

        missing: list[str] = []
        unexpected: list[str] = []
        changed: list[str] = []

        if baseline.fmt == "text":
            # The human-readable manifest records no per-file digests, so the
            # file set cannot be verified from it.  Reporting every packaged
            # file as "unexpected" would be a false-positive flood, so the
            # per-file checks are skipped and the limitation is raised as a
            # REVIEW finding as well as being stated in the notes - a check that
            # did not happen must not read as a pass.
            notes.append(
                "per-file checks were skipped: the recorded manifest is the "
                "human-readable form and lists no per-file digests"
            )
            findings.append(_finding(
                "HG-MF-007", "<package>",
                "the recorded manifest is the human-readable form, which lists "
                "no per-file digests, so the missing/unexpected/edited file "
                "checks could not be performed against it ({} file(s) are in "
                "the package)".format(len(actual)),
                "Record a machine-readable baseline with --record and compare "
                "against that to get per-file coverage; the human-readable "
                "manifest can only be checked for its digests, its source "
                "revisions and its file count.",
            ))
        else:
            missing = sorted(recorded_paths - actual_paths)
            for rel in missing:
                record = baseline.files[rel]
                findings.append(_finding(
                    "HG-MF-001", rel,
                    "the recorded manifest lists '{}' but the package no longer "
                    "carries it (recorded {} byte(s), sha256 {})".format(
                        rel, record.size, record.sha256[:12] or "not recorded"),
                    "Either restore '{}' in the package or, if the removal is "
                    "intended, re-record the baseline with --record and commit "
                    "the new manifest.".format(rel),
                    recorded_sha256=record.sha256 or None, recorded_size=record.size,
                ))

            unexpected = sorted(actual_paths - recorded_paths)
            for rel in unexpected:
                size, sha = actual[rel]
                findings.append(_finding(
                    "HG-MF-002", rel,
                    "'{}' is present in the package but the recorded manifest "
                    "does not list it ({} byte(s), sha256 {}); the package has "
                    "gained a file since the baseline was recorded".format(
                        rel, size, sha[:12]),
                    "Confirm the addition is intended, then re-record the "
                    "baseline with --record so the change is reviewed rather "
                    "than silent.",
                    actual_sha256=sha, actual_size=size,
                ))
            if unexpected_severity == SEV_FAIL:
                for finding in findings:
                    if finding["rule"] == "HG-MF-002":
                        finding["severity"] = SEV_FAIL

            for rel in sorted(recorded_paths & actual_paths):
                record = baseline.files[rel]
                size, sha = actual[rel]
                if record.sha256 and record.sha256 == sha:
                    continue
                if not record.sha256 and record.size == size:
                    continue
                changed.append(rel)
                same_size = record.size == size
                findings.append(_finding(
                    "HG-MF-003", rel,
                    "the content of '{}' changed since the baseline was "
                    "recorded: sha256 {} -> {}{}".format(
                        rel, record.sha256[:12] or "not recorded", sha[:12],
                        " (the size is unchanged at {} byte(s), so this is an "
                        "edit, not a truncation)".format(size) if same_size
                        else " ({} -> {} byte(s))".format(record.size, size)),
                    "Review the change to '{}'. If it is intended, re-record "
                    "the baseline; if not, restore the recorded content.".format(rel),
                    recorded_sha256=record.sha256 or None, actual_sha256=sha,
                    recorded_size=record.size, actual_size=size,
                ))

        # The declared file count is only a distinct signal when the per-file
        # checks could not explain the difference (i.e. text mode); reporting it
        # alongside HG-MF-001/002 would just restate the same change twice.
        per_file_change = any(
            f["rule"] in ("HG-MF-001", "HG-MF-002") for f in findings
        )
        if (baseline.declared_file_count is not None
                and baseline.declared_file_count != len(actual)
                and not per_file_change):
            findings.append(_finding(
                "HG-MF-008", "<package>",
                "the recorded manifest declares {} file(s) but the package "
                "carries {}".format(baseline.declared_file_count, len(actual)),
                "Re-record the baseline, or investigate why the package's file "
                "count no longer agrees with its own manifest.",
                recorded_size=baseline.declared_file_count,
                actual_size=len(actual),
            ))

        # --- source revision ------------------------------------------------
        source_record = _source_record(pkg)
        revision_checks: list[bool] = []
        actual_revision = source_record.get("revision")
        recorded_revision = baseline.revision
        verdict = _revision_matches(recorded_revision, actual_revision)
        if verdict is False:
            findings.append(_finding(
                "HG-MF-004", "<source revision>",
                "the package was cut from source revision {}, but the recorded "
                "manifest records {}; the artefact is not the revision the "
                "baseline describes".format(
                    str(actual_revision)[:12], str(recorded_revision)[:12]),
                "Re-package from the recorded revision, or re-record the "
                "baseline if the new revision is intended.",
                recorded_sha256=recorded_revision, actual_sha256=actual_revision,
            ))
            revision_checks.append(False)
        elif verdict is None:
            findings.append(_finding(
                "HG-MF-005", "<source revision>",
                "a source revision is recorded on only one side: manifest {} / "
                "package {}; the two cannot be compared".format(
                    recorded_revision or "none", actual_revision or "none"),
                "Have the packaging record the source revision in the manifest "
                "and re-run.",
            ))
            revision_checks.append(False)
        else:
            revision_checks.append(True)

        # --- archive digests -------------------------------------------------
        digest_checks: list[bool] = []
        recorded_sha256 = baseline.archive_sha256
        recorded_sha512 = baseline.archive_sha512
        actual_sha256 = _sha256_file(pkg.archive) if pkg.archive else None
        actual_sha512 = _sha512_file(pkg.archive) if pkg.archive else None
        if check_archive_digests and pkg.archive is not None:
            for algo, recorded, actual_digest in (
                ("SHA-256", recorded_sha256, actual_sha256),
                ("SHA-512", recorded_sha512, actual_sha512),
            ):
                if recorded and actual_digest:
                    if recorded.lower() != actual_digest.lower():
                        findings.append(_finding(
                            "HG-MF-006", pkg.archive.name,
                            "the recorded {} digest does not match the archive: "
                            "recorded {}, computed {}; the provenance record and "
                            "the artefact disagree".format(
                                algo, recorded[:16], actual_digest[:16]),
                            "Regenerate the recorded manifest from the archive "
                            "that is actually being published.",
                            recorded_sha256=recorded, actual_sha256=actual_digest,
                        ))
                        digest_checks.append(False)
                    else:
                        digest_checks.append(True)
                elif recorded and not actual_digest:
                    notes.append(
                        "the recorded manifest carries a {} digest and the input "
                        "is not an archive, so it could not be compared".format(
                            algo)
                    )
        elif recorded_sha256 and pkg.archive is None:
            notes.append(
                "the input is a directory tree, so the archive digests the "
                "manifest records could not be compared"
            )

        findings.sort(key=lambda f: (f["severity"] != SEV_FAIL, f["rule"], f["path"]))
        counts = {
            SEV_FAIL: sum(1 for f in findings if f["severity"] == SEV_FAIL),
            SEV_REVIEW: sum(1 for f in findings if f["severity"] == SEV_REVIEW),
        }
        files_status = "FAIL" if any(
            f["rule"] in ("HG-MF-001", "HG-MF-003") for f in findings
        ) else ("REVIEW" if any(f["rule"] in ("HG-MF-002", "HG-MF-008")
                               for f in findings) else "PASS")
        if baseline.fmt == "text":
            # No per-file data was recorded, so the file set was not verified at
            # all - reporting PASS here would claim a check that did not happen.
            files_status = "REVIEW"
        revision_status = (
            "PASS" if revision_checks and all(revision_checks)
            else ("REVIEW" if findings and any(
                f["rule"] == "HG-MF-005" for f in findings) else
                ("FAIL" if findings and any(f["rule"] == "HG-MF-004"
                                            for f in findings) else "REVIEW"))
        )
        digest_status = ("PASS" if digest_checks and all(digest_checks)
                         else ("FAIL" if any(f["rule"] == "HG-MF-006"
                                             for f in findings) else "REVIEW"))

        return {
            "program": PROGRAM,
            "format": REPORT_FORMAT,
            "package": str(package_path),
            "manifest": str(manifest_file),
            "manifest_format": baseline.fmt,
            "origin": pkg.origin,
            "package_root": str(pkg.root),
            "package_name": pkg.name,
            "summary": {
                "recorded_files": len(baseline.files),
                "present_files": len(actual),
                "missing": len(missing),
                "unexpected": len(unexpected),
                "changed": len(changed),
                "counts": counts,
                "total": len(findings),
            },
            "source": {
                "recorded_revision": recorded_revision,
                "recorded_security": baseline.security_baseline,
                "recorded_prep": baseline.prep,
                "actual_revision": actual_revision,
                "actual_security": source_record.get("security_baseline"),
                "actual_prep": source_record.get("prep"),
                "package_name_prefixes": source_record.get("name_prefixes"),
            },
            "archive": {
                "recorded_sha256": recorded_sha256,
                "actual_sha256": actual_sha256,
                "recorded_sha512": recorded_sha512,
                "actual_sha512": actual_sha512,
            },
            "checks": {
                "files": files_status,
                "digests": digest_status,
                "revision": revision_status,
                "text_mode_limits": (
                    "recorded manifest is the human-readable form: it records no "
                    "per-file digests, so missing, unexpected and edited files "
                    "cannot be detected from it"
                ) if baseline.fmt == "text" else "none",
            },
            "counts": counts,
            "total": len(findings),
            "findings": findings,
            "notes": notes,
        }
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def exit_code_for(result: dict) -> int:
    """Exit code for a :func:`verify_manifest` result (same contract as the CLI)."""
    counts = result.get("counts") or {}
    if counts.get(SEV_FAIL):
        return EXIT_FAIL
    if counts.get(SEV_REVIEW):
        return EXIT_REVIEW
    return EXIT_OK


# --------------------------------------------------------------------------
# Rendering + CLI
# --------------------------------------------------------------------------

def render_text(result: dict) -> str:
    summary = result["summary"]
    counts = result["counts"]
    out = ["{} - release-manifest regression check".format(PROGRAM)]
    out.append("package:  {}".format(result["package"]))
    out.append("manifest: {} ({})".format(result["manifest"],
                                          result["manifest_format"]))
    out.append("origin:   {} (package root: {})".format(
        result["origin"], result["package_root"]))
    out.append("")
    out.append("recorded file(s): {} | present: {} | missing: {} | unexpected: {} "
               "| changed: {}".format(
                   summary["recorded_files"], summary["present_files"],
                   summary["missing"], summary["unexpected"], summary["changed"]))
    out.append("checks: " + " | ".join(
        "{}={}".format(name, result["checks"][name])
        for name in ("files", "digests", "revision")))
    out.append("findings by severity: FAIL={} REVIEW={} (total {})".format(
        counts[SEV_FAIL], counts[SEV_REVIEW], result["total"]))
    out.append("")
    if not result["findings"]:
        out.append("no findings")
    for severity in (SEV_FAIL, SEV_REVIEW):
        group = [f for f in result["findings"] if f["severity"] == severity]
        if not group:
            continue
        out.append("=== {} ({}) ===".format(severity, len(group)))
        for finding in group:
            out.append("[{}] {} {}".format(finding["rule"], finding["severity"],
                                           finding["path"]))
            out.append("    {}".format(finding["message"]))
            out.append("    fix: {}".format(finding["correction"]))
        out.append("")
    if result["notes"]:
        out.append("=== notes ===")
        for note in result["notes"]:
            out.append("- {}".format(note))
    return "\n".join(out)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROGRAM,
        description="Detect missing, unexpected and changed files, and a "
                    "mismatched source revision, against a recorded release "
                    "manifest.",
    )
    parser.add_argument("package", nargs="?", default=".",
                        help="the release package to verify: a *-source.tar.gz "
                             "or the directory holding it")
    parser.add_argument("--manifest", default=None, metavar="FILE",
                        help="the recorded manifest (a JSON baseline written by "
                             "--record, or a *-manifest.txt).  Default: the "
                             "*-manifest.txt beside the package, if any")
    parser.add_argument("--manifest-format", choices=("auto", "json", "text"),
                        default="auto",
                        help="how to read --manifest (default: auto-detect)")
    parser.add_argument("--record", action="store_true",
                        help="record a baseline for the package and print it "
                             "(does not verify)")
    parser.add_argument("-o", "--output", default=None, metavar="FILE",
                        help="with --record: write the baseline to FILE "
                             "instead of stdout")
    parser.add_argument("--unexpected-severity", choices=SEVERITIES,
                        default=SEV_REVIEW,
                        help="severity for a file present but not recorded "
                             "(default: REVIEW)")
    parser.add_argument("--no-archive-digests", action="store_true",
                        help="skip comparing the recorded archive digests")
    parser.add_argument("--json", action="store_true",
                        help="emit the report as JSON instead of text")
    parser.add_argument("--quiet", action="store_true",
                        help="print nothing; only the exit code matters")
    return parser


def _default_manifest(package: Path) -> Path | None:
    """The ``*-manifest.txt`` beside an archive, if there is exactly one."""
    directory = package if package.is_dir() else package.resolve().parent
    candidates = sorted(
        path for path in directory.glob("*.txt") if "manifest" in path.name
    )
    return candidates[0] if len(candidates) == 1 else None


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    package = Path(args.package)

    if args.record:
        try:
            baseline = record_manifest(package)
        except (ManifestInputError, OSError) as exc:
            print("error: {}".format(exc), file=sys.stderr)
            return EXIT_INTERNAL
        text = json.dumps(baseline, indent=2, sort_keys=True)
        if args.output:
            Path(args.output).write_text(text + "\n", encoding="utf-8")
            if not args.quiet:
                print("recorded {} file(s) to {}".format(
                    len(baseline["files"]), args.output))
        elif not args.quiet:
            print(text)
        return EXIT_OK

    manifest = Path(args.manifest) if args.manifest else _default_manifest(package)
    if manifest is None:
        print("error: no --manifest given and no single *-manifest.txt found "
              "beside the package", file=sys.stderr)
        return EXIT_INTERNAL

    if args.manifest_format != "auto":
        try:
            text = Path(manifest).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print("error: {}".format(exc), file=sys.stderr)
            return EXIT_INTERNAL
        if args.manifest_format == "json" and not text.lstrip().startswith("{"):
            print("error: {} is not JSON".format(manifest), file=sys.stderr)
            return EXIT_INTERNAL
        if args.manifest_format == "text" and text.lstrip().startswith("{"):
            print("error: {} is JSON, not a text manifest".format(manifest),
                  file=sys.stderr)
            return EXIT_INTERNAL

    try:
        result = verify_manifest(
            package, manifest,
            unexpected_severity=args.unexpected_severity,
            check_archive_digests=not args.no_archive_digests,
        )
    except ManifestInputError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL
    except (OSError, ValueError) as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return EXIT_INTERNAL

    if not args.quiet:
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(render_text(result))
    return exit_code_for(result)


if __name__ == "__main__":
    sys.exit(main())
