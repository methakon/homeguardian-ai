#!/usr/bin/env bash
# ==============================================================================
# HomeGuardian AI - certification PREFLIGHT gate
#
# Runs BEFORE any certification work. Implements the owner's resource policy:
#   1. diff the candidate against the last certified SHA and decide the MINIMUM
#      sufficient verification (docs-only changes must never trigger a rebuild);
#   2. refuse to start while another build/test is already running (no build
#      storms, no parallel trees compiling the same source);
#   3. derive a RAM-safe parallelism level instead of blindly using nproc;
#   4. reuse cached video/OCR evidence when the media digest is unchanged.
#
# Exit codes:
#   0  proceed with the printed plan
#   2  BLOCKED - another heavy job is running (wait, then retry)
#   3  usage / cannot determine plan
# ==============================================================================
set -uo pipefail

CANDIDATE="${1:-}"
LAST_CERTIFIED="${2:-}"
REPO="${HG_REPO:-$(git rev-parse --show-toplevel 2>/dev/null || echo .)}"
EVIDENCE_DIR="${HG_EVIDENCE_DIR:-$HOME/.hermes/cache/scratch/ci-evidence}"

die() { echo "BLOCKED: $*" >&2; exit 3; }
[ -n "$CANDIDATE" ]    || die "usage: $0 <candidate-sha> [last-certified-sha]"
# NB: a linked worktree has .git as a FILE, not a directory - ask git instead.
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || die "not a git repo: $REPO"

echo "=== HG CERTIFICATION PREFLIGHT ==="
echo "  candidate      : $CANDIDATE"
echo "  last certified : ${LAST_CERTIFIED:-<none given>}"

# ---------------------------------------------------------------- 1. heavy jobs
# Detect ANY concurrent heavy work before we add to it. This is the guard that
# the 48-minute run lacked: it happily launched four nohup build jobs at once.
RUNNING="$(ps -eo pid,etime,cmd 2>/dev/null \
  | grep -E 'cmake|ninja|ctest|[Cc]atch|homeguardian_tests|hg_mcp_server|package_release|verify_judge_release|verify_synthetic_mcp_demo|build_all\.sh|run_checks\.sh' \
  | grep -v grep || true)"
if [ -n "$RUNNING" ]; then
  echo "  --- already running ---"
  echo "$RUNNING" | sed 's/^/    /'
  echo "BLOCKED: a heavy job is already running. Serialize; do not start a second." >&2
  exit 2
fi
echo "  concurrent     : none (safe to proceed)"

# ------------------------------------------------------- 2. RAM-safe parallelism
AVAIL_MB="$(awk '/MemAvailable:/ {print int($2/1024)}' /proc/meminfo)"
# ~700 MB per concurrent C++ translation unit is the observed safe rule on the
# 15 GiB workstation. Kaggle CPU sessions are tighter on cores but roomier on RAM.
JOBS="${HG_JOBS:-$(( AVAIL_MB / 700 ))}"
[ "$JOBS" -gt 4 ] && JOBS=4     # cap: leaning on 40 cores is what OOM-killed builds
[ "$JOBS" -lt 1 ] && JOBS=1
echo "  mem available  : ${AVAIL_MB} MB  ->  build jobs: -j${JOBS} (cap 4)"
if [ "$AVAIL_MB" -lt 2000 ]; then
  echo "  WARNING: under 2 GB available; consider deferring the build entirely." >&2
fi

# --------------------------------------------------- 3. diff since last certified
PLAN="targeted"; REASON=""; CHANGED=""
if [ -n "$LAST_CERTIFIED" ] && git -C "$REPO" cat-file -e "${LAST_CERTIFIED}^{commit}" 2>/dev/null \
   && git -C "$REPO" cat-file -e "${CANDIDATE}^{commit}" 2>/dev/null; then
  CHANGED="$(git -C "$REPO" diff --name-only "$LAST_CERTIFIED" "$CANDIDATE" 2>/dev/null)"
  NCH="$(printf '%s\n' "$CHANGED" | grep -c . || true)"
  RUNTIME="$(printf '%s\n' "$CHANGED" | grep -E '^(src/|web_mcp/|tests/|CMakeLists\.txt|cmake/|config/|demo\.sh|deploy/|third_party/)' || true)"
  NRUN="$(printf '%s\n' "$RUNTIME" | grep -c . || true)"
  echo "  files changed  : $NCH"
  printf '%s\n' "$CHANGED" | sed 's/^/    /' | head -30
  if [ "$NRUN" -eq 0 ]; then
    PLAN="docs-only"
    REASON="no runtime/build path changed (src, web_mcp, tests, CMake, cmake, config, deploy, third_party)"
  else
    PLAN="targeted"
    REASON="$NRUN runtime/build file(s) changed - re-run only their affected suites"
  fi
else
  PLAN="full"
  REASON="no comparable baseline SHA available (cold start)"
fi

# --------------------------------------------------- 4. cached media/OCR evidence
if [ -d "$EVIDENCE_DIR" ]; then
  echo "  cached evidence: $(find "$EVIDENCE_DIR" -type f | wc -l) file(s) in $EVIDENCE_DIR"
  echo "                   reuse if the MP4 digest is unchanged (policy 6: no OCR rescan)"
fi

cat <<PLAN
-------------------------------------------------------------------------------
PLAN           : $PLAN
WHY            : $REASON
NEXT           : $(case "$PLAN" in
    docs-only) echo "run checkers only: check_release_package.py, check_release_manifest.py, check_submission_claims.py. DO NOT rebuild 527 C++ tests." ;;
    targeted)  echo "build with -j${JOBS} and run only the affected suites/targets." ;;
    full)      echo "full clean build with -j${JOBS} (justify in the report)." ;;
  esac)
-------------------------------------------------------------------------------
PLAN
exit 0
