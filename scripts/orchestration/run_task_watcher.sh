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

# Default to --dry-run unless explicitly specified by caller
DRY_RUN_ARG=""
if [[ "$*" != *"--live"* ]]; then
  DRY_RUN_ARG="--dry-run"
fi

echo "[$(date -u +'%Y-%m-%dT%H:%M:%SZ')] Invoking Shared Task Watcher (mode: ${DRY_RUN_ARG:-live})" >> "${LOG_FILE}"

# Execute watcher poll cycle
python3 "${SCRIPT_DIR}/shared_task_watcher.py" poll ${DRY_RUN_ARG} "$@" >> "${LOG_FILE}" 2>&1
EXIT_CODE=$?

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo "[$(date -u +'%Y-%m-%dT%H:%M:%SZ')] Watcher exited with status ${EXIT_CODE}" >> "${LOG_FILE}"
fi

exit ${EXIT_CODE}
