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

Handlers cover Launch, HomeStatus, RecentAlerts/GetAlertSummary, Help, Stop/Cancel
and SessionEnded. Unreachable or unauthenticated backends must never become
invented status, zero alerts, routines or camera analysis.

## Local checks

    python3 -m venv .venv
    .venv/bin/python -m pip install -r alexa_skill_python/lambda/requirements.txt
    .venv/bin/python -m unittest discover -s alexa_skill_python/test -v
    .venv/bin/python -m pip check

The recovered baseline passed 26 tests with the actual SDK. This recovery
milestone is not a declaration that the legacy MCP transport is ready for live
integration; further request-envelope, backend/session and failure tests are
required before deployment.

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
