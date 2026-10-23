# HomeGuardian AI(dhar): Python Alexa voice adapter

This is the version-controlled Python ASK implementation restored to `main`.
It is separate from `alexa_skill/`, the existing Node.js mock-data prototype.
The C++ application remains the analysis/security core; Python only renders
read-only backend results as Alexa responses.

## Recovery provenance

Recovered from `alexa_skill_python/` at
`7e4f0076726e69c956063ddd85231a20703c50b4`. That revision includes the real Python
SDK `.ask()` reprompt fix. Earlier implementation commit:
`63f4517e38aebcaded570c6523d1aceb5cb36c44`.

Another independent implementation exists at `alexa_hosted/` on history rooted at
`5edcaab00379f2a815e923cec4789693857a78ac`, later updated by
`7318c236cafc259c96ec092a255e1fc14676d2a1`. It is not copied wholesale: its response
stand-in assigned a string to the SDK's output-speech model, and its gateway
assumptions are not the authenticated two-tool MCP contract used here.

The reachable fetched repository history contains these implementations on other
branches, but no `alexa_skill_python/` or `alexa_hosted/` history on the inspected
`origin/main` baseline `c67e0db2f5c56ba1748fc82fa7b8ec400fb5bf7a`. This is a missing
integration on main, not evidence that Python code was deleted from main.

## Authority and deployment state

- This directory is the repository source of truth for the restored Python skill.
- `alexa_skill/` remains the unchanged Node.js simulator-first prototype, not the
  code assumed to be deployed in the owner's Python console.
- The console's exact deployed bytes and selected Lambda target have NOT been
  independently retrieved. Owner evidence recorded in `docs/STATUS.md` reports
  Python files in the Code tab and an AWS Lambda ARN endpoint selection.
- The current inspection reached an Amazon sign-in wall; no saved browser login
  was available. AWS STS reported an expired local session. No skill ID, function
  name, ARN or log group is guessed.
- Local tests, GitHub publication, Alexa deployment and console simulator
  acceptance are separate states. Restoring these files does not certify the
  current console endpoint or resolve an unobserved deployment failure.

## Files and intents

`lambda/lambda_function.py`: entry point `lambda_function.lambda_handler`.
`lambda/backend.py`: authenticated, loopback-only read adapter.
`lambda/requirements.txt`: tested SDK dependencies.
`interactionModels/custom/en-US.json`: English (US), invocation `home guardian`.
`test/`: local tests using the actual ASK SDK, not permissive response doubles.

Handlers cover Launch, HomeStatus, RecentAlerts/GetAlertSummary, Help, Stop/Cancel,
SessionEnded, unknown/fallback requests and GetRoutineStatus. Routine status is
explicitly unsupported by the two-tool interface, not invented. The model preserves
all original Node-prototype intent names. Unreachable or unauthenticated backends must never become
invented status, zero alerts, routines or camera analysis.

## Local checks

    python3 -m venv .venv
    .venv/bin/python -m pip install -r alexa_skill_python/lambda/requirements.txt
    .venv/bin/python -m unittest discover -s alexa_skill_python/test -v
    .venv/bin/python -m pip check

The recovered baseline passed 26 tests with the actual SDK. The current suite adds
malformed envelopes, full SDK serialization for every model intent, explicit
Stop/Cancel session closure, fallback/routine behavior, SSML escaping, invalid
counts, initialization/session teardown, invalid configuration, timeout and
oversized backend responses. Transport tests use explicitly synthetic fixtures;
they are not evidence of live phone connectivity.

The adapter authenticates initialize, tools/call and DELETE with the same bearer,
uses the returned MCP session, and attempts cleanup even after tool failure.
Responses are limited to 64 KiB; redirects and environment proxies are disabled
so a loopback URL cannot forward credentials elsewhere. Each HTTP phase has a
maximum one-second socket timeout, within a three-second phase-boundary budget.

See the existing `docs/STATUS.md` Alexa checkpoint for the exact reproduction of
the two owner-supplied source failures and current test/deployment evidence.

## Backend boundary

The adapter intentionally refuses non-loopback hosts. An AWS Lambda's loopback
is its own machine, not the Mi A3 or private coordinator. The working phone/VPS
VPN therefore does not make a hosted Alexa Lambda able to reach the backend.
There is no authorized Alexa-cloud-to-private-backend path established here.
Do not expose the gateway publicly, change routing, enroll peers, remove
application authentication or bypass consent to make a voice demo succeed.

Without an approved reachable backend, the correct voice response is a clear
unavailability message. A working Help/Launch response is still useful to
separate skill/SDK/schema failures from backend reachability.

## Approved outbound-only AWS mailbox (prepared, not live-accepted)

The owner approved a mailbox rather than an inbound public relay. The voice
adapter sends an allowlisted request to a dedicated SQS request queue; a bounded
VPS worker calls the existing private MCP coordinator and sends an aggregate-only
reply to a separate queue. It creates no listener, VPN peer, camera/audio stream,
public MCP endpoint or new route to the phone.

`lambda/mailbox_backend.py` is selected only by `HG_ALEXA_TRANSPORT=mailbox` in
the environment or the approved `.env` file. Without that selection,
the original loopback adapter remains selected. Invalid transport/configuration,
AWS authorization failures and unavailable/invalid native replies all return the
existing unavailable speech, never cached readings or fabricated zero alerts.

Standalone Lambda environment configuration uses these four values:

