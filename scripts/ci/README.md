# Remote-first certification (scripts/ci)

Heavy HomeGuardian certification work — full C++20 builds, the Catch2 suite,
OCR over demo video — runs on **Kaggle CPU sessions**, not on the shared
workstation. This directory is the workflow that does it.

## Why

Observed on 2026-10-19: four full CMake build trees were produced in ~10 hours
(570 MB + 915 MB + 594 MB + 549 MB) because the release head moved seven times
and each certification rebuilt from scratch. One 48-minute run fanned out four
`nohup` build jobs at once. The workstation has **40 cores but only 15.8 GB
RAM** — wide parallelism is memory-doomed (a build was already OOM-killed at 28%
with a second build running).

## Files

| file | runs where | purpose |
|---|---|---|
| `certification_preflight.sh` | workstation | decides the *minimum sufficient* verification; blocks build storms; computes RAM-safe `-j` |
| `kaggle_certification.sh` | workstation | stages, pushes, polls and pulls the remote run; verifies SHA identity |
| `kaggle_certification.ipynb` | Kaggle CPU session | exact-SHA checkout, pinned toolchain, build, tests, checkers, structured report |

## Usage

```bash
# 1. always preflight first - it may tell you a rebuild is unnecessary
scripts/ci/certification_preflight.sh <candidate-sha> <last-certified-sha>

# 2. remote certification (no local compiler)
scripts/ci/kaggle_certification.sh <candidate-sha> [--gpu] [--timeout-min 45]
```

Artifacts land in `~/.hermes/cache/scratch/ci-evidence/<sha>/`:
`certification_report.json`, `checksums.json`, `CMakeCache.txt`.

## Hard rules (owner directive 2026-10-20)

1. **Diff before you build.** Documentation-only heads run checkers only. Never
   rebuild 527 C++ tests because a docs SHA changed.
2. **No parallel build storms.** `build_all.sh`, `verify_judge_release.sh`,
   `verify_synthetic_mcp_demo.sh`, `run_checks.sh` never run simultaneously; if a
   script internally launches a build, detect and serialize it.
3. **No uncontrolled `nohup` build jobs.**
4. **One build tree at a time.** Never compile the same source in two trees.
5. **No repeated OCR.** If the MP4 digest is unchanged, reuse cached frame/OCR
   evidence.
6. **Local fallback is capped:** `-j2`, one heavy process, memory-aware, and stop
   if the machine becomes constrained. **Never run heavy local work while
   Hermes 2 is doing USB/fastboot/Linux deployment diagnostics.**
7. **GPU only with a real GPU implementation.** Kaggle GPU quota is not spent
   because capacity exists.

## Known limitations (verify, do not assume)

- **cmake >= 3.28.3** is required by the project. Kaggle's base image may ship
  older; the notebook installs a pinned cmake via pip when needed.
- **Network required:** `spdlog` and `nlohmann/json` come from `FetchContent`
  (GitHub), so `enable_internet: true` is mandatory unless those sources are
  vendored into a prepared dataset. Catch2 (2.13.10) and SQLite (3.53.4
  amalgamation) are already vendored.
- Kaggle session limits, weekly GPU/CPU hours and idle timeouts apply and must be
  re-checked per run rather than assumed.
- The notebook is pushed **private**; it clones the public repository and
  receives only a commit SHA. No `.env`, credentials, tokens, camera recordings
  or phone backups are ever uploaded.

## Sensitivity boundary

The repository is **public**, so a remote clone is safe. Anything not already
public — private camera media, `devices/backup/`, `.env`,
`deploy/docker/secrets/` — must never be staged into the Kaggle directory. The
staging code copies exactly one notebook and one metadata file; keep it that way.
