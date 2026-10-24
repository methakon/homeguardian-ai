# HomeGuardian AI — Status

**Last updated:** 2026-10-24

## Published endpoint

**https://homeguardian.berhampore.in/** is live, serving the native portal
(`hg_alexa_bridge --http`) from the deployment host behind a Cloudflare tunnel.
Verified from outside the build machine:

```
GET /                   200  embedded dashboard
GET /api/health         200  {"ok":true,"status":"up","simulated":true}
GET /api/status         200  state=Running, capture_enabled=false, alert_count=0
GET /api/alerts         200  {"ok":true,"alerts":[],"total":0}
GET /api/camera/status  200  {"configured":false,"reason":"no_camera_configured"}
GET /api/camera/frame   401  frames require the operator secret
```

The listener binds loopback only; the public name is accepted through one explicit
`HG_HTTP_PUBLIC_HOST` / `HG_HTTP_PUBLIC_ORIGIN` opt-in (empty by default, negative
cases tested). Mutations still require the operator-secret CSRF session.

This is a **published portal, not physical acceptance**: no camera is configured, no
observations feed this instance, `capture_enabled=false`, and the phone's private MCP
path is unchanged. The integration panel therefore reports 0 devices, "None live" and
0 fusion runs, which is accurate rather than a failure.

Alexa is **request/response by design** (owner-accepted): an intent is asked, the
skill reads the hub's own summary and speaks it. Nothing is pushed to a device, and
the `alerts_delivered` counter therefore means "released for that path", not
"spoken by an Echo".

## Real camera analysis wiring (motion + statistical change)

Owner-approved scope: wire the existing detectors through the real acquisition path,
report every other category as unavailable, and expose committed alerts through the
established Alexa request/response lane. No new classifier and no unsolicited
notification sender was added.

`--camera-analysis` now connects an explicitly named, registry-authorized source to:
bounded HTTP JPEG fetch (server cadence) → bounded `ffmpeg` decode to one 160x120 luma
plane → existing `CameraActivitySession`/`ActivityChangeDetector` → existing `Pipeline`
with the explicit `camera-motion-activity` allowlist → durable commit. Source identity,
consent, purpose, revocation and capture are re-read before every acquisition **and**
before every commit; any refusal resets baseline, pipeline history and policy. The
reserved synthetic registry id keeps fixture provenance `synthetic`, and status reports
`audio/speech/identity/visitors/falls/animals/emergencies` as unavailable.

`AlertPolicy::admit_durable` advances cooldown only after the durable write succeeds, so
a failed insert can no longer consume the cooldown window.

Verified: camera-analysis target **4 cases / 146 assertions** (CTest 1/1); independent
generated-JPEG end-to-end **40 frames → 40 observations → 2 commits**, IDs/count/severity
matched across rule output, SQLite, the separate MCP process and both ASK intents, with
five authorization negatives each blocked before any fetch; camera preview regressions
**6 tests**; portal smoke **38 checks**; runtime catalogue **20 cases / 40 ASK responses**;
full native **76,430 assertions / 790 cases**, all passing.

Not proven: physical camera acceptance, deployment, continuous video, and proactive
Alexa notification. See [Real-event pipeline acceptance](REAL_EVENT_PIPELINE_ACCEPTANCE.md).

## Camera-source follow-up and real-event acceptance

Integrated `experiment/hg-panasonic-service-transport-06` at
`98280bf4ffb1f3b8147f3e81256657735191adea` after `7e87535`; a second fetch returned
the same source tip. Preserved prior main records while resolving STATUS conflicts.
Corrected camera password-file loading, malformed-port handling, explicit
plaintext opt-in, operator authorization before camera reads, and capture-switch
enforcement. The private snapshot UI sends authorization in headers, never URLs.

Verified: full native **76,430 assertions / 790 cases**, camera regression **6
tests**, portal smoke **38 checks**, and the complete runtime catalogue **10
scenarios x capture on/off = 20 cases / 40 actual ASK SDK responses**. Exact
alert IDs, counts and severity agreed between native rule output, SQLite,
independent MCP and ASK handlers. These are local synthetic/fixture tests.

**Real-event goal remains incomplete.** Physical snapshots are not connected to
analysis; status/UI explicitly say so. Several requested semantic categories are
unimplemented, synthetic-only or require audio/environmental inputs absent from
JPEG. The established ASK/SQS lane is pull-based, not proactive notification.
See [Real-event pipeline acceptance](REAL_EVENT_PIPELINE_ACCEPTANCE.md) for all
nine categories, per-scenario results and the missing production gates. Nothing
was deployed, captured from hardware or changed in the working cloud/phone lane.

## Pre-final verification — 23:48 IST

Audited main `86f90db9a7fa5c9cb9fb8a73ad08c795e4455dce`: all **19 local verification
groups exited 0**. Results and limits: [Pre-final verification](PREFINAL_VERIFICATION.md).
Native: **76,419 assertions / 789 cases**; judge gate: **29 checks / 0 failures**;
ASK: **73 tests in each of two SDK environments**; offline camera adapters: **113
tests**; web client **26**, dashboard/security **161**, portal smoke **38**, and
Java/native bridge **21** checks passed. Android and worker/security suites also
passed as detailed in the report. Test listeners were torn down; deployed services
and physical sensors were untouched.

**Important:** private live-camera browser playback is not accepted on this
revision. Camera adapters are not yet connected to the web viewer; dashboard
signaling/security passes do not establish delivery of physical frames. Alert
durability, deployment/recovery, physical acceptance, historical packaging, and
final video/Devpost delivery remain explicitly open in the report.

## Submission positioning and camera-source follow-up

The submission uses the self-hosted native MCP 2025-11-25 Streamable HTTP route,
with the separately working classic ASK/SQS/private-worker/Android/native path.
Owner-attested voice results remain distinct from independent native/mailbox
verification. Synthetic detector/regression demonstrations are retained; neither
the ASK response nor private live video proves physical monitoring or durable
sensor-to-alert acceptance. No production Alexa+ integration is claimed.

Camera branch tip `31a54e0` was merged and pushed as `13b9647`; **26 offline
bubble-protocol tests passed**. The live-camera class was deliberately not run.
Source acquisition and private-dashboard viewing are separate milestones; live
view integration remains with the camera worker and is not accepted by this merge.

Follow-up regression work fixes two related correlation-time defects: an expired
out-of-order arrival hid valid earlier evidence, and derived motion/audio activity
used fresh wall time instead of its accepted ingestion timestamp. Both new tests
were observed failing before the fixes. The corroboration fixture now learns both
baselines outside the window and interleaves transitions chronologically; no
detection/confidence threshold was changed. Targeted tests passed **73 assertions
in 6 cases**. Full native verification passed **76,419 assertions in 789 cases**;
the canonical judge gate passed **29 checks, 0 failed**, including its explicit
synthetic mTLS detector-wiring/shutdown suite. Initial full runs caught fixture
clock leakage and missing test environment; those failures were resolved before
this final pass. Gate output names baseline `13b9647` plus the then-uncommitted
correlation-time fixes; those exact source changes are committed as `aefde23`.
It is not an unmodified-baseline certification.

Submission documents distinguish AWS aggregate metadata from raw sensor media,
and sensor-pipeline metadata retention from explicit local operator-upload storage.
The recording command includes `--serve`. The legacy `package_release.sh` still
enforces its historical security baseline: it is not a current-main release path,
and its drift guard has not been bypassed. Submit the public repository with an
explicit revision; no current release archive or clean-clone certificate is claimed.

Remaining production gaps: alert-persistence failure/ACK/cooldown semantics;
explicit ingress fusion evidence policy and ingress/MCP database alignment;
monitoring-health reporting rather than query-service availability; deployed
coordinator hardening and bridge recovery. These were not silently marked fixed.
Public video URL, final Devpost submission, and rendered-entry confirmation remain
unverified. No live service, VPN, camera, phone or AWS configuration was changed.

## Backup-plan branch integration

Owner-authorized merge of `backup-plan`, remote tip `7e96321`, into main after
`5d2c385`. Adds the bounded uploaded-media portal, embedded dashboard controls,
HTTP upload/feed/media/delete routes and transport body-framing fixes. The sole
conflict was in this document; both main's integration records and the historical
backup-plan acceptance below are preserved.

Merged-tree verification: rebuilt `hg_alexa_bridge` and `homeguardian_tests` in
the existing `hg_build` at two jobs. Portal/Alexa/MCP tests passed **33,585 assertions
in 55 cases**; native-HTTP tests passed **143 assertions in 25 cases**. The fresh
binary passed **38 loopback portal smoke checks**, including authentication,
CSRF, format/size/traversal refusals, upload/read/delete, lockout and unusable-store
handling. Smoke used a temporary config/data/upload store with capture disabled;
fixture bytes are synthetic, not evidence of real media capture or decoding.
Unit-test temporary paths now honor `TMPDIR` rather than hardcoding `/tmp`.

Build completed with warnings about aggregate initializers, timestamp formatting
and unused test helpers; not a warning-free build or a clean-from-scratch build.
No phone/VPS/AWS deployment, public listener or live media capture was performed.
HTTPS/reverse-proxy acceptance remains unverified; loopback Origin restrictions
were not relaxed. Existing worker, Android, coordinator and native-ingress
implementation files were retained unchanged.

## Worker-dispatch branch integration

Owner-authorized merge of `fix/hg-worker-dispatch-p0-48`, remote tip `e994396`,
into main after `3dc84a5`. Adds task-envelope/guard code, canonical scope checks,
comment-provenance checks, SQLite-backed one-use approval consumption, and
overlay/mTLS acceptance instruments. Merge was conflict-free. Imported identity
and policy files retain their historical network-worker assignment; merging them
does not enroll a worker or activate dispatch.

Merged-tree checks ran with a sandbox HOME and temporary state: guard 12/12,
envelope 16/16 (also passed a repeat run), adversarial scope/provenance checks,
grant consumption/replay/expiry checks, mocked WireGuard 7/7, Docker-boundary
safe/unsafe self-test, vocabulary conformance, and simulated reconnect 15/15.
One stale guard assertion expected admission with only an in-band approval
reference; corrected it to require `AUTHENTICATION_REQUIRED`, preserving the
hardened implementation. Python/shell syntax checks passed.

Vocabulary conformance still reports missing consent-withdrawal/missing-consent
coverage in these imported LAN fixtures; that is not a passed acceptance gate.
`test_poller.py` was not run: it imports an external profile-local poller and is
not self-contained in this checkout. Physical LAN acceptance and new real-socket
mTLS recovery fixtures were not exercised during this merge. Existing Android,
coordinator, native-ingress, release-gate and Alexa implementation files were
verified unchanged. No live poller, task dispatch, WireGuard enrollment, deployment,
firewall change, or device activity was performed.

## Service-transport branch integration

Owner-authorized merge of `experiment/hg-panasonic-service-transport-06`, remote
tip `6f8e2ad`, into main after `062bdc4`. Includes Android welcome/version handling,
resume-watermark adoption, refusal accounting, host regression fixtures, and the
camera source/loopback relay/private-interface gateway adapters.

The single merge conflict was resolved by retaining main's native producer
provenance default `real` from `ea8b1c1`; Java fallback and generated synthetic
fixtures remain `synthetic`. Imported branch audit/runbook documents describe
their historical branch state, not a replacement for current main acceptance.
Main's coordinator security, detector wiring and replay shutdown fixes are retained.

Merged-tree verification: production Java compiled against Android 34; isolated
JVM suites passed with 45 lifecycle, 43 schema, 27 transport-policy, 59 cross-receiver,
34 real-socket TLS and 19 synthetic-vector checks. Receiver vocabulary was derived
from current main (`HG_RECEIVER_REF=HEAD`), not the imported historical default.
Camera source/relay/gateway suites passed all 87 tests with fake media sources.
Live-camera tests and the Docker-based Java-to-C++ runner were not run; no physical
capture or deployment acceptance is claimed. No APK, VPN, VPS, AWS configuration,
or currently deployed Alexa service was changed.

## HG-PORTAL backup portal — acceptance on `backup-plan` (2026-10-23)

A backup operator portal built on the **existing** native demo rather than a parallel stack: no Node, no
framework, no new service. `hg_alexa_bridge --http` gains an uploaded-media store and four routes
(feed/upload/delete on the JSON handler, media bytes via a dedicated callback), plus `--uploads-dir`
defaulting outside the repository. The dashboard is the existing `src/web/index.html`, still embedded into
the binary at build time.

**Branch and SHA:** `backup-plan`, HEAD `7bc6e4a` (origin matches). Seven commits; `main` untouched, no
new worktree, nothing deployed or exposed, Alexa lane untouched. Alexa skill files were not modified.

**Exact commands and results:**

```
python3 scripts/portal_smoke.py                      38 passed, 0 failed
./build/homeguardian_tests "[portal],[alexa],[mcp]"  33585 assertions in 55 cases, all passed
cmake --build build -j4                              exit 0, 0 errors
```

**Verified live on loopback** (not merely compiled): the served dashboard carries the required label
verbatim, "Uploaded media — simulated camera feed", plus an explicit not-live-CCTV banner; a valid PNG
upload returns its own bytes on `/media/<id>`; a valid 5 s MP4 reports `duration_ms=5000` read from the
container; a script renamed `.png`, a non-allowlisted content type, path traversal, an unissued id and a
malformed delete id are all refused; upload and delete without CSRF are `403`; session minting is `401`
without the operator secret; and a portal whose upload store is unusable still serves an empty, labelled
feed instead of erroring or inventing content.

**Size sweep after the fix:** 999,967 · 7-digit length · 1 MiB · 4 MiB · 7 MiB → all accepted;
9 MiB → `413 too_large` from the store's 8 MiB ceiling.

**Measured resources:** RSS 6,648 → 7,520 KiB over 120 uploads of 256 KiB (delta 872 KiB) at ~1.8 ms of
CPU per upload.

**Defect fixed in this work (HG-PORTAL #30).** An upload above roughly 200 KiB returned `400 malformed`
where `413` was expected. The cause was the transport's `Content-Length` parser refusing any declared
length wider than **6 digits** (`value.size() > 6`) — correct while the only bodies were `"{}"`, wrong
once a route could accept 25 MiB. The guard is now 8 digits and the authoritative `body_cap` check is
unchanged. Two further transport defects were found and fixed earlier by running the component: the serve
loop stopped reading at the header terminator (bodies written in a second segment were truncated,
producing intermittent `400`s), and the generic framing check applied the 512-byte JSON cap to the upload
route before the route was known.

**Blockers / not verified:**

* The transport's `Origin` check requires an exact same-origin request from this loopback host and port,
  so a reverse proxy or tunnel presenting a different scheme/host **will be refused**. HTTPS exposure
  needs that check deliberately widened; not done, not tested. Everything is verified on loopback only.
* No per-user accounts — the operator-secret panel only.
* The app-degraded path (as opposed to an unusable upload store, now covered by the smoke) is verified by
  reading the dashboard's JavaScript, not exercised.
* **Not deployed:** no tunnel, no DNS record, no systemd unit installed, no external request ever
  received. Rollback is `kill <pid>`; the portal holds no external state.

**Rollback:** `kill <pid>` (or `systemctl stop <unit>` if supervised). The portal holds no external
state — uploads live in the directory passed as `--uploads-dir`, whose `index.jsonl` is rebuilt on load,
so deleting that directory discards uploads and leaving it keeps them. Nothing else is touched: no
firewall rule, no DNS record, no mail or SSH configuration, no container.

**Next owner:** Portal maintenance. Exposing it beyond this machine is
**not** authorised by this task and must not be attempted before the `Origin`-policy decision above is
made deliberately by the coordinator, with a separate scoped change.

The portal's own runbook is `docs/PORTAL_DEPLOYMENT.md`; its reusable test is `scripts/portal_smoke.py`.


## Alexa Python recovery and supplied-source diagnosis — 2026-10-23

The canonical voice adapter is `alexa_skill_python/`, restored on `main` in
`0b0fde1efbf141cfc8ce9ef592afce42fccc2080`, with exported-Lambda/intent contracts in
`8371e33de56d37e520ced9f96ae101b29c6c9e38`. Both commits were pushed to `origin/main`
and their file blob hashes independently verified using the GitHub API. Python
implementations were recoverable on other branches, not present in the reachable
history of the inspected main baseline. This is missing integration, not proven
deletion from main. The Node `alexa_skill/` prototype remains separate and unchanged.

Owner-supplied source was exercised with real `boto3==1.28.78` and
`ask-sdk-core==1.19.0`, using an explicitly synthetic backend (no AWS/backend
requests). Both dependencies installed and passed `pip check`. Two deterministic
source defects were reproduced for Launch, Help and Stop:

- First supplied version exports `build_skill().lambda_handler` instead of
  calling the factory. Invocation raises
  `TypeError: SkillBuilder.lambda_handler() takes 1 positional argument but 3 were given`.
- Second version calls the factory, but both versions' custom `_Response.build()`
  assign a plain string to `Response.output_speech`. With the real SDK the second
  version serializes `response.outputSpeech` as a string, not the required
  `{type: PlainText, text: ...}` or SSML object. That is an invalid Alexa response.
- The separately recovered historical `.reprompt()`/`.ask()` mismatch is a different
  source defect; it is NOT present in these supplied versions. Do not attribute
  the console failure to it without matching deployed source/logs.

The canonical implementation uses the real SDK response builder and callable
Lambda factory, validates malformed envelopes, escapes backend SSML text and
rejects invalid counts. MCP transport initializes an authenticated session before
the two allowlisted read-only tools, attempts authenticated teardown, bounds
response size/timeouts and refuses redirects/proxies/non-loopback destinations.

Local verification: **39 Python tests passed**, with both the canonical pinned
SDK model (`1.43.0`) and the owner's dependency combination (which locally resolved
SDK model `1.82.0`). Syntax compilation and dependency checks pass. Node regression:
**77 passed, 0 failed**, using existing root `hg_build/hg_alexa_bridge` and
`hg_build/homeguardian` binaries via `HG_BRIDGE_PATH`/`HG_APP_PATH`, not a fresh
C++ build. An initial Node attempt failed because binaries were absent from the
checkout's default build path; the explicit existing-binary run passed completely.

