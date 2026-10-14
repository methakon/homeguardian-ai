#!/usr/bin/env bash
# verify_build_context_exclusions.sh - prove the Docker build context carries no
# credential material, WITHOUT touching the live tree.
#
# WHY THIS IS WRITTEN CAREFULLY
#   An earlier version of this probe planted dummy files at the real credential paths
#   inside the repository - including the mounted secret file - and overwrote a live
#   credential (FRICTION_LOG incident 42). The probe was right and the fixtures were
#   wrong. This version materialises the COMMITTED tree into a scratch directory with
#   `git archive`, plants its fixtures THERE, and builds with that directory as the
#   context. The working tree is never written to, so it cannot damage anything.
#
# Asserts, for every sensitive path: the file is ABSENT from the context. And for
# ordinary source files: PRESENT (a probe that excludes everything proves nothing).
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="${1:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-62s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-62s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-62s SKIP  %s\n' "$1" "${2:-}"; }
cleanup() { [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$W"; }
trap cleanup EXIT

echo "== D-03.1 build-context exclusion probe (isolated fixtures) =="
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
  skip "docker available" "no usable daemon"
  echo "  ____"; echo "  build-context probe: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

live_state() { { [[ -e "$REPO/deploy/docker/certs" ]] && printf 'certs ';
  [[ -e "$REPO/deploy/docker/credentials" ]] && printf 'credentials ';
  [[ -e "$REPO/deploy/docker/device-keys" ]] && printf 'device-keys ';
  [[ -e "$REPO/deploy/docker/offline-ca" ]] && printf 'offline-ca '; } 2>/dev/null; echo; }
# Credential paths present BEFORE the probe runs. The property under test is that the
# probe does not ADD any, not that the operator's tree is empty.
LIVE_BEFORE="$(live_state)"

CTX="$W/context"
mkdir -p "$CTX"
# A clean copy of the COMMITTED tree: no working-tree files, no credentials, and an
# ordinary `git archive` gives us the exact .dockerignore too.
git -C "$REPO" archive --format=tar HEAD | tar -x -C "$CTX" || { bad "materialise the committed tree"; exit 1; }

# Fixtures go into the COPY, never into the repository.
mkdir -p "$CTX/deploy/docker/certs/ca" "$CTX/deploy/docker/certs/provisioned" \
         "$CTX/deploy/docker/secrets" "$CTX/deploy/docker/credentials" \
         "$CTX/src/net/_ctxprobe/credentials" "$CTX/tools/private" "$CTX/tmp_creds"
for f in \
  deploy/docker/certs/server.key \
  deploy/docker/certs/server.crt \
  deploy/docker/certs/ca/ca.key \
  deploy/docker/certs/device.p12 \
  deploy/docker/certs/devices.json \
  deploy/docker/certs/revocations.json \
  deploy/docker/certs/provisioned/issued.p12 \
  deploy/docker/secrets/creds.json \
  deploy/docker/credentials/creds.secret \
  src/net/_ctxprobe/stray.key \
  src/net/_ctxprobe/credentials/deep.secret \
  tools/private/operator.key \
  tmp_creds/scratch.secret ; do
  echo "PROBE-FIXTURE" > "$CTX/$f"
done

# Control files that MUST survive, plus one marker file to prove the COPY worked.
printf 'control\n' > "$CTX/src/net/_ctxprobe/notes.md"

cat > "$W/probe.Dockerfile" <<'DOCKER'
FROM debian:trixie-slim
COPY . /src
RUN set -e; \
    for secret in \
      deploy/docker/certs/server.key deploy/docker/certs/ca/ca.key \
      deploy/docker/certs/device.p12 deploy/docker/certs/devices.json \
      deploy/docker/certs/revocations.json deploy/docker/certs/provisioned/issued.p12 \
      deploy/docker/secrets/creds.json deploy/docker/credentials/creds.secret \
      src/net/_ctxprobe/stray.key src/net/_ctxprobe/credentials/deep.secret \
      tools/private/operator.key tmp_creds/scratch.secret ; do \
      if [ -e "/src/$secret" ]; then echo "LEAKED $secret"; else echo "excluded $secret"; fi; \
    done; \
    for control in tools/provision_ca.sh src/net/TlsTransport.cpp deploy/docker/Dockerfile \
                   src/net/_ctxprobe/notes.md deploy/docker/config/ingress.json.example ; do \
      if [ -e "/src/$control" ]; then echo "present $control"; else echo "MISSING $control"; fi; \
    done
DOCKER

OUT="$(timeout 600 docker build --no-cache -f "$W/probe.Dockerfile" -t hgctx:probe031 "$CTX" 2>&1)"
# The build log echoes the RUN script itself, and that echo contains the marker words,
# so it is removed before counting. Only real probe output remains.
FINDINGS="$(grep -v 'for secret in' <<<"$OUT" | grep -v 'RUN set -e')"
LEAKED="$(grep -c 'LEAKED ' <<<"$FINDINGS" || true)"
EXCLUDED="$(grep -c 'excluded ' <<<"$FINDINGS" || true)"
MISSING="$(grep -c 'MISSING ' <<<"$FINDINGS" || true)"
PRESENT="$(grep -c 'present ' <<<"$FINDINGS" || true)"

[[ "$LEAKED" == "0" ]] && ok "no credential path reaches the build context (12 fixtures)" \
                       || bad "credential material leaked into the context" "$(grep 'LEAKED ' <<<"$FINDINGS" | head -3 | tr '\n' ' ')"
[[ "$EXCLUDED" == "12" ]] && ok "all 12 planted credential fixtures are excluded" \
                         || bad "unexpected exclusion count" "$EXCLUDED of 12"
[[ "$MISSING" == "0" ]] && ok "no ordinary source file was excluded by accident" \
                        || bad "ordinary files missing from the context" "$(grep 'MISSING ' <<<"$FINDINGS" | tr '\n' ' ')"
[[ "$PRESENT" == "5" ]] && ok "all 5 control files are present in the context" \
                        || bad "control files missing" "$PRESENT of 5"
docker rmi hgctx:probe031 >/dev/null 2>&1 || true

# The probe must not have touched the live tree: compare with the snapshot taken before
# it ran (see LIVE_BEFORE).
LIVE_AFTER="$(live_state)"
if [[ "$LIVE_BEFORE" == "$LIVE_AFTER" ]]; then
  ok "the probe left the live repository untouched (fixtures were copies)"
else
  bad "the probe changed the live repository" "before=[$LIVE_BEFORE] after=[$LIVE_AFTER]"
fi
if [[ -s "$REPO/deploy/docker/secrets/creds.json" ]] && \
   grep -q "demo-token" "$REPO/deploy/docker/secrets/creds.json"; then
  ok "the live mounted credential is intact"
else
  bad "the live mounted credential is missing or altered" "$REPO/deploy/docker/secrets/creds.json"
fi

echo "  --------------------------------------------------------------------"
echo "  build-context probe: $PASS passed, $FAIL failed, $SKIP skipped"
[[ "$FAIL" == "0" ]]
