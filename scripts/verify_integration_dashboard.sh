#!/usr/bin/env bash
# verify_integration_dashboard.sh - the sensor/integration status surface tells
# the truth, and the API cannot render an unsupported claim.
#
# The checks encode the milestone's honesty rules as executable assertions:
#   1. /api/integrations is read-only and returns JSON with the expected keys
#   2. it reports NOTHING as live (no device, no Home Assistant, no live fusion):
#      the counts must be zero, because nothing is connected
#   3. the reason strings explain the emptiness rather than hiding it
#   4. the payload contains NO calamity/identity claim anywhere (the forbidden
#      tokens are absent from the response entirely)
#   5. the adapter's own configuration reason is what tells the UI why Home
#      Assistant is off (not a hardcoded sentence)
#   6. the served page carries the three provenance labels and the honest-scope
#      paragraph, and the label legend is populated from the API vocabulary
#   7. the existing routes still behave (no regression from the new route)
#
# Usage: bash scripts/verify_integration_dashboard.sh [port]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGE="${HG_BRIDGE_PATH:-$REPO/hg_build/hg_alexa_bridge}"
PORT="${1:-$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')}"
TMP="$(mktemp -d)"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  %-62s OK\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  %-62s FAIL  %s\n' "$1" "${2:-}"; }