**Initial deployment checkpoint (see endpoint follow-up below): BLOCKED, not accepted.** The existing configured Skill ID
is available privately in root `.env` (`ALEXA_SKILL_ID`); it is not proof of the
console display name or endpoint. Amazon console access reaches a sign-in wall;
the configured AWS STS session is expired. The selected regional ARN, actual
Lambda runtime/handler, deployed package and matching CloudWatch group remain
unverified. No hosting, endpoint, permission, VPN or gateway change was made;
no duplicate skill was created and no Alexa deployment/simulator pass is claimed.
Use the existing skill only. Reauthenticate authorized console/AWS access to
inspect its exact target before deploying; do not send secret keys or bearer tokens.
The `utils.py` S3 helper is unused by the canonical handlers and needs no added
S3 permissions. Alexa-cloud-to-private-C++ connectivity is still not established;
unavailable voice responses are intentional, never fabricated household data.

Workspace decision: owner explicitly approved temporarily retaining the existing
`main` checkout beneath `.worktrees/alexa-main`, leaving the canonical root's
unrelated branch and uncommitted work untouched. Create no further project
folders/checkouts. Console/AWS reauthentication and the selected regional ARN
were not supplied at that initial checkpoint.

### Endpoint follow-up: valid AWS login, cross-account access denied

The owner subsequently supplied the existing Skill ID and all regional Lambda
ARNs, with default/North America in `us-east-1`, Europe/India in `eu-west-1`, and
Far East in `us-west-2`, each qualified by alias `Release_0`. Identifiers were
supplied privately and are not reproduced in public source. A fresh STS
read now confirms valid credentials, superseding the expired-session blocker.

The authenticated AWS account differs from the account in every supplied endpoint.
Exact read-only `GetFunctionConfiguration` requests against all three qualified
ARNs return `AccessDeniedException`: no resource-based policy permits this caller.
That verifies an access boundary, NOT a nonexistent/wrong function or an Alexa
invocation failure. No function package, alias target, runtime, handler or logs
could be read through that personal AWS session. No IAM/policy changes or function
invocations were attempted.

The three regions, separate account and console Code tab are consistent with
Alexa-hosted resources. AWS Lambda ARN selection does NOT contradict Alexa-hosted
hosting: Amazon explicitly provisions Lambda endpoints in these three regions
for hosted skills, separate from personal AWS resources. Actual hosting metadata
and deployed bytes still require authorized developer-console or ASK access.
Do not replace these endpoints, create another skill or broaden permissions to
work around the account boundary. Inspect the existing skill's Code deployment
state and use Code -> Logs with the default region (`us-east-1` as supplied) for
the simulator's actual invocation/error stream. Then compare the deployed source
with the two reproduced source defects before attributing the simulator failure.