- `HG_ALEXA_MAILBOX_REGION`: the approved initial deployment uses `us-east-1`.
- `HG_ALEXA_REQUEST_QUEUE_URL`: the dedicated request queue URL.
- `HG_ALEXA_REPLY_QUEUE_URL`: the separate reply queue URL in the same account.
- `HG_ALEXA_MAILBOX_ROLE_ARN`: the narrowly scoped publisher role in that account.

The existing canonical project `.env` is the local operational source of truth.
Set `HG_ALEXA_ENV_FILE` to that file's path for local tools/deployment validation;
the reader loads only the six allowlisted Alexa settings, not unrelated credentials.
These are the four mailbox values above, `HG_ALEXA_TRANSPORT`, and
`ALEXA_LAMBDA_EXECUTION_ROLE_ARN` (the owner-supplied hosted principal for IAM setup).
Duplicate selected keys fail closed; simple quoting, export and comments are supported.
Environment values override file values. No interpolation or shell execution occurs.

For managed hosting without custom Lambda-environment access, the loader defaults
to `.env` beside the deployed Python modules. Deploy ONLY the required mailbox
settings plus `HG_ALEXA_TRANSPORT=mailbox` there, never the full workstation `.env`,
personal AWS credentials, MCP bearer or unrelated secrets. The cloud cannot read
the workstation's file. `.env` files are gitignored, and the previous generated
JSON configuration has been removed. `HG_ALEXA_TRANSPORT=loopback` explicitly
disables mailbox mode. Do not enable it before the worker/private path is accepted.

The publisher role must trust ONLY the existing skill's exact Lambda execution-role
ARN via `sts:AssumeRole`. The developer-console logs SSO role is not that execution
role. Do not trust an entire hosted AWS account to work around a missing ARN.
Publisher permissions are request-queue `sqs:SendMessage` and reply-queue
`sqs:ReceiveMessage`, `sqs:DeleteMessage`, `sqs:ChangeMessageVisibility`, scoped to
the two queue ARNs. The VPS worker has the opposite direction: consume/delete
requests and send replies. No broad AWS managed policy is needed. Personal/root
AWS credentials must NEVER be installed on the VPS or copied into Lambda code.

The created dedicated queues use managed encryption, TLS-required policies,
60-second retention and 16-KiB message caps. Request envelopes expire after five
seconds, contain only a random correlation ID/tool/time bounds, and cannot choose
an endpoint, token or arguments. Replies contain only state/capture/read-only or
alert-count/highest-severity/read-only aggregates. Raw media, device/alert IDs and
household identity are not transmitted. This is a single-household private-skill
adapter, NOT a multi-tenant authorization or account-linking implementation.

The consumer checks exact correlation, operation, expiry and schema. A shared
standard reply queue can time out under concurrent contention; mismatched replies
are returned visible and never deleted as this caller's data. The nominal
five-second budget is NOT a hard wall-clock guarantee: SDK calls, credential
providers and DNS can overrun a phase boundary. Simulator latency remains an
independent acceptance item.

`deploy/coordinator/alexa_mailbox_worker.py` has no listener and processes one
request at a time. Run it only with dedicated owner-only AWS credentials and the
existing protected private MCP token file. Its required environment is:

- The mailbox region and request/reply URLs above (no publisher role needed).
- `HG_ALEXA_WORKER_CREDENTIALS_FILE`: mode 0600 INI credentials, named profile
  `homeguardian-alexa-worker`; inherited root/default profiles are not used.
- `HG_PRIVATE_MCP_URL`: the operator-approved private coordinator `/mcp` URL.
- `HG_PRIVATE_MCP_APPROVED_IP`: exact approved private IP for non-loopback URLs;
  public/link-local/metadata destinations remain refused.
- `HG_PRIVATE_MCP_TOKEN_FILE`: existing protected coordinator application token.

The CLI is bounded to 300 seconds by default; `--duration-seconds` accepts 1..3600.
SIGTERM/SIGINT stop its polling loop. A persistent supervisor/resource-capped
deployment is separate from this bounded demonstration worker. Do not describe
the source-tested worker as a running service.

Readback currently proves queue configuration only. Native self-probe succeeds,
but the private bridge's HTTP-200 `{}` replies fail JSON-RPC validation. The
exact-principal publisher trust is now configured and read back; actual hosted
AssumeRole, bridge repair, worker identity/deployment, updated Code deployment
and the simulator-to-native round trip remain acceptance
gates. None may be inferred from local fixture tests or queue existence.

Rollback: set `HG_ALEXA_TRANSPORT=loopback` in the deployed `.env`/environment and
redeploy the existing skill; stop only this worker if started. Revoke only its
dedicated credentials if created. Remove the two dedicated queues only after
owner approval and confirming no pending work. Preserve VPN/MCP/mail configuration.

## Console deployment boundary

Do not change the skill's hosting mode or endpoint ARN based on directory names.
First identify the selected endpoint and exact deployment target using authorized
console/AWS access. For an existing Python Lambda, the runtime handler is
`lambda_function.lambda_handler`; package the contents of `lambda/` at the archive
root together with installed dependencies, not the parent directory. If the
verified target is Alexa-hosted Python, use its Code deployment workflow instead.
Import this directory's model only after comparing it with the existing console
model. Never create or publish another skill incidentally.

The reported console `utils.py` is an S3 presigned-URL template helper. This
implementation neither imports it nor uses S3. It is not a required file for
these read-only voice handlers; no bucket credentials or persistence permission
are introduced merely to reproduce a template filename.
