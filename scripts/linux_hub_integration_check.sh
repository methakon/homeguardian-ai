#!/usr/bin/env bash
# The single integration check a Linux hub runs. Builds if needed, runs the four contract tags in order,
# and prints PASS/FAIL per tag plus a final verdict. Exit code 0 only when every tag passed.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [[ ! -x build/homeguardian_tests ]]; then
  echo "building homeguardian_tests ..."
  cmake -S . -B build -DHOMEGUARDIAN_BUILD_TESTS=ON >/dev/null || { echo "FAIL: cmake configure"; exit 2; }
  cmake --build build -j"$(nproc)" --target homeguardian_tests >/dev/null || { echo "FAIL: build"; exit 2; }
fi

fail=0
for tag in '[net]' '[producer]' '[reliability]' '[e2e]'; do
  out="$(HG_SOURCE_DIR="$root" ./build/homeguardian_tests "$tag" 2>&1)"
  if grep -q 'All tests passed' <<<"$out"; then
    printf 'PASS %-14s %s\n' "$tag" "$(grep -oE '[0-9]+ assertions in [0-9]+ test cases' <<<"$out" | head -1)"
  else
    printf 'FAIL %-14s\n' "$tag"; grep -E 'FAILED|REQUIRE|error' <<<"$out" | head -5; fail=1
  fi
done

# The two lines that prove the chain, not just the tests: a genuine alert and the genuine MCP answer.
line="$(HG_SOURCE_DIR="$root" ./build/homeguardian_tests '[e2e][corroboration]' 2>&1 | grep -m1 'MCP get_alert_summary' || true)"
if [[ -n "$line" ]]; then printf 'EVIDENCE %s\n' "$line"; else echo "WARN no corroboration evidence line printed"; fi

if [[ $fail -eq 0 ]]; then echo "VERDICT: PASS - hub integration intact"; else echo "VERDICT: FAIL"; fi
exit $fail
