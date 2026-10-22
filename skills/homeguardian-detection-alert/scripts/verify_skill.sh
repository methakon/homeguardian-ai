#!/usr/bin/env bash
# Lint the skill and re-check its claims against this checkout. With --with-fixture, also run the
# positive/negative fixture (slower: it starts containers).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
cd "$REPO"
echo "== skill lint (structure, anchors, behaviour) =="
python3 "$HERE/skill_lint.py"
rc=$?
if [[ "${1:-}" == "--with-fixture" ]]; then
  echo
  echo "== the fixture the skill's §9 claims =="
  bash scripts/verify_fusion_window.sh || rc=1
fi
exit $rc
