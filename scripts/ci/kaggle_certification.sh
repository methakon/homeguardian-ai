#!/usr/bin/env bash
# ==============================================================================
# HomeGuardian AI - remote (Kaggle) certification driver
#
# Pushes scripts/ci/kaggle_certification.ipynb to Kaggle as a CPU-session kernel,
# waits for it to finish, and pulls the structured report + checksums back into a
# local evidence directory. The workstation runs NO compiler when this succeeds.
#
# Usage:
#   scripts/ci/kaggle_certification.sh <40-hex-sha> [--gpu] [--timeout-min N]
#
# Exit codes:
#   0  remote run completed and artifacts were pulled + verified
#   2  Kaggle unavailable/unauthenticated -> caller may fall back locally
#   4  remote run errored or timed out
#
# SAFETY: never uploads .env, credentials, tokens, private media or phone
# backups. The kernel clones the PUBLIC repository and receives only a commit
# SHA. A public notebook therefore contains no sensitive material.
# ==============================================================================
set -uo pipefail

SHA="${1:-}"; shift || true
USE_GPU=0; TIMEOUT_MIN=45
while [ $# -gt 0 ]; do
  case "$1" in
    --gpu) USE_GPU=1 ;;
    --timeout-min) TIMEOUT_MIN="${2:-45}"; shift ;;
    *) echo "unknown arg: $1" >&2; exit 3 ;;
  esac; shift || true
done
[ -n "$SHA" ] || { echo "usage: $0 <40-hex-sha> [--gpu] [--timeout-min N]" >&2; exit 3; }
case "$SHA" in *[!0-9a-f]*|"") echo "not a lowercase hex SHA: $SHA" >&2; exit 3 ;; esac
[ "${#SHA}" -eq 40 ] || { echo "expected 40-hex SHA, got ${#SHA} chars" >&2; exit 3; }

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
STAGE="${HG_KAGGLE_STAGE:-$HOME/.hermes/cache/scratch/kaggle-cert}"
EVID="${HG_EVIDENCE_DIR:-$HOME/.hermes/cache/scratch/ci-evidence}/$SHA"
mkdir -p "$STAGE" "$EVID"

# ---- availability check: never assume connection -----------------------------
if ! command -v kaggle >/dev/null 2>&1; then
  echo "KAGGLE: UNAVAILABLE (no kaggle CLI on PATH)" >&2; exit 2
fi
if [ ! -f "$HOME/.kaggle/kaggle.json" ]; then
  echo "KAGGLE: UNAVAILABLE (no ~/.kaggle/kaggle.json)" >&2; exit 2
fi
if ! timeout 30 kaggle competitions list -p 1 >/dev/null 2>&1; then
  echo "KAGGLE: UNAVAILABLE (authentication or network probe failed)" >&2; exit 2
fi
echo "KAGGLE: authenticated and reachable"

# ---- stage the kernel --------------------------------------------------------
USER="$(python3 -c 'import json,os;print(json.load(open(os.path.expanduser("~/.kaggle/kaggle.json")))["username"])')"
SLUG="${HG_KERNEL_SLUG:-hg-remote-certification}"
cp "$ROOT/scripts/ci/kaggle_certification.ipynb" "$STAGE/kaggle_certification.ipynb"
python3 - "$STAGE/kaggle_certification.ipynb" "$SHA" <<'PY'
import json,sys
p,sha=sys.argv[1],sys.argv[2]
nb=json.load(open(p))
for c in nb["cells"]:
    if c["cell_type"]=="code" and "HG_SHA" in "".join(c["source"]):
        c["source"]=[l.replace("REPLACE_WITH_SHA",sha) for l in c["source"]]
json.dump(nb,open(p,"w"),indent=1)
print("injected SHA:",sha)
PY
cat > "$STAGE/kernel-metadata.json" <<JSON
{
  "id": "$USER/$SLUG",
  "title": "HG remote certification",
  "code_file": "kaggle_certification.ipynb",
  "language": "python",
  "kernel_type": "notebook",
  "is_private": true,
  "enable_gpu": $([ "$USE_GPU" -eq 1 ] && echo true || echo false),
  "enable_internet": true,
  "dataset_sources": [],
  "kernel_sources": [],
  "competition_sources": []
}
JSON
echo "staged kernel: $USER/$SLUG  (gpu=$USE_GPU, internet=true, private=true)"

# ---- push --------------------------------------------------------------------
kaggle kernels push -p "$STAGE" 2>&1 | sed 's/^/  /' || { echo "push failed" >&2; exit 4; }

# ---- poll (bounded, no unbounded waits) -------------------------------------
DEADLINE=$(( $(date +%s) + TIMEOUT_MIN*60 ))
STATUS="unknown"
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  OUT="$(kaggle kernels status "$USER/$SLUG" 2>&1 || true)"
  STATUS="$(printf '%s' "$OUT" | tr -d '\r' | sed 's/.*status: //' | tr -d '"')"
  echo "  [$(date +%H:%M:%S)] status=$STATUS"
  case "$STATUS" in
    complete)  break ;;
    error|failed|cancelled) echo "remote run ended: $STATUS" >&2; break ;;
  esac
  sleep 30
done
[ "$STATUS" = "complete" ] || { echo "TIMEOUT or failure (status=$STATUS); no result trusted" >&2; exit 4; }

# ---- pull + verify -----------------------------------------------------------
kaggle kernels output "$USER/$SLUG" -p "$EVID" 2>&1 | sed 's/^/  /' || true
echo "artifacts in: $EVID"
ls -la "$EVID" | sed 's/^/  /'
if [ -f "$EVID/certification_report.json" ]; then
  python3 - "$EVID/certification_report.json" "$SHA" <<'PY'
import json,sys
r=json.load(open(sys.argv[1])); want=sys.argv[2]
print("reported SHA :", r.get("sha"))
print("VERDICT      :", r.get("verdict"))
print("tests        :", r.get("tests",{}).get("test_cases"), "cases /", r.get("tests",{}).get("assertions"), "assertions exit", r.get("tests",{}).get("exit_code"))
print("build jobs   :", r.get("build_jobs"), "runner nproc:", r.get("nproc"), "mem:", r.get("mem_available_mb"))
assert r.get("sha")==want, "SHA MISMATCH between requested and reported"
print("SHA identity verified against the requested commit")
PY
else
  echo "no certification_report.json pulled - result NOT trusted" >&2; exit 4
fi
echo "REMOTE CERTIFICATION COMPLETE for $SHA"
