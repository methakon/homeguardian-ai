#!/usr/bin/env bash
# HomeGuardian AI - Shared Task Watcher Runner Script
# Mission: HG-WORKER-ORCHESTRATION-BOOTSTRAP-01
# Safe runner for cron or standalone daemon invocation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
STATE_DIR="${REPO_ROOT}/.hg_orchestrator"

mkdir -p "${STATE_DIR}/logs"
LOG_FILE="${STATE_DIR}/logs/watcher.log"

ARGS=()
if [[ "$*" == *"--live"* ]]; then
  ARGS+=("--live")
  MODE_STR="live"
else
  ARGS+=("--dry-run")
  MODE_STR="dry-run"
fi

for arg in "$@"; do
  if [[ "$arg" != "--live" && "$arg" != "--dry-run" ]]; then
    ARGS+=("$arg")
  fi
done

echo "[$(date -u +'%Y-%m-%dT%H:%M:%SZ')] Invoking Shared Task Watcher (mode: ${MODE_STR})" >> "${LOG_FILE}"

# Execute watcher poll cycle
python3 "${SCRIPT_DIR}/shared_task_watcher.py" poll "${ARGS[@]}" >> "${LOG_FILE}" 2>&1
EXIT_CODE=$?

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo "[$(date -u +'%Y-%m-%dT%H:%M:%SZ')] Watcher exited with status ${EXIT_CODE}" >> "${LOG_FILE}"
fi

exit ${EXIT_CODE}
