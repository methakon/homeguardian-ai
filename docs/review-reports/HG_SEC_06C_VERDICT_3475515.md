# HG-SEC-06C — verdict for 347551558fc3c4f5deb11b2c566b86ecc6e59db4

**Verdict: FAIL — NO-GO.** Do not promote this candidate.

Sanitized summary. Detailed findings, reproduction steps and source references are held privately by
the owner and are deliberately not published here, because the defect is unpatched and this branch is
public.

## What was tested

Independent verification of the exact commit (parent 2eb05bb, no substitution; materialised from the
commit itself with per-file blob hashes re-verified at the end). 206 checks across two reviewers:
authorization and lifecycle boundaries, credential handling, secret-file permissions, output leakage,
the previously passing security checks, and the MCP and dashboard regressions.

## Outcome

- The operator authentication trust boundary is **not** independently secured: the authority for both
  initial enrollment and recovery can still be supplied by the caller. Confirmed by reproduction.
- Enrollment without a credential is closed; the remaining path authorizes through the caller-controlled
  trust root, ending at a promoted, authorized camera.
- Consent, capture and online state are correctly invalidated on re-pair and recovery.
- A viewing token issued before revocation remains usable after recovery. Bounded by binding, single use
  and a short TTL.
- Replay watermark handling is internally inconsistent after a re-pair.
- The operator-credential stdin channel cannot authorize at all (fails closed).
- Credential permissions, output redaction, historical-credential rejection, rotation, revocation,
  restart-replay rejection and all MCP/dashboard regressions pass as previously verified.

## Required

A root-owned, caller-immutable anchor for the operator authority; that same anchor gating initial
enrollment; token invalidation on revocation and recovery; a consistent replay watermark policy; and a
working credential input channel. Detail and the smallest corrective scope are with the owner.