References: [About Alexa-hosted Skills](https://developer.amazon.com/en-US/docs/alexa/hosted-skills/build-a-skill-end-to-end-using-an-alexa-hosted-skill.html)
and [Create and Manage Alexa-hosted Skills: CloudWatch logs](https://developer.amazon.com/en-US/docs/alexa/hosted-skills/alexa-hosted-skills-create.html).
Deployment and simulator acceptance remain **NOT VERIFIED**.

### Owner simulator acceptance and approved outbound mailbox follow-up

The owner subsequently reported successful deployment to the existing skill and
working Launch, Help, unavailable-alert and Stop responses. These are
**owner-reported simulator checks**, not independent deployed-source readback or
live C++ connectivity. Stop spoke Goodbye when tested inside an active session.

The owner approved an outbound-only AWS request/reply mailbox with narrowly scoped
queue/IAM resources and a bounded VPS worker, and read-only Mi A3 diagnostics.
Two dedicated queues (`homeguardian-alexa-requests`, `homeguardian-alexa-replies`)
were created in the owner's account in `us-east-1`; readback verified managed
encryption, TLS-required policies, 60-second retention and 16-KiB message caps.
No hosted-role grant, AWS access key or running worker was created at this stage.
The exact skill Lambda execution-role ARN is still required for narrow publisher
trust; the logs-viewing SSO role is not an acceptable substitute.

Fresh phone evidence distinguishes the fault: its read-only native self-probe
reports `Running`, `read_only=true`, `capture_enabled=false`; installed APK bytes
match the previously accepted artifact. The VPN handshake is fresh and the
coordinator's edge TCP connection comes from the phone peer. However private
Initialize/status requests then returned HTTP 200 with only `{}`, not valid JSON-RPC.
HTTP/auth/session admission therefore MUST NOT be called a successful native
initialize or tool result. Probe sessions were torn down. **SUPERSEDED 2026-10-23
21:43 IST** — the root cause was the installed bridge retaining a stale native
session; after reconnecting only that bridge, valid JSON-RPC initialize/tool results
were obtained (see [live path verification](ALEXA_LIVE_PATH_VERIFICATION.md)). The
permanent bridge-side recovery fix is still open.
No phone reinstall/restart, capture, VPN, firewall, mail or public-listener change
was made. Application-private access was denied as expected and not bypassed.

The initial mailbox worker had 11 local fixture tests, including strict tool and
destination allowlists, stale/replayed request refusal, aggregate-only replies,
authenticated MCP session cleanup and HTTP-200-empty-object rejection. It was
source-tested only at that point. **SUPERSEDED** — the worker is deployed, active and
boot-enabled, and real correlated request/reply round trips have since passed
([live path verification](ALEXA_LIVE_PATH_VERIFICATION.md)).

The Alexa-side mailbox client and explicit backend transport selector are now
implemented with matching producer/consumer contract tests. The combined Python
suite passes **71 tests** under both tested SDK-model combinations; dependency
checks and syntax compilation pass. Python 3.8 syntax parsing also passes, but
this is not execution against the managed runtime. Tests caught and corrected
rejection of the native zero-alert payload's empty severity string; a zero is
accepted only as an actual validated tool result, never as an unavailable default.

Managed hosting can use a gitignored, non-secret `mailbox_config.json` with exactly
four deployment settings instead of requiring Lambda environment-edit permissions.
It contains only region, queue URLs and the publisher role ARN, never credentials.
The default deployment stayed on the existing loopback/unavailable path until
mailbox mode was explicitly configured. No mailbox Code deployment, running VPS
worker, dedicated worker credential or hosted role trust was claimed at that point.
**SUPERSEDED** — mailbox Code deployment was performed by the owner, the dedicated
worker credential and VPS service exist, and genuine mailbox round trips pass; the
execution-role ARN was supplied and the empty bridge reply was a stale-session
effect. Owner-attested Alexa replies: status `Running`/capture disabled, alerts
none recorded. See [live path verification](ALEXA_LIVE_PATH_VERIFICATION.md).

### Execution-role received; configuration moved to the existing .env

The owner supplied the actual hosted Lambda execution-role ARN. A dedicated
publisher role was created in the owner's account, trusting only that exact
principal (not the logs SSO role or the entire hosted account). Readback verified
the trust document and the inline policy's request-send/reply-consume directions,
exact queue resources and TLS condition; no broad attached policies were added.
IAM policy simulation verified 12 action/resource decisions, including denial of
opposite-direction and destructive queue actions. This is not a real hosted-role
AssumeRole or live Alexa-to-native verification.

At owner direction, the six Alexa operational settings now live in the existing
canonical project `.env`; unrelated values were left untouched. The generated
mailbox JSON file was removed. The loader reads only selected Alexa keys from
`HG_ALEXA_ENV_FILE`, or `.env` beside the deployed modules; environment values
take precedence. Local application configuration validation succeeded without
printing/loading unrelated credentials. Do not copy the full workstation `.env`
into Alexa: deploy only the selected mailbox settings. No `.env` values are tracked
or published in this change. No worker key, running worker or mailbox Code
deployment was created; the empty private bridge replies remain unresolved.

The dotenv revision passes **72 Python tests** under both tested SDK combinations,
plus syntax and dependency checks. The canonical `.env` is confirmed ignored and
untracked; only code/tests/sanitized documentation are published.

### RESOLVED — Alexa → mailbox → native C++ path re-verified (2026-10-23 21:43 IST)

The earlier checkpoints below that describe an empty `{}` bridge reply and "no
accepted mailbox round trip" are **SUPERSEDED**. Both are now proven against the
deployed components, with no redeployment, restart, reinstall or configuration
change and with capture still disabled:

- **Native JSON-RPC, not HTTP-200/`{}`:** through the deployed private endpoint,
  `initialize` returned `jsonrpc 2.0`, id 1, `protocolVersion 2025-11-25`,
  `serverInfo {homeguardian-mcp 1.0.0}` and a session id; `tools/list` returned
  exactly `get_alert_summary` and `get_homeguardian_status`; both `tools/call`s
  returned genuine read-only payloads (`Running`, `capture_enabled=false`;
  `alert_count=0`, empty severity); `DELETE` closed the session.
- **Real mailbox request/reply:** operator-origin IAM (not root, not the hosted
  role) obtained validated correlated replies from the deployed worker —
  2.634 s for status and 1.190 s for alerts, 2 of 2 correlated.
- **Deployed code and permissions:** worker, unit and `service.js` SHAs match
  `main`; the deployed `coordinator.js` (content digest `216f9d37…`) **predates**
  the committed coordinator hardening (commit `8458cd8`, digest `214e9cee…`), so
  that fix remains source-tested and
  undeployed. Worker identity keeps one Active key, no groups, no managed policies
  and only the two TLS-required, queue-scoped statements.

Full commands, vantage points and limits: [live path verification](ALEXA_LIVE_PATH_VERIFICATION.md).
Still open: Alexa-Lambda-origin re-run (hosted Code unchanged since deployment),
sustained reliability after the earlier unavailable launch, permanent bridge
stale-session recovery, reboot/network recovery, deployed coordinator hardening,
and any physical sensor-to-alert acceptance.

### Production detector wiring corrective batch — 2026-10-23

Implemented on `main`, **SOURCE + SYNTHETIC-mTLS TESTED / NOT DEPLOYED**. The TLS
ingress now feeds `IngestDetectors` only after an observation's delivery has
completed and only when it was not an idempotent repeat: refused, replayed,
identity-mismatched, consent/capture-blocked and sink-failed observations never
train a detector or refresh liveness. Derived detector output still reaches the
single existing pipeline. A bounded maintenance worker (250 ms, dedicated condition
variable) evaluates quiet devices without waiting for an inbound frame, and
re-authorizes each idle delivery against the credential bound to that device's
accepted evidence plus pairing/revocation/consent/capture. Offline transitions no
longer claim `Provenance::real` or invent a `security_monitor` consent. The
optional ingress also links now: `src/core/ActivityEpisode.cpp` was missing from
`HG_INGEST_CORE_SOURCES`.

Verified (re-run on the reviewed tree): real mutual-TLS regression
`python3 tests/net/run_production_ingress.py` **392 assertions in 6 cases passed**;
`ctest -R production_ingress_tls` **1/1 passed**; shipped-binary synthetic-CA smoke
**accepted=3 rejected=0**, clean SIGTERM exit 0; full native suite from `hg_build`
with `HG_SOURCE_DIR` set **76,095 assertions in 777 cases, all passed**; canonical
gate `bash scripts/verify_judge_release.sh` **PASS, 29 checks, 0 failed**, including
the new `NATIVE_DETECTOR_WIRING` gate that executes this suite. Implementation-time RED (no derived
output, no idle sweep, idle delivery after rotation, a queued connection starved by
the maintenance wait) was observed before each fix; the GREEN runs above were
re-executed independently. See [detector wiring](DETECTOR_WIRING_FIX.md).

Review follow-up fixed two things: `request_stop()` used to flush durable replay
state while a worker could still be writing it (a data race), now flushed only after
every worker is joined; and shutdown is now regressed with **no inbound frame** plus
stop-racing-revocation cases. These ingress cases are tagged `[.]` and run through
that ctest/runner rather than the default binary run — but they are part of the
canonical gate above, and a missing TLS prerequisite yields `SKIP`, never `PASS`.
These are synthetic-certificate, synthetic-observation host tests, not hardware.
Limits: fixed 250 ms availability cadence, idle delivery re-checks
registry state and the bound credential rather than the live socket, and default
fusion policy keeps detector evidence advisory. Nothing was deployed to the phone,
VPS, Docker service or cloud; no physical observation or alert is claimed.

### Producer provenance corrective batch — 2026-10-23

Implemented on `main`, **SOURCE/Java-COMPILE/HOST-PARSER TESTED / NOT INSTALLED**.
The native acquired-observation default now emits canonical `real` instead of
`real_device`; Java absent-field fallback stays `synthetic`. The receiver vocabulary
and authenticated provenance derivation were not weakened. Source-derived tests
consume the actual native default through the Java field extractor and real C++
wire decoder, including all authorization gates and legacy-token rejection.

Observed RED: JVM schema 42 passed/1 failed; native parser rejected the producer
default with `unknown provenance_claim`. Parent GREEN: production Java compilation
against android-34 succeeded; isolated JVM lifecycle **45 passed**, schema **43
passed**, no failures; rebuilt main native regression **24 assertions / 1 case
passed**. These are source/JVM/host proofs, not JNI execution or APK/device evidence.
See [provenance wire fix](PROVENANCE_WIRE_FIX.md). Detector wiring is next; its real
TLS ingress build currently exposes a missing ActivityEpisode link dependency,
which will be addressed with that batch. No live runtime changed.

### Coordinator security corrective batch — 2026-10-23

Implemented on `main`, **SOURCE-TESTED / NOT DEPLOYED**. Malformed HTTP targets,
non-object HTTP/WS JSON and invalid credential types are rejected without crashing
the coordinator; async HTTP entry/body failures are contained. Token rotation
synchronously removes existing channel authority and rejects pending requests;
unchanged-token reload preserves the channel. Superseded/revoked sockets cannot
heartbeat or resolve requests. Client sessions now have finite defaults (1,024
active/initializing sessions, 30-minute idle expiry), reserved initialization
capacity, and no retained/advertised session after failed or disconnected init.
Rejected oversized bodies cannot dispatch later on `end`.

`bash scripts/test_outbound_bridge.sh`: **13 security regressions passed, 0
failed/skipped; existing real-native bridge checks 46 passed, 0 failed**. This run
used the freshly built main-checkout `hg_build/hg_mcp_server`, real workstation
loopback HTTP/WS for recovery checks, and synthetic local credentials. Node syntax
and `git diff --check` passed. Missing native binaries fail rather than silently
skip. Test temporary files remain inside this project. Full C++ regression/build
preparation is separate; no claim of phone/VPS deployment or physical acceptance.
See [coordinator security fixes](COORDINATOR_SECURITY_FIXES.md).

The next authorized batches are producer provenance and detector wiring, in that
order. No live service, VPN, capture, credential or cloud configuration was changed.

### Current Alexa acceptance — 2026-10-23

**Owner-reported deployed Custom Skill functional acceptance:** the owner reported
successful Code deployment, then received `The hub state is Running. Capture is
disabled.` and, using the full invocation `ask home guardian what are my recent
alerts`, `There are no alerts recorded for your home.` Both read-only voice
functions have returned the expected native readings. This is owner-attested
simulator evidence; deployed source bytes and Skill I/O were not independently
retrieved. It is not Smart Home, camera, Alexa+ or production certification.

A subsequent independent **operator-origin mailbox smoke passed 4/4** real
requests (two status, two alerts), with genuine read-only results and no capture.
The worker health check showed active/running with zero restarts. This smoke is
not a Lambda-origin latency reproduction and does not resolve intermittent failures.

**Reliability remains OPEN:** the owner also observed an unavailable launch before
a successful status response. The worker journal contains a `ReadTimeoutError`,
but it is not correlated to that failed invocation; the cause remains unproven.
Bare follow-up alert phrases were silent, whereas the full invocation succeeded.
The status handler supplies no reprompt; session/routing is consistent with that
behavior, but Skill I/O is needed to establish exactly which requests reached it.
Keep using full invocations for isolated acceptance tests. Do not infer zero alerts
from silence, replace unavailable results with cached readings, or change timeouts
without reproducing the failure. No additional skill deployment, phone/VPS restart,
capture enablement, VPN change or queue/IAM configuration change was performed for this
follow-up. Long-outage, reboot and changed-network recovery remain unverified.

### Historical checkpoint: real mailbox/native path recovered; Code deployment pending

**Superseded 2026-10-23 21:43 IST** by [live path verification](ALEXA_LIVE_PATH_VERIFICATION.md):
Code deployment happened, and native + mailbox round trips are re-verified. Retained
for the recovery history below.

Inspection of the accepted installed APK's actual DEX confirms its bridge sets
the required Accept header. A memory-only trace of the already established phone
control connection confirms the phone itself sent `body={}`/status 200; raw frames
and credentials were not printed or persisted. Source inspection identifies a
stale-session recovery defect: native HTTP rejects unknown/expired sessions with
404, while the bridge retains its session ID unless a specific error string is
returned and masks the native status as 200. Reconnecting ONLY the outbound bridge
through the app controls recovered both genuine read-only native tools. The C++
service PID was unchanged; no native restart, APK reinstall, VPN or capture change
was performed. The permanent Android-side session-recovery fix remains open.

The dedicated worker IAM identity was provisioned with only request-consume and
reply-send permissions. An owner-only credential file was installed on the target
VPS; STS verified the worker user, not personal/root identity. Actual AWS requests
in the wrong queue direction were denied. Two failed installer attempts (restricted
PATH, then new-key propagation) revoked their issued keys before retry; no root
credentials were transferred. The successful install verified source/unit bytes,
file permissions, unchanged mail/coordinator PIDs and an unchanged TCP listener set.

`homeguardian-alexa-mailbox` is active and boot-enabled, with 96-MiB RAM and 10% CPU
caps. The bounded process is supervised and privately keeps the native control
session active once per minute without sending unsolicited readings to AWS. This
is a mitigation, not proof of recovery after a long outage. The source revision
passed **73 Python tests** before deployment. The publisher trust and worker role
remain scoped in opposite directions; no broad AWS policy was added.

**Real operator-origin AWS -> VPS -> phone C++ -> AWS verification PASS:**
status returned `Running`, `read_only=true`, `capture_enabled=false` in **2.085 s**;
alerts returned `alert_count=0`, empty highest severity, `read_only=true` in
**1.659 s**. Correlation, tool schema, expiry and session cleanup were validated.
These are genuine native results, not cached evidence or local fixture outputs.

**At this historical checkpoint, Alexa-Lambda origin / simulator-to-native was NOT VERIFIED** (later owner-reported
simulator replies and the re-verified native/mailbox path supersede the deployment gate). The Code package
has not been updated from this session. The browser reaches Amazon Sign-In, no
saved login is available, and secure login prompting is unavailable in this CLI.
The worker's publisher-role trust is data access, not hosted-Code administration.
A private five-file Code source bundle was prepared with selected `.env` settings,
verified against source, protected and excluded from Git. It contains no worker
key, MCP bearer or personal AWS credentials. Authorized console deployment and
the resulting real simulator call are the remaining release gates. Reboot,
changed-network and long-outage recovery remain unverified.

## On-phone analyzer candidate — not yet deployed

The approved in-place update is being prepared on the isolated Android branch.
The current host analyzer suite passes **34 checks, 0 failures**, including a
synthetic alert persisted to SQLite and the exact id recovered after analyzer
recreation. Bridge regression suites also pass: **4 Python tests**, a nested Java
UI harness with **8 scenarios**, and **50 route-policy checks**. Initial alert
fixture failures were traced to a test/phone configuration mismatch; classic
fusion remains the default and its confidence gate is unchanged.

A fresh ARM64 APK build is in progress; no installation or phone acceptance is
claimed. The latest ADB inventory is empty, blocking handset deployment and
readback. Existing phone/VPN/MCP evidence below is historical, not a fresh health
check. Physical sensor acceptance remains open. See the
[candidate verification checkpoint](ANDROID_EDGE_ANALYZER_UPDATE.md).

## Private remote MCP readiness — 2026-10-23

**04:02 IST — REMOTE MCP VERIFIED OVER THE PRIVATE VPN.** All four reported
blockers are resolved: the phone key is enrolled with its own /32, its profile is
configured without exporting its private key, cloud UDP ingress admits the current
verified source, and the private coordinator runs as an active boot-enabled service
on the tunnel address. Through that coordinator the real handset answered
initialize, listed exactly the two native read-only tools, passed both
authentication stages on each tool call, rejected missing/incorrect client bearers
with 401 and a mutating tool with 400, and still answered while the screen dozed.
Public MCP ports remain unreachable and mail was untouched.

Deployment also exposed and fixed a real route-policy defect: refusing any
destination covered by a non-VPN route made the bridge undialable on any real
Wi-Fi plus WireGuard handset, because Wi-Fi always advertises a default route. The
policy now follows kernel longest-prefix priority; the unsafe overlap is still
refused. Off-device policy checks went 44 → 50 and failed before the fix. Phone
update kept the existing signer and both native libraries byte-identical.

Still open: a separately reserved worker peer is not enrolled (needs local root),
so remote MCP is proven from the VPS client listener; restart/reboot recovery and a
changed public address are untested. Details:
[verified connection and remaining blockers](PRIVATE_VPN_REMOTE_MCP_READINESS.md).

**03:25 IST update — VPN WORKING, COORDINATOR DEPLOYED, PHONE BRIDGE PENDING:**
phone enrollment, saved profile configuration and current-source cloud UDP ingress
are applied. A fresh authenticated handshake, assigned phone tunnel address and
two successful post-handshake pings were verified. The private coordinator is active
and boot-enabled with VPN-only bindings, exact ingress restrictions and retained
application authentication. Public MCP ports remain unreachable and mail service
processes were unchanged. The installed phone APK lacks bridge code; its approved
in-place update and an actual remote MCP readback remain pending. See the
[execution checkpoint](PRIVATE_VPN_REMOTE_MCP_READINESS.md).

Earlier read-only baseline, superseded for resolved items:

**BLOCKED:** the existing VPS WireGuard server is up and boot-enabled, but the
phone's supplied public key is not enrolled and its matching local profile has
no address or peer configured. There is no current phone handshake. Cloud UDP
ingress is restricted to an old single source that does not match the worker's
current public IP. The private coordinator is not deployed. Phone MCP remains
healthy, authenticated, read-only and loopback-only; no public MCP port was opened.
See [verified facts, minimum preparation and approval gates](PRIVATE_VPN_REMOTE_MCP_READINESS.md).
The phone already has a boot-registered foreground service; restart after first
unlock is configured, but a real reboot was not re-tested.

## P1 physical-integration checkpoint — 2026-10-23

**Existing phone runtime verified:** a fresh authenticated on-device MCP self-probe
at 02:17:34 IST completed initialize, tool discovery and a status call in 51 ms.
The Mi A3's foreground EdgeService reported engine `Running`, `read_only=true` and
`capture_enabled=false`. The core runtime and read-only MCP are working on the
phone; this is separate from acceptance of the physical alert pipeline below.

**NOT ACCEPTED:** Android → C++ → durable alert → MCP physical acceptance remains
open. The Mi A3 sensor app is installed and real 320×240 camera processing was
observed, but recorded delivery counters stayed at zero and the isolated audit
database contains no physical alert. APK installation and host tests are not
end-to-end acceptance.

Candidate fixes cover network permission, wire vocabulary, Android keystore
compatibility and transport identity. USB repeatedly re-enumerated with kernel
error `-71`; a temporary authenticated Wi-Fi ingress was authorized, but phone
configuration and delivery were not completed. Test listeners are now stopped;
the existing EdgeService was preserved. See
[the acceptance report](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md) for measured
evidence, candidate-only changes, provenance limits and remaining exit criteria.

## Security Audit & Release Candidate Status
- **Candidate `52d3d1436e115fe9061127a35f33b35d10c8dd73`:** Security review results: P0-1 PASS, P0-2 PASS, P0-3 FAIL. Failed due to hardcoded physical device seed secrets, unauthenticated re-pairing, and lack of heartbeat sequence replay protection.
- **Successor Candidate (Branch `fix/hg-sec-06-device-secrets`):** Implements complete physical-device credential remediation (Fix 1-5: CSPRNG 256-bit secrets, POSIX 0700/0600 storage protection, authenticated re-pairing, sequence replay protection across restarts). Complete details in `docs/RELEASE_AUDIT_STATUS.md`.

## Current Phase

Simulator-first development with separately authorized physical-device acceptance.
The deterministic simulator, shared audio broker and consent-gated acquisition
remain distinct from physical verification. Camera capture on the Mi A3 was
authorized and observed; microphone capture was not run in this checkpoint.
Physical alert delivery and restart recovery remain unverified, not complete.

## Completed Work

| Task | Evidence | Date |
|------|----------|------|
| Repository initialized | `homeguardian-ai` on `main` branch | 2026-10-09 |
| Initial documentation | README, ARCHITECTURE, ROADMAP, DEVELOPMENT, TESTING, DECISIONS, STATUS | 2026-10-09 |
| Skill inventory | `docs/SKILL_INVENTORY.md` — 24 relevant skills documented | 2026-10-09 |
| Three-tier architecture | `docs/ARCHITECTURE.md` updated with Tier 1/2/3 | 2026-10-09 |
| CMake build system | `cmake` configures, `make -j4` builds `homeguardian` executable | 2026-10-09 |
| Application entry point | `main.cpp` — config load, signal handlers, main loop | 2026-10-09 |
| Structured logging | spdlog with console + file sinks, configurable level | 2026-10-09 |
| Configuration loading | nlohmann/json, validation, defaults, save/reload | 2026-10-09 |
| Graceful shutdown | SIGINT/SIGTERM handled, atomic flag, clean exit | 2026-10-09 |
| Phase B fixes | Logger::get() throws when uninitialized; test thread safety fixed | 2026-10-09 |
| Event model | Event, Alert, EventValidator, UUID — strongly typed, immutable, JSON serializable | 2026-10-09 |
| Event source | IEventSource interface + SimulatedEventSource for testing | 2026-10-09 |
| Correlation rules | ICorrelationRule interface + TimeWindowCorrelation implementation | 2026-10-09 |
| Pipeline | Synchronous Pipeline with bounded history, validation, correlation | 2026-10-09 |
| Detection: Observation model | `src/core/Observation.{h,cpp}` — provenance/detector/status; converts to Event, reuses pipeline | 2026-10-12 |
| Detection: AnomalyDetector | `src/core/AnomalyDetector.{h,cpp}` — explainable z-score baselines, warm-up, missing-observation, bounded | 2026-10-12 |
| Detection: FusionEngine | `src/core/FusionEngine.{h,cpp}` — confidence propagation, conflict->inconclusive, time window | 2026-10-12 |
| Detection: SyntheticActivityDetector | `src/core/SyntheticActivityDetector.{h,cpp}` — deterministic synthetic detector, no I/O | 2026-10-12 |
| Detection tests | `tests/core/test_detection.cpp` — 20 cases / 67 assertions; full suite 275/2071 | 2026-10-12 |
| Config extension | max_event_history, correlation_window_ms, alert_confidence_threshold | 2026-10-09 |
| Application integration | Application::process_event() delegates to Pipeline | 2026-10-09 |
| Friction log | 6 verified incidents documented | 2026-10-09 |
| SQLite persistence | IDatabase, EventRepository, AlertRepository, SchemaManager, PersistenceManager | 2026-10-10 |
| SQLite amalgamation | Compiled directly into project (no system dev headers required) | 2026-10-10 |
| Family profile model | `core/FamilyProfile.h` — versioned, optional age band, no sensitive attributes | 2026-10-10 |
| Consent model | `core/ConsentRecord.h` — granted/denied/withdrawn, default-deny, expiry | 2026-10-10 |
| Routine model | `core/Routine.h` — validated schedule and time zone, configuration only | 2026-10-10 |
| Phase E repositories | ProfileRepository, ConsentRepository, RoutineRepository | 2026-10-10 |
| Schema migration v1→v2 | Additive, transactional, rerunnable; preserves event/alert data | 2026-10-10 |
| Extended result codes | Constraint subtypes distinguished (PK/UNIQUE vs FK) | 2026-10-10 |
| Phase E configuration | media_capture_enabled/cloud_processing_enabled default OFF | 2026-10-10 |
| Test suite | 57 test cases, 241 assertions, all passing | 2026-10-10 |
| Simulated sensor source | `core/SimulatedSensorSource` — deterministic, synthetic-tagged, timestamps, malformed, disconnect/reconnect | 2026-10-10 |
| Consent gate | `core/ConsentGate` — single fail-closed decision point | 2026-10-10 |
| Consent-gated acquisition | `core/ConsentGatedAcquisition` — denied requests produce no payload | 2026-10-10 |
| F1 tests | Consent gate (9 cases), simulated sensor (6), gated acquisition (4) | 2026-10-10 |
| IMediaDevice interface | `core/IMediaDevice.h` — explicit states, start/stop, deterministic cleanup; no hardware code | 2026-10-10 |
| ConsentGuardedDevice | `core/ConsentGuardedDevice` — gate before init/start/acquire; forces stop on withdrawal/expiry/permission/failure | 2026-10-10 |
| Lifecycle tests | 9 mock cases (denied start, authorized start/stop, withdrawal, expiry, failure, permission revocation, destructor) | 2026-10-10 |
| NDK install | NDK r26d (26.3.11579264) installed side-by-side via official Google download | 2026-10-10 |
| Android camera backend | `backend/android/NdkCameraDevice` — NDK camera API; fail-closed; cross-compiles arm64-v8a | 2026-10-10 |
| Android audio backend | `backend/android/AAudioCaptureDevice` — AAudio; fail-closed; cross-compiles arm64-v8a | 2026-10-10 |
| Delivery-time consent gate | `ConsentGuardedDevice::authorize_delivery()` — re-checks before every frame/sample delivery | 2026-10-10 |
| Fake-backend mock tests | `core/FakeMediaDevice` + `test_fake_device_delivery.cpp` — 8 host cases | 2026-10-10 |
| Defect fix | Errored device no longer resurrected to Idle; camera stop() releases handles | 2026-10-10 |
| Confirmed device | MediaTek MT6580 "Ruby" (model string cosmetic), Android 8.1.0 / API 27, armeabi-v7a, ~962MB RAM | 2026-10-10 |
| armeabi-v7a/API 27 build + on-device self-test | `hg_ondevice_selftest` built for device ABI/API; 17/17 non-capture checks pass on device via adb | 2026-10-10 |
| JDK + SDK platform 27 | Temurin JDK 17 (user-writable) and SDK platform 27 / build-tools 27.0.3 installed | 2026-10-10 |
| APK harness (non-capture) | Minimal debug-signed APK built with JDK+SDK+NDK (no Gradle); keystore outside repo; capture disabled by default | 2026-10-11 |
| Test suite | 93 test cases, 424 assertions, all passing | 2026-10-10 |
| Simulator-first backend | `backend/simulator/SimulatedSensorBackend` — deterministic camera frames + PCM mic samples; bounded ring; Config struct (dimensions, frame-rate, channels, test signals) | 2026-10-10 |
| Linux sensor discovery | `backend/linux/SensorDiscovery` — V4L2 + PipeWire metadata enumeration; opens no device | 2026-10-10 |
| End-to-end simulator workflow | `test_e2e_simulator.cpp` — camera/mic/speaker → consent-gated delivery → synthetic processor → pipeline rule/alert → safe speaker response; failure & consent scenarios | 2026-10-10 |
| Dual logical-device + shared audio broker | `backend/simulator/LogicalDevice` + `AudioBroker` — two independent logical devices share ONE mic (single capture) with per-consumer consent fan-out and ONE bounded arbitrated speaker | 2026-10-11 |
| Product-readiness audit | Fixed stale README, hardcoded CMake paths → `${CMAKE_SOURCE_DIR}`, hardened .gitignore; documented build reproduced fresh | 2026-10-11 |
| Alexa custom skill prototype | `alexa_skill/` — ASK SDK v2 handler + 13 Node unit tests (optional, separate from core; no deployment) | 2026-10-10 |
| Test suite | 147 test cases, 1546 assertions, all passing | 2026-10-11 |
| Runtime integration | `core/SimulatorRuntime` wired into `Application::run()` — the running app exercises the consent gate, shared audio broker, event pipeline, and clean shutdown; 7 runtime integration tests | 2026-10-11 |
| Test suite | 154 test cases, 1583 assertions, all passing | 2026-10-11 |
| Event-to-alert integration | `SimulatorRuntime` routes AUTHORIZED synthetic observations through `Application::process_event()` → Pipeline validation + TimeWindowCorrelation + alert; 9 event-to-alert tests; capture-off/withdrawal/missing-consent produce no events | 2026-10-11 |
| Test suite | 163 test cases, 1617 assertions, all passing | 2026-10-11 |
| Runtime hardening v1 | `core/AlertPolicy` (dedup/cooldown/re-arm, injectable clock, bounded); explicit `AppState` lifecycle + partial-startup cleanup; 27 hardening tests; reproducible demo `docs/DEMO_SIMULATOR.md` | 2026-10-11 |
| Test suite | 190 test cases, 1712 assertions, all passing | 2026-10-11 |
| Runtime readiness v2 | `core/PersistenceAudit` (durable alert metadata via existing SQLite, historical-vs-current distinguishability, retention, fail-safe degraded) + `core/RuntimeStatus` (bounded privacy-safe counters); restart distinguishability verified; 10 audit/status tests | 2026-10-11 |
| Test suite | 200 test cases, 1753 assertions, all passing | 2026-10-11 |
| Runtime readiness v3 | `core/AlexaContract` (versioned, authenticated, read-only integration contract + auth boundary; 14 contract tests) + Node adapter tests (12); SQLite integrity/migration tests (7); privacy/consent regression tests (7); plus P1 rule-specific clearing (3) and P2 scheduled retention (4) landed this cycle | 2026-10-11 |
| Test suite | 235 test cases, 1925 assertions, all passing | 2026-10-11 |
| Runtime readiness v4 | `hg_alexa_bridge` (local NDJSON stdio bridge over the real AlexaContract) + Node process-boundary integration tests (14) and restart tests (4) driving the actual compiled C++ binary; reproducible demo `docs/demo_alexa_integration.sh`; no network/deploy | 2026-10-11 |
| Test suite | 235 test cases, 1928 assertions, all passing; Node 43/43 (13 handler + 12 contract + 14 integration + 4 restart) | 2026-10-11 |
| Security readiness v5 | Deployment-readiness & security review of the Alexa boundary; fixed LOW-1 (bridge now reflects real config capture state, fail-safe); +6 C++ `[alexa]` security-regression cases +2 Node integration cases; report `docs/SECURITY_READINESS_V5.md`; HTTPS endpoint = CONDITIONAL GO (blocked on real authn/TLS/rate-limit/secrets) | 2026-10-11 |
| Test suite | 241 test cases, 1960 assertions, all passing; Node 45/45 (13 handler + 12 contract + 16 integration + 4 restart) | 2026-10-11 |
| Security readiness v6 | `core/AlexaCredentialStore` (real bearer-token verification: expiry/revocation, constant-time compare, 0600 secure-file load, fail-closed, no mock fallback; B1+B4); bridge `HG_BRIDGE_SECURE_CREDS` mode; 14 `[creds]` + 5 secure-mode Node cases; report `docs/SECURITY_READINESS_V6.md` | 2026-10-12 |
| Test suite | 255 test cases, 2004 assertions, all passing; Node 50/50 (13 handler + 12 contract + 21 integration + 4 restart) | 2026-10-12 |
| HTTP adapter v7 | `src/bridge/hg_http_adapter.js` (local-only loopback HTTP adapter over the real C++ bridge; stdlib `http`, no new dependency; method/route allowlists, size/timeout/concurrency limits, rate limiting, generic auth failures, graceful shutdown); 18 `http_adapter` Node tests; report `docs/HTTP_ADAPTER_V7.md`; NOT TLS, NOT production | 2026-10-12 |
| Test suite | 255 test cases, 2004 assertions, all passing; Node 68/68 (13 handler + 12 contract + 21 integration + 4 restart + 18 http_adapter) | 2026-10-12 |
| Hackathon P0 | README corrected (removed false MCP/Ring claims; documented implemented vs simulated vs planned); simulated Alexa+ web experience (`src/web/` + `src/bridge/hg_demo_server.js` + `demo.sh` + `config/homeguardian.demo.json`); bridge `run_scenario` mode (real pipeline → real alerts → durable persistence); 8 `web_demo` Node tests; fixed CMake out-of-source path bug + test CWD-isolation bug; `docs/DEMO_WEB.md` | 2026-10-12 |
| Test suite | 255 test cases, 2001 assertions, all passing; Node 76/76 (13 handler + 12 contract + 21 integration + 4 restart + 18 http_adapter + 8 web_demo) | 2026-10-12 |
| Detection framework (v8) | Observation model, AnomalyDetector, FusionEngine, SyntheticActivityDetector; suite now 275 cases / 2071 assertions; Node 76/76 | 2026-10-12 |
| Stabilization pass (v9) | Verified 275/2071, ctest 100%, Node 76/76, fresh out-of-source build green, ASan reconfirmed on current source (275/2071, zero sanitizer errors), demo loopback+scenario OK, persistence-after-restart OK | 2026-10-12 |
| Demo completion (v10) | Wired v8 detection/fusion framework into the demo scenario: SyntheticActivityDetector + FusionEngine now run in run_demo_scenario, feed inference events through the EXISTING pipeline, and surface a synthetic detection block in the scenario JSON + web UI. Fail-closed capture-off path unchanged (detection absent, events=0). Added detection sources to hg_alexa_bridge target. Suite 275/2071; Node 76/76; ctest 100% | 2026-10-12 |
| Judge-ready validation (v11) | Re-ran full end-to-end demo + failure-path matrix (capture-off fail-closed, inconclusive/conflicting fusion, dedup, restart persistence, malformed requests, loopback) — all PASS; C++ 275/2074, ctest 100%, Node 76/76, ASan 275/2074 zero errors, fresh out-of-source build green; no P0/P1 defects; recording recovery steps added to submission package | 2026-10-12 |
| Privacy audit (v13) | Roadmap H-03: PrivacyAuditLog + verify_enforcement(); wired into ConsentGatedAcquisition + SimulatorRuntime (passive observer); demo `privacy_audit` block. 11 new tests; suite 286/2120; Node 76/76; ctest 100% | 2026-10-13 |
| Native HTTP demo (v14) | Roadmap H-04 + native transport: `hg_alexa_bridge --http` serves the embedded dashboard + JSON API in ONE C++ process (Node eliminated from the demo); foundation reviewed (4 defects fixed: Host/Origin/CSP/shutdown); integration defects fixed (simulator+broker reuse latch, idempotent `grant_consent`, guarded scenario) ; 18 `[nativehttp]` adversarial cases; suite 304/2217; ctest 100%; ASan clean. Footprint: idle RSS ~7 MB vs ~49 MB node, peak ~7.3 MB vs ~57 MB, 1 proc vs 2+, no Node dep | 2026-10-13 |
| Scenario latency investigation (v15) | Root-caused native `/api/run-scenario` latency to SQLite `synchronous=FULL` fsync per commit (~93 ms/commit on this host); NOT a native-design defect (legacy child fsyncs too, 28-220 ms each). 99% of latency in the persist phase; all other phases <1 ms. No safe optimization without weakening durability; `synchronous=NORMAL` option documented and left for approval. Added bounded default-off profiling (`HG_PROFILE=1`). Suite 304 cases / 2214 assertions; ctest 100%; `[nativehttp]` 18/97 | 2026-10-13 |
| Licensing + release readiness (v16) | Apache-2.0 LICENSE (sha256-verified vs apache.org) + NOTICE + README; hackathon P0-2 DONE. SQLite `synchronous=FULL` RETAINED (decision closed; verified WAL + synchronous=2 at runtime; no durability change). Roadmap reconciled: F-01/G-01 DONE in scope via the native transport + embedded dashboard; F-02/F-03/G-02/G-03 DEFERRED. v15 durability decision marked resolved. Release-readiness verified (build/tests/ASan/demo/loopback/persistence/capture-off/privacy/shutdown) | 2026-10-13 |
| Multi-scenario demo (v17) | 5 deterministic scenarios (normal / unusual / inconclusive / consent-denied / default) run through the REAL pipeline; persisted alerts are the pipeline output (the old hardcoded placeholder is removed); scenario consent state is reset per run (order-independent); /api/run-scenario?scenario= allowlisted, /api/alerts + /api/scenarios added; dashboard rewritten with a scenario selector, backend-derived alert detail (severity/rule/message/explanation) and safe DOM (textContent). 320 C++ cases / 2,409 assertions; ASan clean; binary +1.8% (3.19 MB), idle RSS 6.9 MB, 1 process, 1 thread | 2026-10-13 |
| L-11 detection-driven alerts (v18) | Alerts are now CAUSED by detection/fusion evidence: the pipeline's default rule is DetectionFusionRule (fusion of the detector observations carried by the pipeline); alerts only on a `detected` verdict above a confidence floor with >=2 usable votes, never on inconclusive/unavailable/not_detected/conflict. The earlier raw-observation `sim-presence` rule is removed. Detection runs only after an AUTHORIZED acquisition. Alert severity follows fused confidence (>=0.8 warning, else info); alert details carry fusion verdict/confidence/contributing/provenance/reason; API + dashboard surface them. Scenario results re-measured. 329 C++ cases / 2,453 assertions; [l11] 9/40; ASan clean | 2026-10-13 |
| L-12 anomaly detection in pipeline (v19) | New core AnomalyMonitor feeds the bounded activity feature of each AUTHORIZED acquisition to the statistical AnomalyDetector; results become detection Observations (detector_id activity-anomaly) that flow through the SAME pipeline. DetectionFusionRule gained an explicit evidence allowlist (default = activity detector only), so anomaly evidence is ADVISORY: reported/audited/persisted but not a vote unless explicitly allowed (both directions unit-tested). 4 new scenarios (anomaly-warmup/normal/deviation/missing). Measured: warm-up 2/4 no anomaly; normal mean 0.710 sd 0.00894 status normal; deviation status high, 2 anomalies, z=2.442; missing gap probe. L-11 results unchanged (unusual 10/1/9, default 10/2/8). 346 cases / 2648 assertions; [l12] 17; ASan clean; standalone and bridge agree | 2026-10-13 |
| H-01s simulated critical impact + severity prioritisation (v20) | New core `SyntheticImpactClassifier` (deterministic transient-then-stillness pattern: transient >= 0.80, stillness <= 0.25 within 4 samples; a lone transient stays `inconclusive`) and `SyntheticImpactRule` (a CONFIRMED pattern with >= 2 supporting observations maps to `Severity::critical`). `Pipeline` now evaluates every registered rule and returns the HIGHEST severity, so a critical impact alert is never lost behind an unrelated info/warning alert (single-rule behaviour unchanged). Fixed a latent lossy `Observation::from_event` (it kept only activity `energy`/`detect_threshold`; it now carries the bounded feature payload, so detector-specific features such as the impact `phase` reach the rules). New scenario `synthetic-critical-impact` (catalogue now 10). Measured: media 12, events 30, generated 23, delivered 2 (1 CRITICAL `synthetic-impact` + 1 INFO `detection-fusion`), suppressed 21, persisted 2; impact 6 samples / 1 transient / 1 confirmation; the anomaly layer stayed advisory (status `low`, 2 anomalies, `fused_into_alert` false). Regressions unchanged: unusual 10/1/9 warning, default 10/2/8, normal/inconclusive/consent-denied 0. 364 cases / 2811 assertions; `[h01s]` 18/147; Release bridge 3,320,856 B (+60,896 B, +1.87% vs L-12), idle RSS 6,432 kB, 1 process / 1 thread | 2026-10-13 |
| v21 final integration + stability verification (RC) | Fresh out-of-source Release build of all three shipped targets (0 errors). All 10 scenarios verified with asserted expectations (10/10); one critical-capable scenario unchanged. HTTP adversarial matrix 51/51, e2e/stability PASS (deterministic repeated runs, no state leak, RSS/fd flat), retention regression 8/8, fail-closed 7/7, browser smoke test with 0 JS errors, Node 76/76. Three verified defects FIXED with regression coverage: (1) an unsupported scenario id returned HTTP 200 -> now 400 with the same generic JSON error body (handler now owns the business status; transport validation unchanged, dashboard unaffected); (2) an unwritable working directory made the logger throw out of main -> SIGABRT (exit 134); the file sink is now best-effort (console-only fallback); (3) the bridge's own durable alert store never applied alert_retention_days -> unbounded growth; now pruned at startup and bounded-periodically, honouring the config (0 = keep forever). 369 C++ cases / 2841 assertions (was 364/2811); CTest 100%; ASan/UBSan clean; legacy Node 76/76. Release bridge 3,342,280 B (+21,424 B, +0.65%); 1 process / 1 thread; idle RSS 6,392 kB; peak after runs 7,224 kB (= RSS); idle CPU 0.00%; startup 3-4 ms; critical scenario 72-111 ms. WAL + synchronous=FULL retained. `docs/RELEASE_CANDIDATE_V21.md` -> GO (RC only; no merge, no publication). Follow-up finding fixed: 11 test fixtures used fixed CWD-relative DB names, so two suite instances in one working directory produced phantom "disk I/O error" failures (31/27 cases) — now process-unique, with `scripts/verify_suite_concurrency.sh` proving two concurrent Release AND ASan instances both pass with zero disk-I/O errors. A fifth defect was found and fixed while starting the demo: the dashboard was embedded at CONFIGURE time with no dependency on `src/web/index.html`, so a dashboard-only edit left a stale page inside an already-built binary (demo.sh served the previous UI). Generation is now a build-time custom command (`cmake/generate_web_assets.cmake`) with the HTML as a real dependency; verified by touching the HTML and running `make` alone, plus a fresh configure/build and the served page | 2026-10-13 |
| D-01 hub container (branch `feature/distributed-homeguardian`) | The owner changed the target architecture to a central C++20 hub in Docker plus real Android acquisition nodes; v21 remains the tested hackathon fallback. D-01 ships a multi-stage container (build → test → runtime): debian:trixie-slim in both stages because the project requires CMake >= 3.28.3 and builder/runtime glibc must match; no Node.js and no Python in the image; runs as uid 10001 with cap_drop ALL and no-new-privileges; no privileged mode, no device passthrough, no docker.sock; **no published ports** (the loopback-only HTTP server is deliberately not exposed) with an opt-in 127.0.0.1 host override; health check via /dev/tcp (no curl); persistent named volume at /data; credentials mounted read-only as a Compose secret (never in a layer); exec'd entrypoint for clean SIGTERM; mem/cpu/pids limits and log rotation; restart `unless-stopped`. `docker build --target test` runs the existing suite inside the container toolchain; `scripts/verify_docker_hub.sh` asserts health, real-core responses, persisted alerts, survival across container replacement, clean SIGTERM, isolation, WAL header in the volume, and reports footprint. **D-06 genuine Alexa: OFFICIAL ALEXA DOCKER RUNTIME BLOCKED** — no official Alexa/Alexa+ container image exists, AVS developer tools are withdrawn, the AVS Device SDK was archived 2024-01-16, and the Alexa+ Category SDK / MCP Toolkit are partner-only; the official skill route needs owner approval. Details and primary-source references: `docs/DISTRIBUTED_DEPLOYMENT.md` | 2026-10-13 |
| D-02 Android acquisition node (branch `feature/distributed-homeguardian`) | Real acquisition application on the existing no-Gradle APK pipeline (JDK + SDK 27 + NDK r26d, armeabi-v7a). New core: `CaptureAccounting` (bounded, authorized, timestamped payload contract; thread-safe; saturating counters; drop reasons) and `NodeIdentity` (opaque hashed node id + sanitised bounded labels) — the D-03 registration identifier. **AAudio backend completed**: it previously opened a stream but registered no data callback, so no PCM was ever read; it now builds a PCM_I16 stream at the configured rate/channels, registers data + error callbacks, authorizes EVERY buffer through the consent gate, accounts it, tracks negotiated rate/channels and counts xruns/disconnects. Camera1 bridge gained front/back selection, size (320x240/640x480/854x480) and FPS (5/10/15) selection, per-frame wall-clock timestamps and a local frame count, keeping the per-frame delivery gate. MainActivity is the node UI: identity + display name, consent grant/**withdraw**, camera and microphone controls, a live RECORDING banner, live counters + last refusal reason, runtime permission flows, and a non-capture self-test. Capture is off by default; starting needs an explicit tap + OS permission + consent record; leaving the activity stops and releases everything (no foreground service, no background capture — deliberately). Verified on the host: `[d02]` 18 cases / 212 assertions, full suite 387 cases / 3053 assertions (was 369/2841), ctest 100%, fresh out-of-source Release build of all 3 host targets with 0 errors (homeguardian 3,210,648 B, hg_alexa_bridge 3,342,280 B), ASan/UBSan 387/3053 with 0 sanitizer reports, APK built+signed+aligned with no invokedynamic in the dex, all 12 JNI entry points exported, `scripts/verify_android_node.sh` 18/18. Footprints: APK 794,159 B, native library 1,140,532 B. **D-02 PHYSICAL CAPTURE ACCEPTANCE: BLOCKED — NO DEVICE ATTACHED** (no frame/sample/battery numbers are reported, because none were observed) | 2026-10-13 |
| Skills validation and detection integration (branch `feature/skills-detection-integration`, D-02x) | Home Assistant tooling was reviewed separately; no runtime dependency was added. New pure-C++20 layer, no new dependencies: `BoundedFeatures` (bounded audio RMS/peak/ZCR + motion changed-fraction from permitted PCM/luma, refusing oversize with a reason, no ML, no media retention), `ActivityDetectors` (`ActivityChangeDetector` EMA baseline + sustained confirmation; `VectorDriftDetector` cosine drift with a baseline that only updates while normal), `DempsterShafer` (Dempster's rule over an 8-element power set with conflict mass K; suppresses rather than averages when K>0.9; frame = {routine, activity_change, environmental_anomaly} — a calamity hypothesis is unrepresentable), `PrecursorDetection` (result vocabulary limited to activity_change_detected / audio_activity_deviation / possible_animal_activity / environmental_correlation / insufficient_evidence), `EnvironmentalCorrelation` (authorized env samples + windowed correlation with reported lag; Category-B evidence at info only), `SecurityObservation` (device id, source type, acquisition + ingestion timestamps, real/synthetic provenance, confidence/quality, consent/authorization context, bounded validated payload; strict `from_json`; availability tracker that keeps a dead sensor distinguishable from a silent one; bounded cooldown de-duplication), `HomeAssistantAdapter` + `HttpClient` (read-only GET /api/states, disabled by default, explicit entity allowlist, token from a mounted 0600 file refused if group/other-readable, CR/LF header injection refused, 64 KiB response cap, https refused because no TLS is linked, no actuation path). Integration: new observations convert to the standard `Event` and flow through the EXISTING pipeline; new detector ids are ADVISORY by default so all historical scenarios behave identically; opt-in via the existing evidence allowlist. No new path can reach `Severity::critical`. THREE REAL DEFECTS found by the new tests and fixed: (1) `DempsterShafer::belief` summed SUPERsets (commonality) instead of subsets, reporting belief 1.0 on total ignorance — `plausibility()` added to show the interval honestly; (2) the environmental-precedence rule required the environmental hypothesis to top the DST fusion, which strong activity evidence prevents, so the rule could never fire; (3) the availability observation was built by copying a moved-from object (empty device id, empty consent context, defaulted provenance). Verification: `[precursor]` 6/200, `[sensors]` 7/487, `[ha]` 6/179, `[integration][skills]` 7/65; **full host suite 413 cases / 3,984 assertions** (was 387/3,053); ctest 100%; fresh out-of-source Release build of all three targets with 0 errors (homeguardian 3,210,648 B and hg_alexa_bridge 3,342,280 B unchanged — the new sources are registered where they are referenced, i.e. the test target, and will be linked into the hub when D-03/D-04 add the ingestion route); ASan/UBSan 413/3,984 with 0 sanitizer reports. NOT implemented, with reasons recorded: YAMNet/embeddings, YOLO, Farneback optical flow, MediaPipe pose, Frigate, MQTT, Home Assistant WebSocket events, actuator control, Alexa (still externally blocked), and any species identification. Full matrix, evidence grading and per-method records: `docs/SKILLS_INTEGRATION_VALIDATION.md` | 2026-10-14 |
| Skills integration, second increment: dashboard surface + section-J coverage (branch `feature/skills-detection-integration`, D-02x) | **Dashboard (section K):** new read-only `/api/integrations` route whose every value is DERIVED from real state - the Home Assistant block is built from `HomeAssistantConfig::problems()` (so the reason it is off comes from the adapter's own validation), the device count from a real `DeviceAvailabilityTracker`, and the label vocabulary from the detection layer's closed result set. Rendered as **Sensor & integration status** with the three provenance labels stated verbatim (LIVE SENSOR DATA / SYNTHETIC TEST DATA / HYPOTHESIS / UNVALIDATED CORRELATION) plus a per-label legend mapping each result to observation, correlation or hypothesis; every empty surface shows WHY it is empty, and the payload carries no calamity, identity or fall claim at all. **New verification:** `scripts/verify_integration_dashboard.sh` - 31 API assertions + 5 shell checks, 0 failed (counts are zero, every empty surface has a reason, the vocabulary is exactly the permitted set, the hypothesis is labelled a hypothesis, forbidden tokens are absent, existing routes still 200, and a query string is refused 404). **Verified defect fixed (Incident 33):** a malformed credential table aborted the bridge - `json.exception.type_error.306` escaped `main()` as SIGABRT/core dump on the DEFAULT credential path (the secure path already returned 3). The load is now guarded: bounded fail-closed message, exit 3, no permissive fallback, nothing served; the harness includes that adversarial case. **Missing-observation guard:** `PrecursorSuite::fusion(now)` degrades a stale finding to `insufficient_evidence` (a stale finding is not an update) after `stale_after` (default 300 s) instead of repeating it as current. **Section-J gap suite** `tests/core/test_skill_gaps.cpp` `[gaps]` (4 cases / 84 assertions): multiple devices reporting simultaneously (independent liveness, per-device dedup keys, three devices coexisting in one pipeline history, no path to critical), unauthorized source rejected and consent withdrawal stopping delivery with zero further requests and resuming only on re-authorization, upstream authentication failure (401 -> errors counted, zero observations, entity not marked alive, no token echoed), and resource bounds under sustained input (5000-sample activity stream, oversized vector refused, 500-device registry capped, 500-entry dedup capped, oversized feature payload refused). Also fixed by these tests: the multi-device case asserted a precondition it never established (Incident 35 - the default 90 s timeout versus the test's own 200/400 s arithmetic). Full host suite now **417 cases / 4,068 assertions** (`[precursor]` 6/200, `[sensors]` 7/487, `[ha]` 6/179, `[integration][skills]` 7/65, `[gaps]` 4/84). Documentation split into the four required files: `SKILL_VALIDATION_REPORT.md` (matrix, A/B/C grading, per-method records, proof-bearing inventory, correction ledger), `ANIMAL_ACTIVITY_DETECTION.md`, `HOME_SECURITY_SENSOR_INTEGRATION.md`, `HOME_ASSISTANT_INTEGRATION.md` | 2026-10-14 |
| D-03 secure device transport (branch `feature/secure-device-transport`) | A separate, mutually authenticated TLS ingestion boundary between device and hub, so the loopback-only demo server is untouched and unpublished. **Credential model:** a private CA (`tools/provision_ca.sh`) issues one certificate per device whose CN IS the device id; the hub requires BOTH that the chain verifies against the CA AND that the SHA-256 fingerprint equals the `credential_id` the registry binds to that device - two independent bindings, because either alone would let one phone act as another. Identity comes from the certificate, never the payload. **Protocol** (`src/net/DeviceProtocol.*`): versioned, 4-byte big-endian length + JSON, 4 KiB hard cap validated before allocation, enumerated kinds, and a closed reject vocabulary (identity_mismatch, device_unknown, device_revoked, device_unpaired, consent_missing, consent_withdrawn, capture_disabled, replayed_sequence, duplicate_observation, malformed_timestamp, stale_timestamp, invalid_value, unknown source/observation type, oversized, queue_full, bad_version, bad_frame). **Provenance is derived:** real requires authenticated peer AND paired AND recorded consent AND capture enabled AND live acquisition; a `real` claim the hub cannot substantiate is recorded synthetic and flagged, and the trust facts are read from the registry rather than from the caller. **Transport** (`src/net/TlsTransport.*`): OpenSSL, TLS 1.2 minimum, `SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT` against the private CA with the verify result checked explicitly, socket timeouts, no custom crypto, verification never disabled in any mode including tests. **Registry** (`src/net/DeviceRegistry.*`): bounded per-device record (id, name, paired, credential_id, revoked, type, capabilities, camera/mic, last heartbeat, last accepted sequence, last authorized observation timestamp, online, consent) with strict persistence that deliberately does NOT restore capture or liveness on restart; revocation is total and is re-read from a revocation file on every connection, so it takes effect on the next message instead of at the next restart. **Accept path** (`src/net/ObservationIngest.*`): every check independent and ordered, and the sequence watermark commits ONLY after all of them pass, so a refused message cannot consume a sequence number. **Ingress** (`src/net/DeviceIngressServer.*`, `src/main_ingress.cpp`): one protocol on the published port (no dashboard, no commands, no SQLite), bounded connection queue, bounded worker pool, idle timeouts, clean SIGTERM, alerts persisted through the existing `PersistenceAudit` (WAL + synchronous=FULL unchanged). **D-02x limitation closed:** `src/net/IngestDetectors.*` wires ActivityChangeDetector, the existing AnomalyMonitor, SensorDeduplicator and DeviceAvailabilityTracker per device into the SAME pipeline; VectorDriftDetector and the PrecursorSuite fusion are deliberately not wired to a single scalar. Derived ids stay advisory (not in the evidence allowlist) and no path can reach `Severity::critical`. **Android:** transport implemented in the APK (`ObservationTransport.java`: bounded 128-entry queue with drop-oldest accounting, one worker thread, bounded reconnect backoff, mutual TLS from a PKCS#12, no credential in any payload or log) - compile-verified only; no phone attached, so physical delivery is BLOCKED. **Verification:** host suite `[net]` 9 cases / 396 assertions; full host suite **426 cases / 4,464 assertions** (Debug, Release and ASan/UBSan all agree; D-02x was 413/3,984); ctest 100% on both Debug and Release; 0 sanitizer reports; the container test stage runs the full suite AND the real-TLS acceptance (`scripts/verify_device_transport.sh`: handshake, unknown device, wrong credential, revocation without restart, replay, future and ancient timestamps, unlisted source type, oversized frame, missing handshake, consent withdrawal, TLS-only port, clean SIGTERM, durable alert count across restart); D-01 container acceptance re-run at 26/26. **End-to-end (the decisive acceptance):** with both containers running, a mutually authenticated client delivered 8 observations over mutual TLS; the hub's own alert count, read through its dashboard API from inside the hub container, moved 0 -> 1; the served history carried a `detection-fusion` alert; the ingested evidence and the alert row were durable in the shared SQLite store (main file plus `-wal`); no critical severity was produced; the alert survived container replacement (1 -> 1). The same run refused an identity-mismatched device, a revoked device without a restart, and an unregistered device. Acceptance totals: D-03 TLS **21/21** (inside the container image build too), D-03 end-to-end detection **32/32**, D-01 hub container **26/26**. Docs: `docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md`, `docs/ANDROID_TRANSPORT_D03.md` | 2026-10-14 |

## Test Results

```
100% tests passed, 0 tests failed out of 1
All tests passed (1925 assertions in 235 test cases)
Focused: [audit] 11 / 50; [status] 3 / 13; [alertpolicy] 14 / 62; [clearing] 3 / 17; [retention] 7 / 34; [alexa] 14 / 70; [integrity] 7 / 39; [privacy] 16 / 66; [lifecycle] 17 / 107; [hardening] 5 / 13; [event2alert] 9 / 34; [runtime] 7 / 40; [sim] 24 / 915; [dual] 12 / 74; [e2e] 9 / 73 — all pass
ASan/UBSan full suite: passed (235 cases), no leaks, no sanitizer errors
Android cross-compile (arm64-v8a/API 34): passed (static archive, not runnable)
Android cross-compile (armeabi-v7a/API 27): passed (on-device self-test binary)
On-device non-capture self-test (real hardware): 17/17 PASS, exit 0
Application run (simulator mode): capture-off → no media/no alerts + final_state=Stopped; capture-on → authorized observations through Pipeline, 1 alert delivered + persisted + 8 dedup-suppressed, pipeline_history=10, bounded speaker, final_state=Stopped
Restart: same audit DB, 2nd process reports historical_alerts=1 (distinguishable from current-process alerts)
Alexa contract: 14 C++ contract tests + 12 Node adapter tests pass (mock clients, no deploy); LIVE integration NOT tested (needs deployed authenticated endpoint) — documented as pending
Local process-boundary integration: hg_alexa_bridge (NDJSON stdio over the REAL AlexaContract) driven by Node — 16 integration + 4 restart cases against the actual compiled binary; reproducible demo docs/demo_alexa_integration.sh; no network listener, no deploy
Simulated Alexa+ web experience (`demo.sh`): `bash demo.sh` builds the bridge and serves a loopback-only web UI that drives the REAL backend (consent gate → event pipeline → correlation → alert dedup → SQLite persistence); `run_scenario` bridge mode produces real alerts; 8 `web_demo` Node cases; `docs/DEMO_WEB.md`. Simulated bridge only — no Alexa device, no live Amazon identity, no physical sensor. (The native C++ Model Context Protocol server `hg_mcp_server` was subsequently added in PR #20).
Clean-clone reproducibility (hackathon P0): fixed the tests-target `../src/...` relative paths (now `${CMAKE_SOURCE_DIR}` absolute) so an out-of-source build from ANY directory configures and builds; fixed a test that read `config/homeguardian.json` relative to CWD (now resolves via `HG_SOURCE_DIR`). Verified: fresh build in `/tmp` → 255/2001 + ctest 100%, and the web demo ran end-to-end on the fresh binary
Security review v5: docs/SECURITY_READINESS_V5.md — fail-closed auth, read-only ops, no credential leak, no network listener, no shell exec, bounded lifetime all VERIFIED; HTTPS endpoint CONDITIONAL GO (blocked on real authn/TLS/rate-limit/secrets)
Release reproduction: fresh out-of-source build (existing dep cache) → 241/1960 + bridge, new tests included
```

## Verification Status (separated by layer)

- **Compile (Android arm64-v8a / API 34):** VERIFIED.
- **Compile (Android armeabi-v7a / API 27):** VERIFIED.
- **Mock integration (host fake backend):** VERIFIED.
- **Linux simulator (consent, lifecycle, delivery, bounded buffers, dual-device,
  shared audio broker, e2e):** VERIFIED — deterministic, host-only; NO physical
  sensor opened.
- **Documented build reproduction (fresh out-of-source dir):** VERIFIED.
- **On-device non-capture self-test (real hardware):** VERIFIED — consent gate,
  lifecycle, fail-closed guards, cleanup; no camera/mic opened.
- **APK harness (build/package only):** VERIFIED — debug-signed APK builds and
  packages; declares only CAMERA + RECORD_AUDIO; capture disabled by default;
  NOT installed, no permissions granted, no capture run.
- **Real V4L2/ALSA/PipeWire capture or OS permission semantics:** NOT VERIFIED —
  simulator-only; requires physical hardware or a root-loaded virtual device.
- **Physical hardware capture (camera/audio):** BLOCKED — awaits explicit
  operator approval before the first camera/microphone activation. NOT claimed.

## Current Work

Phase F2.2 — Android device backends. NDK r26d installed; NdkCameraDevice and
AAudioCaptureDevice cross-compile for arm64-v8a / API 34 and are fail-closed by
default. On-device capture, permission-revocation, and hardware-boundary consent
testing are BLOCKED: no device is attached (adb empty) and API 34 is an
unconfirmed assumption. Hardware-level consent enforcement is not claimed.

## Blockers

- Real camera/audio capture (F22-09) is BLOCKED pending: an APK harness with a
  proper Android runtime permission flow, and explicit approval before the first
  camera/microphone activation. The no-capture on-device self-test already runs
  and passes. Device is connected and authorized.
- The device's marketing model string ("S25_Ultra") does not match its actual
  hardware (MediaTek MT6580 "Ruby", 32-bit, ~1 GB RAM); build targets follow the
  real hardware, not the model string.

## Known Issues

- `Logger::initialize()` is not thread-safe on first call. Acceptable for single-threaded startup; must be fixed before multi-threaded use.
- `Application` singleton pattern prevents multiple instances. Acceptable for single-process design.
- Pipeline is synchronous only. No async processing yet — by design for Phase C.
- Consent records are household configuration only; they are not proof of legally valid consent and are not yet enforced by any media-processing component (none exists yet).

## Next Authorized Task

Phase E2 — Sensing and Inference (deferred): simulated event source, camera
interface, face/voice analysis, ONNX Runtime. No media capture is enabled by
default and none is implemented.

## Resource Budget

- CPU: 30% of 40 logical CPUs = 12 logical CPUs max
- Build parallelism: `-j4` (conservative)
- Memory: 15 GB total, monitor usage during builds

## Session Info

- **Remote:** `origin` → `https://github.com/methakon/homeguardian-ai.git`
- **Repository:** `homeguardian-ai`
- **Branch:** `main`
- **Phase A commit:** `312cb75`
- **Phase B commit:** `058c692`
- **Phase B fix commit:** `e9ebfa3`
- **Phase C commit:** `479dccf`
- **Friction log commit:** `90691ef`
- **Phase D commit:** `5712aac`
- **Phase E commit:** `a472968`
- **Phase F1 commit:** `90841e9`
- **Phase F2.1 commit:** `e81584e`
- **Phase F2.2 commit:** `50e43ea`
- **F2.2 hardening commit:** `94e6714`
- **F22-08 commit:** `2caf62e`

## D-03.1 — security and reliability hardening (complete)

Implemented and verified: server identity verification on both sides, split credential
storage (offline CA / runtime / device keys), delivery-time authorization with fail-closed
handling, durable restart-surviving replay protection with a resume point, Android
acknowledgment/queue/lifecycle correctness, and honest processing-failure reporting.

Executed: host suite 430 cases / 4,565 assertions; ctest 100 % (Debug and container
Release); TLS acceptance 21/21; identity matrix 9/9; CA isolation 14/14; end-to-end
detection 38/38 (including replay-after-restart and mid-session revocation); D-01 26/26;
suite concurrency PASS; retention 8/8; context-exclusion probe 6/6; footprint measured.

Still blocked: physical Android acceptance (no device), Alexa (Amazon), live Home
Assistant (needs TLS).

## D-03.2 — replay/processing consistency (complete)

Corrected the ordering defect the external audit found (a durable watermark that advanced
before processing, turning a sink failure into silent loss), made the documented durability
real (file fsync + directory fsync, failures reported), and made replay protection mandatory
for a live ingress with a loopback-only isolated-test opt-out.

Executed: host suite 432 cases / 4,642 assertions; ctest 100%; ASan/UBSan clean; TLS
acceptance 24/24 (incl. 3 new mandatory-replay cases); TLS identity 9/9; CA isolation 14/14;
end-to-end 38/38; D-01 26/26; suite concurrency PASS; retention 8/8. Fault injection at
every boundary: validation, begin, sink, complete, repeat, concurrency, unwritable store,
dead-process marker.

Still blocked: physical Android acceptance (no device); Alexa (Amazon); live Home Assistant
(needs TLS).

## H-01 — real hardware activation (H-01a partial, H-01b-e software complete, H-01f blocked)

Discovered: a real microphone (built-in ALC897 analog input via PipeWire 1.0.5, 48 kHz) and a
real speaker (default sink is the Bluetooth `YO-457BT`; built-in IEC958 also available). The
Android phone the milestone reports as connected is **NOT** visible: `lsusb` shows no phone
and `adb devices -l` is empty.

Implemented and built on `feature/h01-hardware-activation`: a real PipeWire-backed microphone
adapter (`PwAudioCaptureDevice`), a real speaker adapter with a bounded queue, volume limit,
cancel and a locally generated tone (`LocalSpeaker`), the explainable audio activity session
(`AudioActivitySession`), and the host-side permission-gated service (`hg_audio_bridge`) that
keeps audio hardware OUT of every container. No ALSA headers exist on this host and `sudo`
needs a password, so the adapters drive PipeWire's CLI rather than linking libasound.

Safety, verified: `hg_audio_bridge --start` without the confirmation flag refuses (exit 3,
nothing opened); the capture adapter throws unless capture was explicitly authorized; the
whole `[audio]` suite is silent and deaf (dry-run/refusal paths only). Nothing has yet opened
the physical microphone: the first capture and the first playback are the operator's actions.

Amazon: **BLOCKED — no verified supported runtime** (no Amazon image exists, AVS tooling is
not generally available, and the in-repo Alexa skill is a simulated web demo, not a runtime).

Docs: `docs/HARDWARE_ACTIVATION_H01.md`.

## H-02A local voice feedback - test results (feature/h02a-local-voice-feedback)

| suite | result |
|---|---|
| full C++ suite | **460 cases / 5,547 assertions**, all passing |
| `[voice]` (new) | **17 cases / 133 assertions**, all passing - no sound is played by any of them |
| `[audio]` (H-01) | 20 cases / 1,599 assertions, all passing |
| ctest | 100% |

Sustained capture (H-01b) remains PASS: 35 s, 1,605,632 samples, 489 windows, 0 dropped, clean stop.
Bluetooth output (H-01 tone): OPERATOR CONFIRMED audible.
Spoken feedback (H-02A): implemented and unit-tested; physical speech acceptance BLOCKED pending
`sudo apt-get install -y espeak-ng`. Numbers that need the engine (first/subsequent utterance
latency, synthesis memory) are recorded as NOT MEASURED rather than estimated.

## H-02B corrections - test results (feature/h02a-local-voice-feedback)

| suite | result |
|---|---|
| full C++ suite | **464 cases / 5,564 assertions**, all passing |
| `[voice]` | **21 cases / 151 assertions** (4 new regression tests; no test plays a sound) |
| ctest | 100% (70.81 s) |

Real speech: `engine_name` = espeak-ng, `completed_within_wait=true`, `playback_jobs=1`, rc 0,
3.79 s per sentence, 7.5 MB peak RSS, 2% CPU. Real microphone->detection->speech session:
`voice_spoke: "Unusual sound activity was detected. The source has not been identified."` from a
real sustained_increase finding. HomeGuardian's spoken sentence is NOT marked physically verified
until the operator confirms hearing it.

## H-03 Android Wi-Fi node - status

| item | state |
|---|---|
| APK | BUILT + SIGNED, 882,617 B (`hg_apk_build/homeguardian-harness.apk`) |
| Transport wiring | IMPLEMENTED + COMPILED (was dead code before this milestone) |
| Artifact verification | dex + .so inspected; 0 `invoke-custom` |
| USB/ADB | **BLOCKED - DEVICE NOT ENUMERATED** |
| Phone model/API/ABI | UNKNOWN (not assumed; earlier docs assumed API 27) |
| Install / permissions / capture / Wi-Fi / USB-unplug | **BLOCKED** (no device) |
| Live remote CCTV | NOT YET IMPLEMENTED |

No hardware figure is reported because no hardware was reachable. The exact operator steps are in
`docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md` section 3.

## H-03 physical device pass (2026-10-14)

| item | state |
|---|---|
| Device | MediaTek MT6580 clone (`0e8d:201c`), API 27, armeabi-v7a, ~962 MB RAM, ~12.1 GB storage |
| App | installed (v0.2.0, 883,891 B), launched clean, idle by design, permissions granted |
| Node identity | `hg-443a03b54537c21f` (reported by the app's own native layer) |
| Provisioning | CA + server cert (SAN contains IP 10.20.30.6) + unique device cert; credential_id matches the registry |
| Ingress | running as a least-privilege container, `tls=mutual(min 1.2)`, `registry_devices=1 paired=1`, publishing 10.20.30.6:8443 |
| Hub accepts this device | **VERIFIED**: `accepted=3 rejected=0` over mutual TLS; spoofed id refused `identity_mismatch`; watermark durable at `last_sequence=3` |
| Phone over Wi-Fi | **PENDING OPERATOR** (phone is locked; Import + Connect + consent + Start are on-device actions) |
| Camera/mic on hardware | **PENDING OPERATOR** |
| USB unplug acceptance | NOT STARTED (must follow Wi-Fi delivery) |
| Remote live CCTV | NOT YET IMPLEMENTED |

## H-03 JNI defect (fixed) - status

| item | state |
|---|---|
| UnsatisfiedLinkError | **FIXED** (extern "C" region; symbols now unmangled, verified from the ELF) |
| Build-time JNI gate | ADDED (`scripts/verify_jni_exports.sh`, run by the APK build): 16 declarations / 16 exports |
| Gate non-vacuity | PROVEN (fails on zero declarations and on a .so with no JNI entry points) |
| APK + reinstall | 884,425 B, armeabi-v7a only; `adb install -r` Success, app data preserved |
| Launch | alive, native library loaded, 0 UnsatisfiedLinkError / 0 FATAL / 0 SIGSEGV / 0 ANR |
| 30-second ticker proof | **BLOCKED - PHONE LOCKED** (`mDreamingLockscreen=true`, activity STOPPED, tick counter 0) |
| H-03 acceptance (import, connect, consent, camera/mic, Wi-Fi) | **PENDING OPERATOR UNLOCK** |

## H-03 JNI fix - historical lab audit on test device

| item | state |
|---|---|
| UnsatisfiedLinkError | **FIXED + VERIFIED at runtime** (tick runs; both new entry points resolve) |
| 30-second ticker proof | **PASS**: heartbeats #20/#40/#60/#80, 10.25 s apart (20 x 500 ms), across a background/foreground cycle |
| Faults during the run | UnsatisfiedLinkError 0, app FATAL 0, SIGSEGV 0, SIGABRT 0 |
| JNI export gate | 16 Java declarations / 16 unmangled exports; build fails on any mismatch; non-vacuity proven |
| APK | 884,530 B, armeabi-v7a only, `adb install -r` Success (app data preserved) |
| H-03 acceptance (import credentials, connect, consent, camera/mic, Wi-Fi delivery, USB unplug) | **PENDING OPERATOR** - not assumed to have passed |

## H-03 consent lifecycle (fixed)

| item | state |
|---|---|
| Earlier denial reason (device) | `ambiguous - multiple active grants for the same purpose` |
| Root cause | the JNI store APPENDED a granted record on every press; the gate refuses >1 active grant |
| Fix | `SingleActiveGrantConsentRepo`: <=1 active grant per (subject,purpose); idempotent grant; withdrawal removes every granted record |
| Gate | **unchanged** - a test still forces `ambiguous` when genuinely conflicting grants are supplied |
| Tests | `[consent_lifecycle]` 13 cases / 45 assertions; full suite **477 / 5,609**, all passing |
| Denial visibility | reason + explanation in the returned string, the HGNode log, and `nativeConsentState()` polled each tick |
| `wi-fi: unknown` | **diagnosed**: `getActiveNetworkInfo()` needs `ACCESS_NETWORK_STATE` (was undeclared) -> SecurityException. Declared now (verified in the APK) |
| APK | 928,459 B, armeabi-v7a only, JNI gate 17 declarations / 17 exports, `adb install -r` Success |
| Physical sensor counters | **PENDING OPERATOR** - grant consent (deliberately more than once), then Start camera / Start microphone |

## Ingress health check (H-03 follow-up) - corrected

| item | state |
|---|---|
| Defect | the ingress inherited the HUB's image-level probe (HTTP on 8080) and reported `unhealthy` while serving mutual TLS on 8443 |
| Fix | service-specific probe `hg_ingress_probe` + `deploy/docker/ingress-healthcheck.sh` + a compose `healthcheck` override; the image-level hub probe is unchanged |
| What it verifies | real TLS handshake; server cert chained to the private CA AND covering the expected name/address; mutual-TLS demand reported separately - **not** a TCP port check, and **not** device authentication (`device_auth=NOT_PERFORMED`) |
| Acceptance (container test stage) | `scripts/verify_ingress_healthcheck.sh` - all checks pass, including the negatives (wrong name, non-TLS listener, missing CA, missing configured CA) |
| Regressions | D-03 TLS acceptance 24/24, TLS identity 9/9, CA isolation 14/14 (1 skipped), full ctest green |
| Live deployment | `docker inspect` -> `status=running health=healthy` (uid 10001, 0 restarts); the health log carries the probe's real output; the listener the phone dials is up on 10.20.30.6:8443 |
| Operator prerequisites confirmed | the mounted credential tree must be readable by the container uid (`sudo chown 10001:10001`, else exit 78) and the data volumes writable by it |
| H-03 physical acquisition / standalone Wi-Fi | **STILL PENDING** - operator-gated, not claimed |

## H-04 planning refinements + H-03 acceptance readiness

| item | state |
|---|---|
| Mail gate | **functional preservation**, not byte-identical output: availability, TLS certificate identity, mailbox integrity, DNS, delivery, latency. Queue depth/counters/logs are expected to move (owner review, refinement 1) |
| SSH | **unchanged** for this migration; any restriction deferred to a separate approval, only after alternative admin/recovery access is verified (refinement 2) |
| x402 retirement | only after HomeGuardian connectivity is **validated** (not merely configured) AND the rollback has been **exercised** from the **off-VPS** backup (refinement 3) |
| Read-only pre-flight bundle | `scripts/oracle_preflight_readonly.sh` - no writes, no privilege escalation (0 escalation commands), redacted output; **validated by running it on a real Linux host**: 11 sections, rc=0, no secret-shaped text, privilege-needing commands degrade to SKIPPED |
| H-03 acceptance harness | `scripts/verify_h03_physical_acceptance.sh` - checks the seven physical items from real device + hub evidence; items it cannot observe report NOT OBSERVED and FAIL (never a vacuous pass); `--post-usb` uses hub-side watermark evidence only, because adb is gone after the unplug |
| Phone instrumentation | the app now logs its counters every ~10 s (`counters video=... frames_processed=... samples=... obs_emitted=...` and `counters transport queued=... sent=...`), because UI scraping is unusable on this device. APK 928,615 B, JNI gate 17/17, reinstalled - and its line is **not yet observed** because the phone is locked (the tick runs only while the activity is resumed) |
| H-03 physical acceptance | **PENDING OPERATOR** - no result claimed or simulated |
| Oracle VPS | **not contacted, not modified** |

## Camera1 / NDK backend-selection defect (FIXED)

| item | state |
|---|---|
| Reported failure | `camera: initialization failed (NdkCameraDevice: no camera available)` - the Camera1 path was unreachable behind a failing NDK enumeration |
| Device evidence | `dumpsys media.camera` reports **2 camera devices**; the NDK camera2 API enumerates **0** on this MT6580/API 27 unit |
| Fix | explicit backend selection (`camera1` / `camera2` / `auto`), a native lifecycle representation for the Java Camera1 bridge (`Camera1Device`, Android-free and host-tested), a Java-start failure that clears the native capturing state and the video flag, a per-kind accountant stop, and a Java-side reconciler that releases Camera1 whenever the native layer leaves capture |
| Preserved | consent gate + per-frame authorization, CAMERA permission checks, withdrawal, activity-pause stop, bounds checks, capture accounting, and the NDK/camera2 backend for devices that support it |
| Tests | `[camera1]` 9 new cases / 46 assertions (delta-proven: `[camera1]` tag 6 -> 15 cases); full suite **486 / 5,652** (was 477 / 5,609) |
| Build | APK 965,451 B, JNI gate 21 declarations / 21 exports, `install -r` Success, clean launch, `camera availability: legacy(Camera1)=2` |
| Physical camera acceptance | **PENDING OPERATOR** - requires consent + Start camera, then real preview callbacks and rising frame counters; not claimed, not simulated |
| NDK camera2 hardware status | preserved, **NOT hardware-verified** (incomplete capture-session implementation; this device cannot exercise it) |

## Frame processing: 518 authorized frames, 0 processed (FIXED)

| item | state |
|---|---|
| Measured geometry | **720x576 NV21** - 622,080 bytes/frame exactly (322,237,440 / 518); byte accounting CORRECT |
| Exact rejection | `pixels <= FeatureBounds::kMaxPixels` - 414,720 > 307,200 (640x480); `++frames_processed` lives inside that block, so every authorized frame was skipped silently |
| Fix | `PreviewSizePolicy` selects the closest SUPPORTED size inside the bounds; the bridge REFUSES when nothing is processable; the frame path uses one bounds predicate; skips are counted by reason, logged rate-limited, and exposed as `proc_skip_*` + `last_geo` in every counters line |
| Not done on purpose | silently downsampling an oversized frame (it would change what the motion descriptors mean) and relaxing the bounds |
| Tests | `[previewsize]` 5 cases / 43 assertions, incl. the measured geometry as a regression; full suite **491 / 5,698** |
| Build | APK 997,465 B, JNI gate 22/22, install Success |
| Physical acceptance | **PENDING OPERATOR** - next Start camera reports the negotiation; rising `frames_processed` + real motion observations required, not claimed |
| Video flag | `video=off` in the sampled ticks is consistent: capture had stopped, and the 518 frames are cumulative - not evidence of active capture |


## MCP server (HG-MCP-01)

| item | state |
|---|---|
| Protocol | MCP **2025-11-25** (Streamable HTTP); also accepts 2025-06-18 / 2025-03-26 clients and answers anything else with 2025-11-25 |
| Tools | `get_homeguardian_status`, `get_alert_summary` - both read-only, both delegated to the existing `AlexaContract` |
| Transport | `POST /mcp` (JSON or SSE), `GET /mcp` -> 405 + Allow, `DELETE /mcp` -> session termination; sessions minted on initialize, idle-expired, capped |
| Security | bearer auth via `AlexaCredentialStore` (0600, owner-only, fail-closed, no fallback); Origin allowlist -> 403; loopback bind with an explicit opt-in required for anything else; header/body caps, timeouts, bounded workers and in-flight cap |
| Refusals | no credential store -> exit 3; world-readable credential file -> exit 3; non-loopback bind without opt-in -> exit 4; no unauthenticated mode exists |
| Tests | `[mcp]` **19 cases / 89 assertions**; full suite **510 cases / 5,787 assertions** (baseline before this work: 491 / 5,698) |
| End-to-end | `scripts/verify_mcp.sh` -> **43 passed / 0 failed** against the real binary over loopback |
| Not implemented | OAuth 2.1 client registration, TLS, server-initiated SSE stream, resources/prompts, per-token quotas, session resumability - see docs/MCP_SERVER.md section 7 |
| Unchanged | engine, web demo, NDJSON bridge, and every existing target; `main` untouched |


## Coordinator handover update — 2026-10-23 12:41 IST

### Alexa skill diagnostic checkpoint (owner-console evidence)

- Skill: `HomeGuardian AI(dhar)`, locale en-US, invocation `home guardian`.
- Owner reports both `open home guardian` and `ask home guardian for help` return Alexa's generic “There was a problem with the requested skill's response.”
- Alexa Code-tab screenshot shows `lambda_function.py`, `backend.py`, `requirements.txt`, and `utils.py`; the displayed handler file begins with `import backend` before the Alexa SDK imports. The owner supplied `utils.py` contents: an S3 presigned-URL helper using `boto3`, `S3_PERSISTENCE_REGION`, and `S3_PERSISTENCE_BUCKET`. No evidence yet shows that helper is invoked during skill startup.
- Owner reports CloudWatch Logs currently shows no logs. This is not proof of no invocation until the correct Lambda function and log group are identified.
- Alexa Build → Endpoint screenshot shows **AWS Lambda ARN** selected, with regional ARN fields populated. This conflicts with `alexa_hosted/DEPLOYMENT.md`, which documents the intended console path as Alexa-hosted Python. Do not change endpoint configuration blindly.
- Root cause remains **unconfirmed**. Candidate causes: endpoint ARN references the wrong/nonexistent Lambda; endpoint points at a runtime/code package different from the intended Alexa-hosted deployment; or a Python import/module initialization failure before handler execution.
- Backend adapter defaults `HG_GATEWAY_URL` to `http://127.0.0.1:8080`; that is unsuitable for reaching a separate C++ gateway from AWS Lambda, but cannot alone explain Help failing because Help is intended not to call the backend.
- Next diagnostic: read the full Default Region ARN from the Endpoint page and verify the exact Lambda function exists in `us-east-1`; if it exists, inspect its runtime and that function's own Monitor/CloudWatch logs. Do not add AWS permissions or change the ARN until verified. Do not share secrets or .env contents.
- Acceptance state: **Alexa simulator acceptance FAIL / blocked**; C++ backend live connection remains unverified; skill should remain private/unpublished until owner authorizes otherwise.

### Worker reliability audit update (provided report; not independently reproduced)

- The report records branch `fix/hg-worker-dispatch-p0-48` at `a303d895edcb0dc40ea57254891d863d32625eca`, PR #28 open/unmerged, and lane `experiment/hg-lan-mtls-07` at `139e53b45cdf`.
- Reported targeted tests: `test_poller.py`, `test_dispatch_p0_48_adversarial.py`, `test_p0_48_grant_atomicity.py`, `test_task_envelope.py` all exit 0 (16/16 reported). Queue-pull and idle-resume Kanban tasks reportedly completed; duplicate/unauthorized/prose/bare-JSON negatives reportedly rejected.
- Critical acceptance gap remains: GitHub-comment → poller → real-dispatch is **BLOCKED**, with task `HG-LAN-MTLS-07-006` blocked and dispatch attempt rc=1. The report attributes this to fetched comments lacking API provenance attachment `api_verified_comment(..., api_origin=True)`. Privileged dispatch must remain disabled until this path is fixed and independently verified.
- Native service report at main HEAD `503a9a6`: a missing/unavailable alert-store path explains a generic tool refusal in one fixture; a present store returned read-only status and zero alerts. Portal device listing remains unsupported by current two-tool MCP contract and would require a new authorized read-only tool. Google sign-in remains blocked pending owner-created OAuth Web client ID. Treat these as reported evidence unless independently rechecked.

These notes preserve the distinction between owner-console observations, worker-reported results, and independently verified repository facts.

## Workspace consolidation and handover discipline — 23 Oct 2026

**Coordinator directive:** workers must use the existing canonical HomeGuardian AI repository/project root as their only working directory. Do not create extra project copies, sibling clones, worktrees, scratch project roots, or new top-level/subfolders. Keep temporary artifacts outside the project root and remove them after use. Work only in the existing, architecturally correct source/test/documentation paths; a genuinely necessary new source directory requires coordinator approval first.

**Branch discipline:** continue on the single task branch already assigned to the issue/PR. Do not create experimental branches or split one task across multiple branches; ask before branching if no task branch is assigned. Keep changes scoped, commit/push the branch, and report branch, exact SHA, PR, tests/evidence, blockers, and next action. Security-sensitive work stays unmerged and privileged dispatch stays disabled until independent acceptance.

**Single living handover:** update this existing `docs/STATUS.md` after each meaningful milestone and before handoff. Record only verified state, distinguish worker-reported evidence from independently reproduced results, and include changed files, exact commit SHA, test commands/results, blockers, owner, and next action. Do not create duplicate handover files.

**Google Docs limitation:** Google Drive access was unavailable at this checkpoint; the external handover was not edited or verified. This repository file is the interim canonical handover. Once access is restored, maintain the existing Google Doc rather than creating a replacement, and keep this repository status synchronized.

**Worker instructions posted:** 23 Oct 2026, coordinator comments added to issues #23 (comment ID 6360358111), #24 (6360358687), and #25 (6360359414). These comments communicate workspace/branch/handover discipline and task-specific guardrails. Posting instructions does not prove that a worker has started or completed work.


## Alexa implementation handover — coordinator checkpoint, 23 Oct 2026 14:21 IST

### Remote repository evidence

- Verified via GitHub API that commit `fb41c9ca718d96cc57f88abb34820a2cbe02161a` exists on the repository history; its change records the approved temporary `.worktrees/alexa-main` checkout and the still-blocked console/AWS endpoint verification. This commit is the reported current `origin/main` SHA from the worker; confirm the branch head again before any subsequent write.
- Canonical Alexa source remains `alexa_skill_python/`; `alexa_skill/` is the separate Node.js mock prototype. The Python entry point is `lambda_function.lambda_handler`. The model locale is `en-US`, invocation name `home guardian`.
- Worker reports two supplied-source defects were reproduced with real SDK objects: (1) not invoking the Lambda handler factory; (2) serializing `outputSpeech` as a string instead of an Alexa speech object. These are source-level findings, not proof that either version is the one deployed in the console.
- Worker reports Python 39/39 passing under two dependency combinations, Node regression 77/77 passing using existing root native binaries, plus syntax and dependency checks. These test totals are worker-reported in this checkpoint and have not been rerun independently here. The commit and file hashes can be inspected through GitHub, but GitHub metadata alone does not verify local test execution.
- The worker reports authenticated MCP session initialization before the two allowlisted read-only tools, safe response rendering/SSML escaping, malformed-envelope handling and bounded transport behavior. Actual remote Alexa-to-C++ connectivity is explicitly **not established**.

### Deployment gate remains open

- Owner/worker still need authorized access to the existing Alexa Developer Console and AWS target to verify the Skill ID, selected regional Lambda ARN, actual function/runtime/handler, deployed bytes and matching CloudWatch log group.
- No duplicate skill is to be created. Do not change endpoint/hosting mode or add permissions until the current target is identified. Do not claim simulator acceptance or live backend connectivity without direct evidence.
- The temporary checkout under `.worktrees/alexa-main` is recorded as owner-approved for preserving unrelated root work. It is an existing exception, not permission to create any more project folders, copies or worktrees. Use the canonical project/repository root and existing paths for subsequent work.

### Next action / owner

- **Alexa worker + owner:** provide the selected regional Lambda ARN and confirm the Skill ID from the existing console; inspect the exact target and its logs. Keep the skill private until the endpoint, simulator behavior and approved backend route are verified.

---

## Network dispatch P0 and provenance — historical checkpoint, 2026-10-23

**Scope:** Network dispatch. **Issues:** #23, #14.

**Current verified state**

- Branch `fix/hg-worker-dispatch-p0-48`, SHA `f44d1a6b878d471f271325ea18daff73ff312065`, pushed.
  `origin/main` was merged in at **owner instruction** (`8458cd8`, 285 commits, no conflicts); rollback
  point preserved: `8d74f5377dac`. `main` itself was NOT modified.
- PR #28 (base `dev/production-integration`) — **open, not merged.** After the merge it reports
  **`mergeable=false`** and its diff widened to 54 commits / 66 files. Both are direct consequences of the
  merge and are flagged for owner decision, not silently accepted.
- Privileged dispatch remains **DISABLED**. No independent acceptance has been performed.

**Changed files (my lane only)**

`scripts/worker/task_guard.py`, `scripts/worker/task_envelope.py`,
`scripts/worker/test_dispatch_p0_48_adversarial.py`, `scripts/worker/test_p0_48_grant_atomicity.py`,
`scripts/worker/test_task_envelope.py`. Verified after the merge that the five repairs are still present by
content **and** behaviour, and that `git diff 8d74f53 f44d1a6 -- scripts/worker/` is empty.

**Repairs and evidence**

| # | repair | state |
|---|---|---|
| 1 | protected-op fallbacks never derive trusted context; refuse `AUTHENTICATION_REQUIRED` | done |
| 2 | envelope never trusts caller-supplied author; provenance minted out-of-band with a per-process nonce | done |
| 3 | scope confinement canonicalised (path traversal) | done |
| 4 | absent/empty approvals deny against the trusted canonical operation | done |
| 5 | SQLite owner grant consumed atomically once | done (was already present; demonstrated) |

**Tests (nonexecuting; no network, no dispatcher, no production DB)**

    test_dispatch_p0_48_adversarial.py  exit 0  item 3 9/9 + pre-fix control; repairs 1+4; repair 2
    test_p0_48_grant_atomicity.py       exit 0  first consume accepted; second APPROVAL_REPLAYED; expired APPROVAL_EXPIRED
    test_task_envelope.py               exit 0  16/16, verified idempotent across three consecutive runs
    test_poller.py                      exit 0  duplicates, wrong-worker, unauthorized author, cron PATH

Negative controls include **forged in-band provenance** (refused — the nonce defeats it), unauthenticated
caller JSON (refused), missing approvals (denied), and a pre-fix predicate control proving the scope suite
cannot pass vacuously.

**Blockers**

1. GitHub-comment → poller → **spawned worker turn** is unproven; completing it requires enabling
   privileged dispatch, which is forbidden. Proven instead: fetch → mint → admit → approval gate.
2. **Concurrent** (not merely sequential) duplicate grant consumption is not yet tested.
3. Workspace conflicts were reported: the earlier isolated checkout conflicts with the later
   single-workspace rule, and a second branch `experiment/hg-lan-mtls-07` remains.
4. The external local poller lives outside the repository, so its provenance-wiring change
   is not version-controlled here.

**Next action** — owner/coordinator to choose the PR #28 path (keep widened, retarget to `main`, reset to
`8d74f53` with the rollback point, or re-cut a narrow branch); then add the concurrent-consumption test and
resolve the workspace/branch compliance items.

---

## Network tooling source checkpoint — 2026-10-23 (late)

Historical source-branch report, before the follow-up merge into main:

- **`origin/main` merged into this branch**, then **all P0 work landed in main independently** — the entire
  `fix/hg-worker-dispatch-p0-48` history before the import commit is now an **ancestor of `origin/main`**.
  Verified: `scripts/worker/task_guard.py`, `scripts/overlay/docker_boundary_check.py`,
  `scripts/overlay/wg_preflight.sh`, `scripts/mtls_fixtures/vocabulary.py`, `scripts/worker/test_poller.py`
  are on main and byte-identical to this branch's head.
- **Consequence: PR #28 is superseded.** Its base `dev/production-integration` is at `8f40870` while main is
  ~55 commits ahead. It should be **closed, not merged** (owner decision, not taken here).
- **Import commit `a696a6e`** adds 13 uncovered artifacts found by a repo-wide read-only audit (method and
  full ranking: issue #23 comment `6371756146`): fusion replay + test + doc (21/21 pass, stdlib-only, 0.46 s),
  `scripts/ci/*` resource-safety preflight + rebuild-avoidance proposal, `docs/review-reports/*` (the missing
  `3475515` NO-GO record), `docs/SECURITY.md`, media-policy tooling and an Android session-wire patch.
  **Imported byte-identical, not reauthored** — provenance is in the commit body.
- **PR #31 open against `main`** (1 commit, 13 files). Not merged. It will need a rebase as main advances.

**Reported source state:** branch = `main + 1 commit`; worktree clean; four P0 suites exit 0; privileged
dispatch **DISABLED**; no main write; no merge; Alexa files untouched.

**Open for the owner (not self-resolved):**

1. Rebase this branch and land PR #31, then **close PR #28 as superseded**.
2. **Workspace/branch policy conflict** (raised earlier, still unresolved): the single-fixed-workspace rule
   forbids worktrees, while an earlier directive mandated a separate checkout.
   The canonical root remains checked out on the camera workstream's branch.
3. Test gap: **concurrent** (not sequential) duplicate grant consumption remains untested.

**Other worktrees reported as holding uncommitted work at that checkpoint**: `vpn-phone-bridge`
(`EdgeAnalyzer.{h,cpp}` + tests), `planp/backup plan` (`LiveCameraFeed.h` + `/api/camera/*` — the camera
portal exists only uncommitted), `private-mcp-malformed-messages` (coordinator crash hardening, audit F01),
`qa-sec-audit` (independent HG-SEC-01 gate).

## Worker tooling follow-up integration

Merged source tip `1cdc6fbdd24da3a348c4cbd66960ebd1d6bee46b` into the existing
main checkout, without conflicts. The source checkpoint above is historical;
its pending-import statements are superseded by this record. PR administration
and other worktrees were not changed.

Combined-tree verification: offline feature replay **21 tests passed**, worker
guard **12/12**, envelope **16/16 on two consecutive runs**, adversarial scope/
provenance checks and sequential grant consumption/replay/expiry all exited 0.
Three imported shell scripts passed `bash -n`. Tests used sandbox HOME and
temporary state; no live dispatcher, credentials, cameras or cloud jobs ran.
The replay remains offline/advisory, with `alert_eligible=false`; it does not
replace or rewire the C++ core. No native runtime or existing worker/security
implementation was changed, so the completed full pre-final suite was not repeated.

Imported certification tooling is **source-only, not operational acceptance**:
the remote notebook is absent, result validation is incomplete, and the preflight
classifier omits some runtime paths. See `scripts/ci/README.md`. Media tooling
was syntax-checked only; its checksum command must not be used as proof of consent
or authorization. No historical patch was applied. Truly concurrent grant
consumption and external-profile poller integration remain unverified.

## Published Google portal and physical-camera acceptance

The HTTPS portal now serves the existing dashboard through the cookieless Google
OIDC gateway. The dedicated client is configured server-side. The owner confirmed
successful real login after popup-handoff recovery. Household reads require an
explicit protected, exact-subject grant; the owner's selected verified identity
is granted read-only access, not every Google user. Other identities and mutations
remain denied. Source milestones: `5ca476f` and `57b3a6c`.

Focused verification at `57b3a6c`: OAuth/grants/bridge 258 assertions passed;
MCP client 26 passed; dashboard/security 161 passed. Deployed gateway digest
matched source. A server-local session preflight using the deployed gateway module
and service UID returned owner-authorized `/api/me` 200, native status 200,
alerts 200 and devices 200. This was not another real Google login: its session
was created locally for authorization/backend verification and revoked afterward.
The genuine backend reported zero alerts/devices and camera `configured:false`;
these are not evidence of live monitoring.

Physical camera discovery found the previously identified second camera at a new
DHCP lease, confirmed against its historical NIC identity. Device addresses and
fingerprints remain outside this public report. Vendor discovery and its vendor
TCP listener respond; standard snapshot HTTP/RTSP/ONVIF listeners do not. Existing
JUAN HTTP and Bubble adapters do not implement this V380 encrypted-generation
transport. Historical protocol evidence already records an unsupported-generation
result; no real decoded frame or physical-camera alert has been accepted.

The existing Oracle-to-phone read-only MCP path was freshly queried without
changing its configuration: both native status and alert-summary tools returned
genuine data. Status reported `Running`, `capture_enabled:false`; alerts reported
zero. This verifies read connectivity, not capture, physical detection or a spoken
Echo response. Alexa developer-console UI acceptance remains blocked by the
available browser's Amazon sign-in state.

Workstation shutdown remains conditional on independently working live capture,
analysis and alert readback. Those criteria are not met; do not shut down or claim
the requested physical-monitoring acceptance from synthetic tests.

### Camera DHCP recovery hardening

The native snapshot bridge now supports opt-in, explicit-MAC relocation with
`--camera-scan auto --camera-mac <private identity> --camera-state-file <protected path>`.
It requires registry-backed `--camera-analysis`. Capture and fresh registry
authorization gate recovery probes and credential transmission; ordinary fetches
also verify the destination identity when recovery is enabled. Snapshot/server
fingerprint fallback was removed: a JPEG from an arbitrary host cannot authorize
credential redirection. Saved state is a bounded subnet/MAC/TTL-checked cooldown
hint, never a trusted destination. Failed scans persist cooldown; probe IO uses
absolute monotonic deadlines and bounded concurrency.

Focused parent verification: locator 1,827 assertions / 24 cases; native camera,
portal and HTTP 1,120 assertions / 77 cases; camera portal security 6 tests. All
passed; no physical camera was contacted by these synthetic/loopback tests.

Recovery is currently **request-driven, at most once per minute**, not a scheduled
background retry every minute. It applies to an authorized supported HTTP snapshot
source, not the proprietary V380 listener. Installing this bridge binary does not
configure the physical camera, implement its unsupported transport, or establish
workstation-independent capture. Do not equate these code tests with live feed
or physical-alert acceptance.

### Legacy codec boundary correction and camera-source clarification

The legacy V380 password encoder allocated two encrypted blocks for an exactly
16-byte password, then copied them into a field sized for one block plus its key.
It now uses exactly one zero-filled 16-byte block after the existing input-size
check. Empty, 15-byte and 16-byte inputs remain supported; 17-byte inputs fail
without producing a field. Focused rebuilt protocol tests passed 33,042 assertions
in 12 cases, including the 24-assertion boundary case. This is a memory-safety fix,
not encrypted-generation camera support or physical-feed acceptance.

The V380 finding must not be generalized to every owner camera. A separate
JUAN/EseeCloud source was found on the LAN: anonymous snapshot access was rejected,
the previously configured camera credential returned a real 24,321-byte JPEG,
ffmpeg decoded a 160x120 grayscale frame (19,200 bytes), and consecutive snapshot
hashes differed. Those measurements establish physical acquisition/decoding only.
The subsequent native-analysis experiment was stopped at the owner's direction
so the earlier device-specialist session can continue it; no persisted physical
alert, Alexa readback, production feed or independent capture is claimed.

### Physical observations and Android decoder follow-up

After the owner resumed implementation, a bounded run fetched 40 distinct JPEGs
from the supported physical JUAN camera. The unchanged native analysis path
produced 40 observations with `provenance:real`, zero decode failures and zero
durable-commit failures. No event qualified for an alert in this run; zero committed
alerts is not evidence of successful physical-alert delivery. Frames were requested
by a local verification harness, so this is not unattended-monitoring acceptance.

CameraAnalysis now includes an Android API30+ AImageDecoder implementation alongside
the unchanged Linux subprocess decoder. Android decoding stays in native C++ and
does not depend on a workstation ffmpeg executable. The three focused decoder
tests passed (error/cleanup shim, Linux JPEG regression, real NDK compile/link and
API29 rejection); existing camera analysis tests passed 146 assertions / 4 cases.
The extracted production decoder was also cross-compiled and executed on the Mi A3
against a real camera JPEG, producing 19,200 luma bytes with exit 0. That on-device
decoder probe does not establish deployed autonomous acquisition, a physical alert
or end-to-end Alexa readback. Integration into the existing foreground service and
its actual MCP alert store remains a separate acceptance step.

### Reviewed recovery follow-up from the camera session

Reviewed the camera session's same-address restart and DHCP-move changes in the
shared checkout before integration. The initial proposal failed to compile due to
a duplicate `camera_scan` declaration and did not distinguish the default `off`
from an explicit operator opt-out. The integrated version arms MAC-bound recovery
when a MAC is explicitly enrolled, while `--camera-scan off` still disables it.
Existing registry-backed analysis and capture/credential gates remain required.

The restart regression also exposed a status defect: an old successful frame
masked a later disconnect. Three consecutive fetch failures now take precedence
over cached-frame freshness, and a new verified frame clears the failure streak.
Synthetic loopback recovery tests cover same-address return, a MAC-matched address
move via the failure-recovery hook, and zero probes for an untouched healthy feed.

Parent verification passed: locator 1,856 assertions / 27 cases; native camera,
portal and HTTP 1,120 assertions / 77 cases; portal security 6 tests; three CLI
checks for implicit MAC enrollment, explicit off and explicit auto. This verifies
the recovery hooks, not a no-browser background scheduler or a physically changed
DHCP lease. The phone-local monitor integration remains separate.

### Android deployment checkpoint and preservation of the Alexa lane

The recovery integration above is published as `30a99b8`. A separate Android
foreground-service camera integration remains uncommitted work in progress; do
not treat that integration as included in the recovery commit.

The updated service APK compiled, signed, installed and started, but private
bridge restoration was refused by its route policy: the underlying Wi-Fi default
route was treated as a conflicting non-VPN route for the approved tunnel peer.
The UI also used the older interface-subnet gate, which rejected a /32 overlay
configuration. No firewall, tunnel keys, network routes or public listeners were
changed, and no route check was bypassed to force a connection.

Restored the previous installed APK with a signed version-code rollback wrapper,
retained its application data and restarted its existing saved private bridge.
Fresh reads from the VPS using the established mailbox worker's private MCP
client returned genuine native responses: status `Running`, `read_only: true`,
`capture_enabled: false`; summary `alert_count: 2`, `highest_severity: warning`,
`read_only: true`. The count alone does not establish the alerts' provenance or
physical-event-to-Alexa acceptance. Restored connectivity does not mean that the
new autonomous phone camera monitor is deployed: capture is currently disabled.

Android camera integration deployment is blocked pending a tested correction to
the actual socket/VPN route-selection contract. Physical camera restart, DHCP
movement, phone reboot and workstation-off acceptance remain unverified. The
existing Alexa lane is restored; a production-certified autonomous camera lane
is not claimed.

### Autonomous phone camera monitor: route correction and first live deployment

The route refusal above was a defect in the admission rule, not in the tunnel.
The rule treated *any* covering non-VPN route as proof of unprotected egress, so
the Wi-Fi default route vetoed a peer that the overlay carries with a /32 host
route. Admission now compares longest-prefix specificity: the VPN must be
strictly more specific than every competing non-VPN route, an equal or broader
VPN prefix is refused, and two equally specific VPNs are refused rather than
guessed. The socket binder no longer takes the first `TRANSPORT_VPN` network; it
re-runs admission against live platform state, binds the network that actually
routes the destination, re-checks that the network still exists immediately
before `Network.bindSocket`, and never falls back to another interface on
failure.

Measured device shape for the deployed phone: Wi-Fi `0.0.0.0/0` via `wlan0`
(network 100) plus `192.168.77.1/32` and `192.168.77.4/32` via `tun0`
(network 101). This shape is now admitted; the previous rule refused it.
Retained refusals, all asserted: public destinations the VPN does not route,
LAN hosts, equal-prefix VPN/LAN overlap, a more-specific LAN route, cloud
metadata and link-local, multicast, broadcast, unspecified, unwired or
permission-denied platform state, loopback-by-name (DNS could be repointed) and
an ambiguous VPN pair.

Off-device verification, run against the production class, not a copy:
`tests/portal/test_android_route_security_policy.py` compiles the real
`RouteSecurityPolicy` with the API 34 platform jar plus stubs, then executes
`tests/portal/android_route/RouteSecurityPolicyRegressionTest.java` — **57
checks, 0 failed**, including the measured device shape, every retained refusal
above, and API 30 adapter behaviours (route parsing, correct-VPN selection with
a decoy VPN enumerated first, no bind when the VPN vanished, refusal on EPERM).

Deployed: APK `versionCode 9` (`1.1.1`) built with the real `hg_mcp_server`
source set plus the camera analysis sources, signed, installed over `versionCode
8` and started on the Mi A3. The bridge dialled, authenticated and reported
`auth OK` with a live MCP session, which the refused build could not do. The
previous APK was pulled and verified (`sha256
1ec33b58d1600ac18009283f224a1b12dcb11177496a3408fc5584fc7d36de25`) as the
rollback before the update.

Live autonomous evidence from the deployed phone, with the app's Activity
closed (same process throughout, PID 13741):

```
before HOME : capture_enabled=true  observations_processed=377
after 171 s : capture_enabled=true  observations_processed=550   (+173, ~1.01/s)
camera      : state=analyzed  decode_failures=0  fetch_failures=1
              identity_gate=owner_approved_fixed_host_mac_no_rediscovery
alerts      : committed=0  total_alert_count=2  persistence_healthy=true
```

Frames came from the supported JUAN camera at its authorized fixed private host,
over the native `LiveCameraFeed` + `CameraAnalysis` path; no browser, relay or
workstation process participated. No alert was committed during that sample;
counts alone do not identify which qualification or suppression gate prevented
an alert. The two alerts already in the store predate this deployment. Their
provenance was not established by the aggregate readback, so neither a synthetic
nor a physical origin is asserted for those two records.

Worker-origin readback through the established private path (VPS mailbox
worker's own MCP client) returned genuine live data with capture on:
`get_homeguardian_status` → `{"state":"Running","read_only":true,
"capture_enabled":true}`; `get_alert_summary` → `{"alert_count":2,
"read_only":true,"highest_severity":"warning"}`.

Also true, and not to be over-read: the SQS request/reply round trip was not
re-exercised in this deployment. The dedicated worker credential is
least-privilege (send-only to the reply queue) and the workstation AWS session
is expired, so the reply queue could not be read here; that link stays at its
previous WORKER-VERIFIED state. Physical camera restart, a real DHCP address
move, phone reboot recovery and workstation-off operation remain unverified.

#### Deployment addendum: artifact identity and bridge self-recovery

The installed APK was pulled back off the device and hashed against the build
output: both are `sha256 6366ef7deb5a76e6f51eec80121d1edd9c34a8baf14226359d71a7b3eef4ecbd`,
and that APK was built from the committed source, so the running artifact is the
reviewed one rather than a look-alike.

One transient outage was observed and is recorded rather than smoothed over.
With the screen off, the outbound bridge session dropped; for a few minutes the
worker-origin private read returned `UNAVAILABLE` while the on-device engine kept
answering its own loopback probe and kept analysing frames. No operator action
was taken: the bridge re-established itself, and three consecutive worker-origin
reads then returned `{"state":"Running","read_only":true,"capture_enabled":true}`
with capture still on. This is evidence of self-recovery from a dropped control
session on an unchanged network. It is not evidence of recovery from a reboot, a
changed camera address or a changed phone network, which remain unverified.

#### First real-frame alert committed and readable through the Alexa path

While the deployed monitor was running unattended, it crossed the detector
baseline and committed one alert from real camera frames:

```
camera.alerts_committed : 1        (was 0 in the deployment sample above)
camera.observations_processed : 1,082
camera.decode_failures  : 0
camera.state            : analyzed
capture_enabled         : true
total_alert_count       : 3        (was 2)
```

The alert count is the store's own count, not a delivery counter. Read through
the established private path — the same mailbox worker client the Alexa skill
reaches — the summary moved with it:

```
get_alert_summary -> {"alert_count": 3, "read_only": true,
                      "highest_severity": "warning"}
get_homeguardian_status -> {"state": "Running", "read_only": true,
                            "capture_enabled": true}
```

So: a real LAN camera frame, analysed natively on the phone with no viewer and
no workstation process, produced a durable alert that is now returned by the
worker-origin read the Alexa skill uses. The observation's provenance is `real`
(the edge camera configuration is registered with `is_synthetic: false`), which
is why this is a physical-path result rather than the synthetic matrix.

What this still does not establish: that Alexa spoke the reply to the owner
(the SQS request/reply leg was not re-exercised, as noted above), that the
monitor recovers from a reboot, a camera address change or a phone network
change, or that the workstation being powered off changes anything — the
workstation was never part of this path, but proving that requires the owner's
shutdown, which was not performed.

#### Follow-up: sustained operation, engine restart and authorization negatives

Before the controlled restart, fresh worker-origin reads returned `Running`,
`capture_enabled: true` and 12 stored alerts. The service was still PID 13741.
The local native counter had reached 6,724 observations. The local RESTART
control restarted only the native engine, not Android or the network: the
counter reset to 4 and advanced to 18, decode failures stayed at zero, and all
12 stored alerts survived. This verifies native-engine restart and resumption;
it is not a phone-reboot test or remote-recovery acceptance.

Both missing-bearer and deliberately invalid-bearer requests to the actual
phone-local MCP endpoint returned HTTP 401. No credentials, grants or capture
consent were altered for those negative checks.

The remote read did not pass after restart. Source review found an independent
session defect: the bridge attaches its cached native session even to a fresh
`initialize`, while the native server returns HTTP 404 for an expired/unknown
session before dispatch. The bridge previously cleared that cache only on a particular error-message
substring, which the empty 404 response does not contain. The correction below
has now passed host regression testing; no passing remote restart result is
claimed yet.

A later transport check also found the phone's Wi-Fi enabled but not connected,
with no IPv4 address on `wlan0`. Only the VPN virtual network remained in the
platform view; the VPS had received no recent handshake from the phone. The
coordinator returned HTTP 503, so current unavailability cannot be attributed
solely to the session defect. Restarting only the saved outbound bridge did
not restore that underlying Wi-Fi link. No Wi-Fi/VPN configuration, firewall,
route, peer key or public listener was changed. Live remote recovery requires
the existing physical network link to return before it can be accepted.

#### Native MCP session recovery correction: tested and built, deployment held

Fresh initialization now omits any cached native session header. Only a
successful initialization adopts the new session header. Native HTTP 404,
401 and 403 invalidate the cached session; error status and JSON-RPC errors
are preserved instead of being presented as HTTP 200. Generated errors retain
the original request ID. Redirects are disabled; the fixed loopback destination,
bearer authentication and read-only allowlists remain enforced. No request is
automatically replayed.

`python3 tests/portal/test_android_mcp_session.py` compiles the actual production
relay against the Android platform jar, then drives that relay against a real
loopback HTTP peer with test credentials. Parent verification passed 13 checks:
stale-session initialization, subsequent use of the fresh session, HTTP
404/401/403/500 failures, explicit reinitialization after 404, redirect refusal,
error/request-ID preservation, malformed input, failed initialization, ordinary
tool text not modifying session state, mutating-tool denial and JSON typing.
The route-security regression also passed. These are host tests, not proof of
phone-to-VPS recovery.

The signed versionCode 10 (`1.1.2`) APK built successfully; SHA-256:
`4f48d5814de642faa15ddc93e6c52c2ba5f775e14132530ad62191f960b95a55`.
Deployment is held: the phone remains on versionCode 9, and fresh checks still
report Wi-Fi enabled but not connected. Installing during that outage would
not verify the repaired remote path. Restore the existing saved Wi-Fi link,
then deploy and repeat engine restart plus both worker-origin reads. Neither
phone reboot recovery nor changed-network recovery is accepted by this result.

### Outdoor-camera judge access and interactive synthetic simulations

The owner explicitly approved access for every Google-signed-in account, including
judges, and clarified that camera controls remain read-only but simulation
selection/execution must be interactive. The deployed gateway now enables this
with `HG_OUTDOOR_REVIEW_ACCESS=google_viewers`. Google verification and the VIEWER
role remain required. The review scope permits camera snapshots/status and
aggregate status/alerts; device inventory, microphone/private media, capture and
consent changes, uploads, deletion and administration remain restricted.

GET `/api/scenarios` lists the actual native catalogue. POST
`/api/run-scenario?scenario=<id>` executes the existing C++ scenario implementation
in a new private fixture using a separate trusted `hg_review_bridge` executable.
Each fixture has generated credentials and a temporary audit database, no HTTP
listener and no physical camera configuration. Timeout, output-size and global
single-child bounds apply. Synthetic results are labelled and never replace live
camera state or write into the live alert store. The gateway UI enables only the
scenario selector/run button in addition to read controls; it does not ask judges
for an operator secret.

Source milestone: `6c2efdf`. Parent verification: OAuth/UI 263 assertions; reviewer
policy 64 assertions, also repeated with real native scenario execution; dashboard
161 checks; MCP adapter 26 checks; native simulation integration 16 tests including
all ten catalogue entries and timeout/overflow/cleanup controls. No skipped native
integration was accepted.

Deployed to `hg-gateway.service` only. A separate synthetic runner was installed;
the live portal bridge binary/process, mail, tunnel, VPN and other VM services were
not changed. Production environment remains mode 0600. Source/module/executable
checksums were read back and matched their tested artifacts.

Deployed-code preflight ran under `www-data` with temporary synthetic reviewer
sessions, not real Google logins. Every catalogue entry ran successfully through
the gateway; normal/inconclusive/consent-denied produced zero alerts and unusual
produced one synthetic alert. Anonymous calls returned 401; device inventory and
capture/consent mutation remained denied; revocation returned subsequent calls to
401. Preflight sessions were revoked. Public HTTPS serves the updated interactive
UI and still denies anonymous camera/simulation API calls.

The gateway restart invalidated old in-memory sessions: reviewers must refresh the
original portal tab and sign in again. A fresh real-account login after this
cutover remains owner acceptance. Camera status currently returns
`{"configured":false}` from the actual private bridge: access policy is fixed,
but no live portal feed is asserted or fabricated. Simulations work independently
of that camera connection.
