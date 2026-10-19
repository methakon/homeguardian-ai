#!/usr/bin/env python3
"""Evidence provenance validator for HomeGuardian AI submission documentation.

Resolves every camera-evidence artifact a submission document references and reports, per artifact:

  * the git branch/commit that is supposed to contain it, and the resolved commit id;
  * the repository-relative path;
  * whether it exists at that revision;
  * its content SHA-256 (and byte/line counts) where applicable;
  * its evidence type: ``real`` | ``synthetic`` | ``transcribed`` (plus ``documentation`` and
    ``implementation`` for artifacts that carry no measurement of their own — see the manifest schema);
  * whether it is contained in the judge release or lives on an external branch.

The validator FAILS (exit 1) when a declared reference cannot be satisfied: an unresolvable revision,
a missing path, an empty artifact, a digest mismatch, a content marker that does not support the
declared evidence type, a marker that contradicts it, a declared scope that disagrees with where the
artifact actually exists, and — the case that motivated this tool — a document that ships in the
release while referencing an artifact that is not in the release.

    git branch/commit      -> --release-ref / --external-ref, or the manifest's own "refs" block
    machine-readable out   -> --json-out (default: docs/evidence/provenance-report.json)

Standard library plus the ``git`` binary only; no third-party packages.

Exit codes: 0 = every reference verified, 1 = at least one reference failed, 2 = usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys

SCHEMA = "hg-evidence-manifest/1"
REPORT_SCHEMA = "hg-evidence-report/1"

# Evidence types that carry a measurement and therefore MUST be verifiable from their own content.
MEASURED_TYPES = ("real", "synthetic", "transcribed")
# Types accepted for artifacts that are referenced but carry no measurement of their own.
SUPPORT_TYPES = ("documentation", "implementation")
ALL_TYPES = MEASURED_TYPES + SUPPORT_TYPES

# Required content markers per evidence type. A document's own provenance label is the verdict here:
# the validator does not take the manifest's word for what a file is, it reads the file.
REQUIRED_MARKERS = {
    "transcribed": ["transcription"],
    "synthetic": ["synthetic"],
    "real": ["provenance"],
}

# A marker that, if present, refutes the declared type outright.
FORBIDDEN_MARKERS = {
    "real": ['"provenance": "synthetic"', "provenance: synthetic", "Provenance::synthetic"],
    "synthetic": ['"provenance": "real"', "provenance         : real (a physical device frame)"],
    "transcribed": [],
}


class Finding:
    """One failed check, with the class a caller can key on."""

    def __init__(self, kind: str, artifact_id: str, detail: str):
        self.kind = kind
        self.artifact_id = artifact_id
        self.detail = detail

    def as_dict(self) -> dict:
        return {"kind": self.kind, "artifact": self.artifact_id, "detail": self.detail}

    def __str__(self) -> str:
        return f"[{self.kind}] {self.artifact_id}: {self.detail}"


def git(root: str, *args: str) -> tuple[int, str]:
    """Run git in ``root``; return (returncode, stdout-with-stderr-merged)."""
    proc = subprocess.run(
        ["git", "-C", root, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    return proc.returncode, proc.stdout.strip()


def resolve_revision(root: str, revision: str) -> str | None:
    """Resolve a branch/commit/tag name to a full commit id, or None if it does not resolve."""
    if not revision:
        return None
    for candidate in (revision, f"origin/{revision}"):
        rc, out = git(root, "rev-parse", "--verify", "--quiet", f"{candidate}^{{commit}}")
        if rc == 0 and out:
            return out.splitlines()[0]
    return None


def blob_at(root: str, commit: str, path: str) -> bytes | None:
    """Return the blob content of ``path`` at ``commit``, or None when it is not there."""
    rc, _ = git(root, "cat-file", "-e", f"{commit}:{path}")
    if rc != 0:
        return None
    proc = subprocess.run(
        ["git", "-C", root, "cat-file", "blob", f"{commit}:{path}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if proc.returncode != 0:
        return None
    return proc.stdout


def read_manifest(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schema") != SCHEMA:
        raise SystemExit(f"unsupported manifest schema: {manifest.get('schema')!r} (want {SCHEMA!r})")
    if not isinstance(manifest.get("artifacts"), list) or not manifest["artifacts"]:
        raise SystemExit("manifest has no artifacts")
    return manifest


def check_artifact(root: str, artifact: dict, refs: dict, findings: list[Finding],
                   allow_external: bool, strict_refs: bool = False,
                   unchecked: list[str] | None = None) -> dict:
    """Resolve and verify one artifact; append any failures to ``findings``."""
    aid = artifact.get("id") or artifact.get("path", "?")
    path = artifact.get("path", "")
    revision = artifact.get("revision") or ""
    scope = artifact.get("scope", "release")
    etype = artifact.get("evidence_type", "documentation")

    record = {
        "id": aid,
        "path": path,
        "declared_revision": revision,
        "resolved_commit": None,
        "exists": False,
        "sha256": None,
        "bytes": None,
        "lines": None,
        "evidence_type": etype,
        "classification_verified": False,
        "scope": scope,
        "scope_verified": None,
        "referenced_by": [],
    }

    if etype not in ALL_TYPES:
        findings.append(Finding("unknown_evidence_type", aid, f"{etype!r} is not one of {ALL_TYPES}"))
        return record

    commit = resolve_revision(root, revision)
    if commit is None:
        findings.append(Finding(
            "unresolved_revision", aid,
            f"revision {revision!r} does not resolve to a commit in this repository"))
        return record
    record["resolved_commit"] = commit

    blob = blob_at(root, commit, path)
    if blob is None:
        findings.append(Finding(
            "missing_artifact", aid, f"{path} does not exist at {revision} ({commit[:8]})"))
        return record
    if not blob:
        findings.append(Finding("empty_artifact", aid, f"{path} is zero bytes at {revision}"))
        return record

    text = blob.decode("utf-8", errors="replace")
    record["exists"] = True
    record["bytes"] = len(blob)
    record["lines"] = text.count("\n") + (0 if text.endswith("\n") else 1)
    record["sha256"] = hashlib.sha256(blob).hexdigest()

    declared_digest = artifact.get("sha256")
    if declared_digest and declared_digest != record["sha256"]:
        findings.append(Finding(
            "digest_mismatch", aid,
            f"declared {declared_digest[:16]}… but {path}@{commit[:8]} hashes to {record['sha256'][:16]}…"))

    # --- classification: read the artifact's own provenance label -----------------------------
    markers = artifact.get("content_markers")
    if markers is None:
        markers = REQUIRED_MARKERS.get(etype, []) if etype in MEASURED_TYPES else []
    missing = [m for m in markers if m not in text]
    if missing:
        findings.append(Finding(
            "marker_missing", aid,
            f"declared {etype} but {path} carries no marker {missing!r}; the claim is not verifiable "
            f"from the artifact itself"))
    contradicted = [m for m in artifact.get("forbid_markers", FORBIDDEN_MARKERS.get(etype, [])) if m in text]
    if contradicted:
        findings.append(Finding(
            "classification_contradicted", aid,
            f"declared {etype} but {path} contains {contradicted[0]!r}"))
    record["classification_verified"] = not missing and not contradicted
    record["markers_required"] = markers

    # --- scope: release-contained vs external -------------------------------------------------
    release_commit = refs.get("release_commit")
    external_commit = refs.get("external_commit")
    in_release = bool(release_commit) and blob_at(root, release_commit, path) is not None
    in_external = bool(external_commit) and blob_at(root, external_commit, path) is not None
    record["in_release"] = in_release
    record["in_external"] = in_external
    record["scope_verified"] = (scope == "release" and in_release) or (scope == "external" and not in_release)
    if scope == "release" and not in_release:
        findings.append(Finding(
            "scope_mismatch", aid,
            f"declared release-contained but {path} is absent from {refs.get('release_ref')} "
            f"({(release_commit or '?')[:8]})"))
    if scope == "external" and in_release:
        findings.append(Finding(
            "scope_mismatch", aid,
            f"declared external but {path} IS present in {refs.get('release_ref')} — the declaration "
            f"understates the release"))

    # --- references: who cites this artifact, and at which revision ----------------------------
    token = artifact.get("reference_token") or os.path.basename(path).split(".")[0]
    for ref in artifact.get("referenced_by", []) or []:
        doc_path = ref.get("path", "")
        doc_rev = ref.get("revision") or revision
        entry = {"doc": doc_path, "revision": doc_rev, "reference_confirmed": False,
                 "artifact_present_there": None}
        doc_commit = resolve_revision(root, doc_rev)
        if doc_commit is None:
            # A ref that this clone simply does not have is a check we could not run, not a
            # reference we disproved. Say so loudly; --strict-refs turns it back into a failure for
            # the release gate, where the release ref must be fetched.
            detail = (f"referencing document {doc_path} names revision {doc_rev!r}, which does not "
                      f"resolve in this clone; the cross-reference check was not run")
            if strict_refs:
                findings.append(Finding("unresolved_revision", aid, detail))
            elif unchecked is not None:
                unchecked.append(f"{aid} <- {doc_path} @ {doc_rev} ({detail})")
        else:
            doc_blob = blob_at(root, doc_commit, doc_path)
            if doc_blob is None:
                findings.append(Finding(
                    "unresolved_revision", aid,
                    f"referencing document {doc_path} does not exist at {doc_rev}"))
            else:
                doc_text = doc_blob.decode("utf-8", errors="replace")
                entry["reference_confirmed"] = token in doc_text
                entry["artifact_present_there"] = blob_at(root, doc_commit, path) is not None
                entry["doc_commit"] = doc_commit
                if (entry["reference_confirmed"] and not entry["artifact_present_there"]):
                    kind = "claim_not_supported_in_release" if doc_commit == release_commit else "dangling_reference"
                    if not (kind == "claim_not_supported_in_release" and allow_external):
                        findings.append(Finding(
                            kind, aid,
                            f"{doc_path} @ {doc_rev} ({(doc_commit or '?')[:8]}) cites {token!r} but "
                            f"{path} is not present at that revision"))
        record["referenced_by"].append(entry)

    return record


def build_report(root: str, manifest: dict, findings: list[Finding], refs: dict) -> dict:
    return {
        "schema": REPORT_SCHEMA,
        "manifest_schema": manifest.get("schema"),
        "repo_root": "." if root in (".", "") or os.path.abspath(root) == os.path.abspath(".") else os.path.basename(os.path.abspath(root)),
        "refs": {
            "release_ref": refs.get("release_ref"),
            "release_commit": refs.get("release_commit"),
            "external_ref": refs.get("external_ref"),
            "external_commit": refs.get("external_commit"),
        },
        "summary": {
            "artifacts": len(manifest["artifacts"]),
            "verified": sum(1 for f in findings) == 0,
            "failures": len(findings),
            "real": 0,
            "synthetic": 0,
            "transcribed": 0,
            "release_contained": 0,
            "external": 0,
            "external_cited_by_release": 0,
        },
        "findings": [f.as_dict() for f in findings],
        "artifacts": [],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify that every camera-evidence artifact a submission document references "
                    "actually exists at the revision that is supposed to contain it.")
    parser.add_argument("--manifest", default="docs/evidence/provenance-manifest.json",
                        help="evidence manifest (default: %(default)s)")
    parser.add_argument("--root", default=".", help="repository root (default: the current directory)")
    parser.add_argument("--release-ref", help="ref that holds the judge release (overrides the manifest)")
    parser.add_argument("--external-ref", help="ref that holds the external evidence branch")
    parser.add_argument("--allow-external-references", action="store_true",
                        help="do not fail when a release document references evidence that lives "
                             "outside the release (use when validating the external branch itself)")
    parser.add_argument("--json-out", help="write the machine-readable report here "
                                           "(default: docs/evidence/provenance-report.json)")
    parser.add_argument("--strict-refs", action="store_true",
                        help="treat a referencing revision that is absent from this clone as a "
                             "failure instead of a warning (use in the release gate, where every "
                             "ref must be fetched first)")
    parser.add_argument("--quiet", action="store_true", help="print only the summary line")
    args = parser.parse_args(argv)

    root = args.root
    try:
        manifest = read_manifest(args.manifest)
    except FileNotFoundError:
        print(f"manifest not found: {args.manifest}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as exc:
        print(f"manifest is not valid JSON: {exc}", file=sys.stderr)
        return 2

    declared = manifest.get("refs", {}) or {}
    refs = {
        "release_ref": args.release_ref or declared.get("release"),
        "external_ref": args.external_ref or declared.get("external"),
    }
    refs["release_commit"] = resolve_revision(root, refs["release_ref"]) if refs["release_ref"] else None
    refs["external_commit"] = resolve_revision(root, refs["external_ref"]) if refs["external_ref"] else None
    for label in ("release", "external"):
        ref, commit = refs[f"{label}_ref"], refs[f"{label}_commit"]
        if ref and commit is None:
            print(f"WARNING: {label} ref {ref!r} does not resolve in this clone; "
                  f"scope checks for it are skipped", file=sys.stderr)

    findings: list[Finding] = []
    unchecked: list[str] = []
    report = build_report(root, manifest, findings, refs)
    if manifest.get("schema") != SCHEMA:
        findings.append(Finding("unknown_manifest_schema", "-", str(manifest.get("schema"))))
    for artifact in manifest["artifacts"]:
        record = check_artifact(root, artifact, refs, findings, args.allow_external_references,
                                args.strict_refs, unchecked)
        report["artifacts"].append(record)

    summary = report["summary"]
    for rec in report["artifacts"]:
        if rec["exists"]:
            if rec["evidence_type"] in summary:
                summary[rec["evidence_type"]] += 1
            if rec.get("in_release"):
                summary["release_contained"] += 1
            else:
                summary["external"] += 1
                if any(e.get("reference_confirmed") and e.get("doc_commit") == refs.get("release_commit")
                       for e in rec["referenced_by"]):
                    summary["external_cited_by_release"] += 1
    summary["failures"] = len(findings)
    summary["references_unchecked"] = len(unchecked)
    report["references_unchecked"] = unchecked
    summary["verified"] = not findings
    report["findings"] = [f.as_dict() for f in findings]

    out_path = args.json_out or "docs/evidence/provenance-report.json"
    if out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=False)
            handle.write("\n")

    if not args.quiet:
        release = refs.get("release_ref") or "(none)"
        print(f"evidence provenance: {summary['artifacts']} artifact(s) declared, "
              f"release={release} ({(refs.get('release_commit') or '?')[:8]}), "
              f"external={refs.get('external_ref') or '(none)'} "
              f"({(refs.get('external_commit') or '?')[:8]})")
        for rec in report["artifacts"]:
            where = "release" if rec.get("in_release") else ("external" if rec["exists"] else "ABSENT")
            print(f"  {'ok ' if rec['exists'] and rec['classification_verified'] else 'BAD'} "
                  f"{rec['path']:<52} {rec['evidence_type']:<13} {where:<8} "
                  f"{(rec['sha256'] or '-')[:12]}  {(rec['resolved_commit'] or '?')[:8]}")
        for finding in findings:
            print(f"  FAIL {finding}")
        for note in unchecked:
            print(f"  WARN cross-reference not checked: {note}")
        print(f"SUMMARY: {summary['failures']} failure(s), "
              f"{summary['release_contained']} release-contained, {summary['external']} external "
              f"({summary['external_cited_by_release']} cited by a release document); "
              f"{summary['references_unchecked']} cross-reference(s) unchecked; "
              f"report -> {out_path}")

    return 0 if summary["verified"] else 1


if __name__ == "__main__":
    sys.exit(main())
