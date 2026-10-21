#!/usr/bin/env bash
# verify_suite_concurrency.sh - the test suite must be safe to run twice at once
# in the same working directory.
#
# Defect this guards (fixed in v21): nine fixtures used FIXED, CWD-relative
# database filenames ("test_persistence.db", "test_consent_gate.db", ...) and
# deleted them in their constructors. A second suite instance running in the same
# directory therefore unlinked the first instance's open database, and SQLite
# reported the collision as "SQL execute failed: disk I/O error" on the next
# PRAGMA journal_mode = WAL / CREATE TABLE -- nine phantom failures that look
# like a product defect but are pure test-fixture interference.
#
# The fixtures now suffix their paths with the process id, so concurrent
# instances use different files. This script reproduces the original setup:
# two instances started together, both must pass with no "disk I/O error".
#
# Usage: HG_TESTS_PATH=/path/to/homeguardian_tests bash scripts/verify_suite_concurrency.sh
set -uo pipefail

TESTS="${HG_TESTS_PATH:?set HG_TESTS_PATH to a built homeguardian_tests}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/hg_concurrency.XXXXXX)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "== two suite instances in one working directory =="
cd "$WORK" || exit 1
( cd "$WORK" && HG_SOURCE_DIR="$REPO" "$TESTS" > "$WORK/a.log" 2>&1 ) &
PA=$!
( cd "$WORK" && HG_SOURCE_DIR="$REPO" "$TESTS" > "$WORK/b.log" 2>&1 ) &
PB=$!
wait "$PA"; RA=$?
wait "$PB"; RB=$?

FAIL=0
for inst in a b; do
  line=$(grep -oE 'All tests passed \([0-9]+ assertions in [0-9]+ test cases\)' "$WORK/$inst.log" | head -1)
  if [[ -n "$line" ]]; then
    printf '  %-14s %s\n' "instance $inst" "$line"
  else
    printf '  %-14s FAILED: %s\n' "instance $inst" \
      "$(grep -oE 'test cases:.*' "$WORK/$inst.log" | head -1)"
    FAIL=1
  fi
done
[[ "$RA" == "0" && "$RB" == "0" ]] || { echo "  non-zero exit: a=$RA b=$RB"; FAIL=1; }

dio=$(grep -ci 'disk I/O error' "$WORK/a.log" "$WORK/b.log" | awk -F: '{s+=$2} END{print s+0}')
if [[ "$dio" == "0" ]]; then
  echo "  no 'disk I/O error' in either instance            OK"
else
  echo "  'disk I/O error' occurrences: $dio                FAIL"
  FAIL=1
fi

# fixture databases must clean up after themselves; homeguardian.log is the
# logger's own file (expected, written by both instances) and is reported apart.
leftovers=$(find "$WORK" -maxdepth 1 \( -name '*.db' -o -name '*.db-wal' -o -name '*.db-shm' -o -name '*.db-journal' \) | wc -l)
if [[ "$leftovers" == "0" ]]; then
  echo "  leftover fixture databases in the working directory: 0   OK"
else
  echo "  leftover fixture databases: $leftovers                  FAIL"
  find "$WORK" -maxdepth 1 -name '*.db*'
  FAIL=1
fi
[[ -f "$WORK/homeguardian.log" ]] && echo "  (homeguardian.log present, as expected: the logger's own file)"

echo "  ----------------------------------------------"
if [[ "$FAIL" == "0" ]]; then echo "  suite concurrency: PASS"; else echo "  suite concurrency: FAIL"; fi
exit "$FAIL"
