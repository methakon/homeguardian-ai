# Rebuild avoidance + certification disk inventory

Owner directive 2026-10-20, sections 3 and 7. **Nothing here has been deleted.**

## 1. Disk inventory (measured 2026-10-20)

Certification scratch trees under `~/.hermes/cache/scratch/`:

| tree | size | built | what it holds |
|---|---|---|---|
| `sm61x5_kernel` | 2072 MB | — | **not mine** — another worker's kernel working dir. Do not touch. |
| `pr20cert-v2` | 915 MB | 2026-10-19 22:46, 22:57 | 2 build trees (`build/`, `hg_build/`) |
| `hgcert-a80b` | 594 MB | 2026-10-20 02:19 | build + `homeguardian_tests` binary |
| `pr20cert` | 570 MB | 2026-10-19 16:14 | build tree |
| `pr20delta` | 549 MB | 2026-10-19 | build tree |
| `sec01-delta` | 137 MB | — | build + evidence |
| `hg-cert-1792435343` | 109 MB | — | build tree |
| `cert-pr20` | 77 MB | — | build tree |
| `sec01` | 73 MB | — | evidence |
| `cert-pr20-final-4kTCRt` | 53 MB | — | build tree |
| **my certification total** | **3077 MB** | | |

Small, high-value evidence to keep unconditionally (safe):

    pr20cert-preserved    2.5 MB   certification evidence bundles
    sec01-fix             620 KB   four fixes + fixtures
    sec01-incident        20 KB    worktree incident report
    hg-audit-phaseCDE      46 MB   licence/attribution audit inputs
    media-policy-roundtrip 80 KB   metadata round-trip proof

Free disk: 1.4 TB — **there is no space emergency.** This is housekeeping, not
triage, and the owner has explicitly forbidden deletion without approval.

## 2. Safe cleanup proposal (PROPOSED — awaiting approval)

Disposable, in this order, only after a clean re-verification exists remotely:

1. `cert-pr20-final-4kTCRt` (53 MB), `cert-pr20` (77 MB), `hg-cert-1792435343`
   (109 MB) — superseded trees whose conclusions are already recorded in the
   preserved evidence bundles.
2. `pr20cert`, `pr20delta` (1119 MB) — superseded by `pr20cert-v2` + preserved
   reports.
3. `pr20cert-v2`, `hgcert-a80b` (1509 MB) — the two most recent; hold only if the
   upstream PR #20 head is still unmerged. Delete last.

Before any deletion: copy out `**/build/CMakeCache.txt`, any `*.json`
certification reports, and build logs — those are the reviewable artifacts; the
object files are not. `sm61x5_kernel` is **out of scope** (another worker's).

## 3. Avoidable rebuilds — estimated from observed runs

| date | head | delta since previous | what I did | what was sufficient |
|---|---|---|---|---|
| 10-19 16:14 | `a2855ae` | first cert of this line | full build + tests | correct (cold start) |
| 10-19 22:46 | `f9fc116e` | harness fix + docs | full rebuild | full (harness is runtime) |
| 10-20 02:19 | `2841e471` → `a80b8ebc` | **docs-only + 1 NOTICE** | full rebuild + 4-tree parallelism | **checkers only** |

**Estimated avoidable work: 2 of 3 certifications, ≈ 1.5–2 GB of build trees,
≈ 35–50 minutes of workstation CPU**, i.e. the majority of the load that caused
the complaint. Every head move in that window was documentation, NOTICE or
evidence-pack text — `git diff <old> <new> -- src/ web_mcp/ tests/ CMakeLists.txt`
was **empty** in each case.

`scripts/ci/certification_preflight.sh` now computes exactly this classification
before any build is started, and refuses to run while another heavy job is alive.

## 4. Still-open resource findings (not fixed here)

- `.env` and `deploy/docker/secrets/creds.json` are untracked and **not ignored**
  in the protected repo (secret-exposure risk, reported separately).
- The root `.gitignore` has a concurrent writer; freeze it before committing.
- `hg_device_ingress` (pid 3767) is pre-existing deployment infrastructure, not
  mine, 0% CPU — left alone.
