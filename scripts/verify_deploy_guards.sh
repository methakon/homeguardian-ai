#!/usr/bin/env bash
# H-04 items 4a/4b: deployment guards for the two usability findings.
#
# WHAT IT PROVES
#   1. an UNPROVISIONED deployment fails the compose run itself, with a message naming HG_CERTS_DIR
#      and what to do - not by silently mounting an empty directory;
#   2. a provisioned HG_CERTS_DIR is used verbatim (there is no `./certs` fallback left to find);
#   3. the image bakes in NO token and NO credential material (no key/cert files);
#   4. the ingress entrypoint, given an empty mount, refuses with exit 78 and prints EVERY missing
#      file plus the exact provisioning commands;
#   5. it still refuses a mounted CA SIGNING KEY (the security property must survive the friendlier
#      message - a better diagnostic must not become a weaker gate);
#   6. the hub entrypoint's demo-token fallback announces itself, because the token is no longer baked
#      into the image and a silent default would be a credential handed to whoever runs the image.
#
# Runs on a HOST with docker/compose (the container test stage has no docker daemon). Missing docker
# or a missing image is a SKIP with a reason, never a pass.
#
# Usage: bash scripts/verify_deploy_guards.sh
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE="$REPO/deploy/docker/docker-compose.yml"
IMAGE="${HG_IMAGE:-homeguardian-hub:local}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-66s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-66s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-66s SKIP  %s\n' "$1" "${2:-}"; }

echo "== deployment guards (unprovisioned mount + image token) =="

# ---------------------------------------------------------------- 1/2. compose-level requirement
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    OUT="$(cd "$REPO" && env -u HG_CERTS_DIR timeout 60 docker compose -f "$COMPOSE" config 2>&1)"; RC=$?
    if [[ $RC -ne 0 ]]; then ok "compose REFUSES an unprovisioned run (exit $RC)"; else bad "compose REFUSES an unprovisioned run" "rc=0"; fi
    case "$OUT" in
        *HG_CERTS_DIR*provision*) ok "the refusal names HG_CERTS_DIR and the provisioning step" ;;
        *) bad "the refusal names HG_CERTS_DIR and the provisioning step" "$(printf '%s' "$OUT" | head -2 | tr '\n' ' ')" ;;
    esac

    TMPDIR_CERTS="$(mktemp -d)"
    OUT="$(cd "$REPO" && HG_CERTS_DIR="$TMPDIR_CERTS" timeout 60 docker compose -f "$COMPOSE" config 2>/dev/null)"
    # `compose config` canonicalises short volume syntax into long form
    # (type/source/target/read_only), so accept EITHER rendering rather than assuming the short one.
    if { printf '%s' "$OUT" | grep -q "source: $TMPDIR_CERTS" && printf '%s' "$OUT" | grep -q "target: /etc/homeguardian/certs"; } \
       || printf '%s' "$OUT" | grep -q "$TMPDIR_CERTS:/etc/homeguardian/certs"; then
        ok "a provisioned HG_CERTS_DIR is mounted verbatim (no fallback path)"
    else
        bad "a provisioned HG_CERTS_DIR is mounted verbatim" "$(printf '%s' "$OUT" | grep -E 'source:|target:|certs' | head -3 | tr '\n' ' ')"
    fi
    rm -rf "$TMPDIR_CERTS"
else
    skip "compose refuses an unprovisioned run" "docker compose unavailable here"
    skip "a provisioned HG_CERTS_DIR is mounted verbatim" "docker compose unavailable here"
fi

# ---- no fallback may exist in the file at all (pure text check, no docker needed)
if [[ "$(grep -c ':-\./certs' "$COMPOSE")" == "0" ]]; then
    ok "no ':-./certs' default remains in the compose file"
else
    bad "no ':-./certs' default remains in the compose file"
fi

# ---------------------------------------------------------------- 3/4/5/6. image-level checks
if ! command -v docker >/dev/null 2>&1; then
    skip "image-level guards" "docker unavailable here"
elif ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    skip "image-level guards" "image $IMAGE not built here (build the runtime target first)"
else
    ENV_JSON="$(docker image inspect "$IMAGE" --format '{{json .Config.Env}}' 2>/dev/null)"
    if printf '%s' "$ENV_JSON" | grep -q 'HG_TOKEN'; then
        bad "the image bakes NO token" "HG_TOKEN present in the image environment"
    else
        ok "the image bakes NO token (HG_TOKEN absent from its environment)"
    fi

    LEAK="$(docker run --rm --entrypoint /bin/sh "$IMAGE" -c \
            'find /etc/homeguardian -type f \( -name "*.key" -o -name "*.crt" -o -name "*.pem" -o -name "*.p12" \) 2>/dev/null' 2>/dev/null)"
    if [[ -z "$LEAK" ]]; then ok "the image bakes NO credential material"; else bad "the image bakes NO credential material" "$LEAK"; fi

    EMPTY="$(mktemp -d)"
    OUT="$(docker run --rm -v "$EMPTY:/etc/homeguardian/certs:ro" --entrypoint /usr/local/bin/hg-ingress-entrypoint "$IMAGE" 2>&1)"; RC=$?
    if [[ $RC -eq 78 ]]; then ok "an empty mount refuses with exit 78 (EX_CONFIG)"; else bad "an empty mount refuses with exit 78" "rc=$RC"; fi
    case "$OUT" in
        *"NOT provisioned"*) ok "the refusal says the mount is not provisioned" ;;
        *) bad "the refusal says the mount is not provisioned" "$(printf '%s' "$OUT" | head -1)" ;;
    esac
    case "$OUT" in
        *ingress.json*server.crt*server.key*ca/ca.crt*devices.json*)
            ok "the refusal lists EVERY missing file (not just the first)" ;;
        *) bad "the refusal lists every missing file" "$(printf '%s' "$OUT" | sed -n '4p')" ;;
    esac
    case "$OUT" in
        *provision_ca.sh*init*"chown 10001:10001"*) ok "the refusal prints the provisioning + permission commands" ;;
        *) bad "the refusal prints the provisioning + permission commands" ;;
    esac
    rm -rf "$EMPTY"

    # The security property must still hold: a mounted CA signing key is refused outright.
    withkey="$(mktemp -d)"; mkdir -p "$withkey/ca"; : > "$withkey/ca/ca.key"
    OUT="$(docker run --rm -v "$withkey:/etc/homeguardian/certs:ro" --entrypoint /usr/local/bin/hg-ingress-entrypoint "$IMAGE" 2>&1)"; RC=$?
    if [[ $RC -eq 78 ]] && printf '%s' "$OUT" | grep -q "REFUSING to start"; then
        ok "a mounted CA SIGNING KEY is still refused outright (gate not weakened)"
    else
        bad "a mounted CA signing key is still refused outright" "rc=$RC: $(printf '%s' "$OUT" | head -1)"
    fi
    rm -rf "$withkey"

    OUT="$(timeout 20 docker run --rm -e HG_TOKEN= --entrypoint /bin/bash "$IMAGE" -c '/usr/local/bin/hg-entrypoint 2>&1 | head -3' 2>&1)"
    if printf '%s' "$OUT" | grep -qi "synthetic demo token"; then
        ok "the hub announces its demo-token fallback instead of using it silently"
    else
        bad "the hub announces its demo-token fallback" "$(printf '%s' "$OUT" | head -1)"
    fi
fi

echo "  --------------------------------------------------------------------"
echo "  deployment guards: $PASS passed, $FAIL failed, $SKIP skipped"
[[ $FAIL -eq 0 ]] || exit 1