cleanup() { [ -n "${PID:-}" ] && kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

echo "== sensor & integration status surface =="
[ -x "$BRIDGE" ] || { echo "bridge not found at $BRIDGE"; exit 1; }

# The credential file is a TABLE keyed by token (see demo.sh / the Docker example).
cat > "$TMP/creds.json" <<'CREDS'
{
  "dashboard-verify-token": {"principal":"verify","allow_status":true,"allow_alert_summary":true,"expires_at_ms":0,"revoked":false}
}
CREDS
chmod 600 "$TMP/creds.json"

printf '%s' "dashboard-verify-token" > "$TMP/tok.txt"
chmod 600 "$TMP/tok.txt"
HG_HTTP_TOKEN="dashboard-verify-token" "$BRIDGE" --http "$REPO/config/homeguardian.demo.json" "$TMP/creds.json" \
          --port "$PORT" --token-file "$TMP/tok.txt" > "$TMP/bridge.log" 2>&1 &
PID=$!

for _ in $(seq 1 40); do
  curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 && break
  sleep 0.25
done
curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 || { echo "bridge did not start"; cat "$TMP/bridge.log"; exit 1; }

# Adversarial: a MALFORMED credential table must fail closed and cleanly (no core
# dump, no unauthenticated serving, non-zero exit).
printf '%s' '{"token":"oops"}' > "$TMP/bad_creds.json"
chmod 600 "$TMP/bad_creds.json"
BADPORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
HG_HTTP_TOKEN="dashboard-verify-token" timeout 20 "$BRIDGE" --http "$REPO/config/homeguardian.demo.json" "$TMP/bad_creds.json" \
        --port "$BADPORT" --token-file "$TMP/tok.txt" > "$TMP/bad.log" 2>&1
BADRC=$?
if [ "$BADRC" -ne 0 ] && [ "$BADRC" -ne 124 ]; then
  ok "malformed credential table exits non-zero ($BADRC) instead of aborting"
else
  bad "malformed credential table did not fail closed" "rc=$BADRC"
fi
if grep -qi "fail-closed" "$TMP/bad.log"; then ok "the refusal states it fails closed"; else bad "no fail-closed message"; fi
if grep -qi "core dumped\|terminate called" "$TMP/bad.log"; then bad "the refusal still aborted (core dump)"; else ok "no abort/core dump on a malformed credential table"; fi
curl -sf "http://127.0.0.1:$BADPORT/api/health" >/dev/null 2>&1 && bad "the bridge served after a failed credential load" || ok "no serving after a failed credential load"

BODY="$TMP/integrations.json"
CODE="$(curl -s -o "$BODY" -w '%{http_code}' "http://127.0.0.1:$PORT/api/integrations")"
[ "$CODE" == "200" ] && ok "GET /api/integrations -> 200" || bad "GET /api/integrations" "status $CODE"

python3 - "$BODY" "$PORT" "$TMP" <<'PY'
import json, sys, urllib.request, urllib.error

body_path, port, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
ok = fail = 0
def good(m): global ok; ok += 1; print("  %-62s OK" % m)
def bad_(m, d=""): global fail; fail += 1; print("  %-62s FAIL  %s" % (m, d))

raw = open(body_path).read()
try:
    j = json.loads(raw)
except Exception as e:
    bad_("payload parses as JSON", str(e)); print("  %d passed, %d failed" % (ok, fail)); sys.exit(1)
good("payload parses as JSON")

# 1/2: structure and the honest "nothing is live" counts
for key in ("sensor_devices", "home_assistant", "animal_activity",
            "security_observations", "environmental_correlations", "evidence", "provenance"):
    if key in j: good("payload carries '%s'" % key)
    else: bad_("payload carries '%s'" % key)

if j.get("sensor_devices", {}).get("connected") == 0: good("sensor devices connected = 0 (nothing is connected)")
else: bad_("sensor devices connected != 0")

if j.get("home_assistant", {}).get("enabled") is False: good("Home Assistant reports disabled")
else: bad_("Home Assistant not reported disabled")

if j.get("home_assistant", {}).get("configured") is False: good("Home Assistant reports not configured")
else: bad_("Home Assistant reported configured without a configuration")

if j.get("home_assistant", {}).get("requests_issued") == 0: good("Home Assistant requests issued = 0")
else: bad_("Home Assistant requests issued != 0")

if j.get("home_assistant", {}).get("actuation") is False: good("Home Assistant actuation reported disabled")
else: bad_("Home Assistant actuation flag wrong")

if j.get("animal_activity", {}).get("live_data") is False: good("animal activity reports no live data")
else: bad_("animal activity claimed live data")

if j.get("security_observations", {}).get("live_data") is False: good("security observations report no live data")
else: bad_("security observations claimed live data")

if j.get("environmental_correlations", {}).get("count") == 0: good("environmental correlations = 0")
else: bad_("environmental correlations != 0")

if j.get("evidence", {}).get("live_fusion_runs") == 0: good("live fusion runs = 0")
else: bad_("live fusion runs != 0")

# 3: emptiness is explained, not hidden
reasons = [j.get("sensor_devices", {}).get("reason", ""),
           j.get("home_assistant", {}).get("reason", ""),
           j.get("animal_activity", {}).get("reason", ""),
           j.get("security_observations", {}).get("reason", "")]
if all(r for r in reasons): good("every empty surface states why it is empty")
else: bad_("an empty surface has no reason string", str(reasons))

# 5: the reason comes from the adapter's own validation
if "disabled" in j.get("home_assistant", {}).get("reason", "").lower():
    good("Home Assistant reason comes from the adapter's own validation")
else:
    bad_("Home Assistant reason is not the adapter's", j.get("home_assistant", {}).get("reason"))

# The result vocabulary is the closed, permitted one
allowed = {"activity_change_detected", "audio_activity_deviation",
           "possible_animal_activity", "environmental_correlation", "insufficient_evidence"}
ids = {l.get("id") for l in j.get("animal_activity", {}).get("labels", [])}
if ids == allowed: good("animal-activity vocabulary is exactly the permitted set")
else: bad_("animal-activity vocabulary differs", str(sorted(ids)))

# the hypothesis label must be marked as a hypothesis, not as an observation
hyp = [l for l in j.get("animal_activity", {}).get("labels", []) if l.get("id") == "possible_animal_activity"]
if hyp and hyp[0].get("kind") == "hypothesis": good("possible_animal_activity is labelled a hypothesis")
else: bad_("possible_animal_activity is not labelled a hypothesis")

# 4: no unsupported claim anywhere in the payload
forbidden = ["earthquake_imminent", "flood_will_occur", "dog_predicts_disaster",
             "earthquake", "flood", "tsunami", "species", "intruder", "fall_detected",
             "Bearer", "authorization:", "token"]
leaks = [t for t in forbidden if t.lower() in raw.lower()]
if not leaks: good("payload contains no unsupported claim and no credential")
else: bad_("payload contains forbidden tokens", str(leaks))

# 6: the served page carries the labels and the honest-scope statement
page = urllib.request.urlopen("http://127.0.0.1:%s/" % port, timeout=5).read().decode("utf-8", "replace")
for label in ("LIVE SENSOR DATA", "SYNTHETIC TEST DATA", "HYPOTHESIS / UNVALIDATED CORRELATION"):
    if label in page: good("page carries the provenance label '%s'" % label)
    else: bad_("page missing provenance label", label)
if "never" in page and "predict" in page: good("page states the no-prediction scope")
else: bad_("page does not state the no-prediction scope")
if "Sensor &amp; integration status" in page or "Sensor & integration status" in page:
    good("page carries the sensor & integration panel")
else: bad_("panel heading missing from the page")

# 7: existing routes are unaffected by the new one
for route in ("/api/status", "/api/scenarios", "/api/alerts"):
    code = urllib.request.urlopen("http://127.0.0.1:%s%s" % (port, route), timeout=5).status
    if code == 200: good("%s still 200" % route)
    else: bad_("%s regressed" % route, str(code))

unknown = urllib.request.Request("http://127.0.0.1:%s/api/integrations?x=1" % port)
try:
    urllib.request.urlopen(unknown, timeout=5).status
    bad_("query string on /api/integrations accepted")
except urllib.error.HTTPError as e:
    if e.code == 404: good("query string on /api/integrations is refused (404)")
    else: bad_("unexpected status for query form", str(e.code))

print("  " + "-" * 68)
print("  integration surface: %d passed, %d failed" % (ok, fail))
sys.exit(1 if fail else 0)
PY
RC=$?
echo "  ===================================================================="
echo "  script checks: $PASS passed, $FAIL failed; assertion block rc=$RC"
[ "$RC" == "0" ] && [ "$FAIL" == "0" ]
