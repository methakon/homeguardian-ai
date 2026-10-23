# Remote-first certification (scripts/ci)

These imported scripts describe a remote-first certification workflow. They
are not a completed or certified remote runner: the referenced notebook is
absent from this revision. No remote run was performed during integration.

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
| `kaggle_certification.ipynb` | Kaggle CPU session | required but absent; remote execution remains blocked |

## Usage

```bash
# 1. always preflight first - it may tell you a rebuild is unnecessary
bash scripts/ci/certification_preflight.sh <candidate-sha> <last-certified-sha>

# 2. only after restoring and reviewing the missing notebook and runner gates
bash scripts/ci/kaggle_certification.sh <candidate-sha> [--gpu] [--timeout-min 45]
```

Artifacts land in `~/.hermes/cache/scratch/ci-evidence/<sha>/`:
`certification_report.json`, `checksums.json`, `CMakeCache.txt`.

## Hard rules (owner directive 2026-10-20)

1. **Diff before you build.** Documentation-only heads run checkers only. Never
   rebuild the full native suite because a docs SHA changed.
2. **No parallel build storms.** `build_all.sh`, `verify_judge_release.sh`,
   `verify_synthetic_mcp_demo.sh`, `run_checks.sh` never run simultaneously; if a
   script internally launches a build, detect and serialize it.
3. **No uncontrolled `nohup` build jobs.**
4. **One build tree at a time.** Never compile the same source in two trees.
5. **No repeated OCR.** If the MP4 digest is unchanged, reuse cached frame/OCR
   evidence.
6. **Local fallback is capped:** `-j2`, one heavy process, memory-aware, and stop
   if the machine becomes constrained. **Never run heavy local work while
   device deployment diagnostics are running.**
7. **GPU only with a real GPU implementation.** Kaggle GPU quota is not spent
   because capacity exists.

## Known limitations (verify, do not assume)

- Remote output validation checks the requested SHA, but does not independently
  require a passing verdict/test exit code or verify artifact checksums. Shell
  syntax success is not certification; do not rely on this driver until those
  gates and its missing notebook are completed and tested.
- The preflight path classifier is advisory: it omits paths such as `apk/`,
  `alexa_skill_python/`, `lambda/` and `scripts/`. Review the diff manually;
  a printed `docs-only` plan must not bypass affected tests.
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
