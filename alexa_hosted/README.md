# HomeGuardian AI(dhar) — Alexa-hosted Python skill

A **voice adapter**, not an application. The analysis stays in the existing C++ HomeGuardian engine; this
skill asks the gateway questions and reads the answers. It never invents a state, and it never predicts.

## Layout

```
alexa_hosted/
  lambda/lambda_function.py     handlers: Launch, Help, HomeStatus, AlertSummary, CameraAvailability,
                                AnimalActivity, RoutineStatus, EmergencyAdvisory, Stop/Cancel, SessionEnded,
                                catch-all, exception handler
  lambda/backend.py             the adapter: read-only HTTP, short timeout, no credentials on the wire,
                                redaction by construction, safe failure when the gateway is unreachable
  lambda/requirements.txt       ask-sdk-core
  skill-package/skill.json      manifest (private, no permissions requested)
  skill-package/interactionModels/custom/en-US.json   invocation "home guardian" + 9 intents
  test/test_alexa_skill.py      14 tests, stdlib unittest — the API cases use the REAL C++ gateway
```

## Run the tests

```bash
python3 -m unittest discover -s alexa_hosted/test -v
# -> Ran 14 tests ... OK (expected failures=1)
```

No pip install is needed for the suite: the SDK is stubbed for the handler wiring tests (labelled as such),
and every data test starts `build/hg_alexa_bridge --http` on loopback and reads what that server actually
returned. The one expected failure is a real gateway gap, recorded rather than hidden — see below.

## The gateway contract, as measured (not assumed)

| route | method | auth | observed response |
|---|---|---|---|
| `/api/health` | GET | none | `{"ok":true,"status":"up","simulated":true}` |
| `/api/alerts` | GET | none | `{"alerts":[],"ok":true,"total":0}` — a **verified** empty set |
| `/api/status` | GET | none accepted | `{"status":{"error":"unauthorized"...},"summary":{...}}` — **refused** |
| `/api/session` | POST | `x-hg-secret` header | 200 + a single-use CSRF (`x-hg-csrf`), 400/401/403 without it |
| `/api/run-scenario` | POST | `x-hg-secret` + `x-hg-csrf` | state change — **never called by this skill** |

**Known gap (in-suite expected failure).** The bridge's GET read routes hand the handler an *empty*
credential (`src/bridge/NativeHttp.h:431-437`, `invoke({"status", "", 5, false})`), so `/api/status` answers
`unauthorized` over HTTP while `/api/health` and `/api/alerts` work. Alexa-hosted code cannot use the
bridge's stdio mode, so **the capture-state read has no authenticated HTTP path today**. The adapter handles
this honestly: the capture state is reported as unestablished and never guessed, while the alert count — which
the gateway *did* verify — is still reported. Wiring a credential into the read routes is a runtime change to
the C++ engine, so it is proposed separately rather than smuggled into this adapter.

## Two distinctions the skill is built around

1. **A verified zero is not a refusal.** `/api/alerts` returning `{"alerts":[],"ok":true}` means "no recorded
   alerts"; `/api/status` returning an error means "not established". Reading the second as the first would be
   a false all-clear, so `backend.py` refuses any body carrying an error before it reads a collection.
2. **A count is not a cause.** The activity intent reports how many authorized observations exist. It does not
   say what caused them, does not infer species, and makes no claim that animal behaviour predicts
   earthquakes, fires or any other emergency. Unvalidated ideas are described as not established.

## Credentials and owner dependencies (Phase 4)

| # | item | exists? | actually necessary? | where to get it | how to provide it |
|---|---|---|---|---|---|
| 1 | Alexa Skill ID | **yes** — the owner created "HomeGuardian AI(dhar)" | yes | Developer Console → your skill → *Build* | paste into deployment; not a secret |
| 2 | Skill locale | chooses itself (`en-US` model shipped) | yes | console → *Interaction Model → Language* | select `English (US)` |
| 3 | Invocation name | in the shipped model (`home guardian`) | yes | console → *Invocation Name* | upload the model, or type it |
| 4 | Alexa-hosted Python config | **partial** — the skill is hosted-Python; the code is here | yes | console → *Code* tab | **paste `lambda_function.py` + `backend.py`**, or `ask deploy` |
| 5 | ASK CLI | **NO** (`ask` is not installed here) | **no** — the console editor path avoids it | `npm install -g ask-cli` | only if you prefer CLI deploys; needs your browser login |
| 6 | Amazon-hosted skill repo access | **not yet** | only for CLI deploys | console → *Code* → the hosted repo link | owner's developer account |
| 7 | Oracle VPS HTTPS endpoint | **NO** — nothing is deployed; no tunnel config exists | **yes for live use** | deploy the gateway behind Cloudflare Tunnel per `docs/ORACLE_VPS_DEPLOYMENT_PLAN.md` | provide as `HG_GATEWAY_URL` (an env var, never in code) |
| 8 | Gateway auth config | `/api/health` and `/api/alerts` need none; the state route wants `x-hg-secret` | no for read-only voice | generate a launch secret on the host | host-side env/secret only |
| 9 | Account linking | **not required** | no | — | the skill is read-only for one home and requests no personal info |
| 10 | AWS root / Amazon password | **never needed** | no | — | do not share these with anyone, including this agent |

The one genuinely missing piece for a live end-to-end demo is **(7)**: the Lambda runs in AWS, so it cannot
reach `127.0.0.1`. Until the gateway has an HTTPS origin, the hosted skill can only be tested with a local
harness.

## Amazon Developer Console — exact steps

1. Console → **Alexa Skills** → your skill **HomeGuardian AI(dhar)**.
2. **Build → Interaction Model → JSON Editor**: paste `alexa_hosted/skill-package/interactionModels/custom/en-US.json`
   and **Save Model** → **Build Model**. Expect no errors; `home guardian` is the invocation name.
3. **Build → Endpoint**: choose **Alexa-hosted (Python)**. (If you prefer a self-hosted endpoint later,
   the HTTPS URL would be your Cloudflare Tunnel origin and account linking is still not needed.)
4. **Build → Code**: paste `lambda_function.py` and `backend.py` from `alexa_hosted/lambda/`, add
   `ask-sdk-core` via the console's *requirements* if the template did not already include it, then **Deploy**.
5. **Test** tab: use the utterances in `test/test_alexa_skill.py`'s model assertions, e.g.
   *"open home guardian"*, *"ask home guardian for the home status"*, *"are there any unusual animal activities"*.
6. **Distribution**: keep it private/unpublished until the owner authorises submission. Nothing here has been
   submitted or certified.

## Devpost / demo material

* **Voice demo script**: "Alexa, open Home Guardian" → *ready* → "ask Home Guardian for the alert summary" →
  the verified count → "are the cameras available" → the honest "the service does not report camera
  availability over this interface" → "emergency advisory" → a status, then the explicit statement that
  HomeGuardian does not predict emergencies.
* **Simulator instructions**: the console *Test* tab, or `ask dialog --locale en-US` with the ASK CLI once
  installed; every tested utterance is in the suite.
* **Architecture**: Alexa → (hosted Python adapter) → HTTPS → HomeGuardian C++ gateway (analysis, alerts,
  SQLite) → read-only MCP projection. The C++ side performs all analysis.
* **Integration evidence**: `python3 -m unittest discover -s alexa_hosted/test -v` — 14 tests, one recorded
  expected failure (the `/api/status` read gap). The API cases run against the real C++ bridge binary.
* **Needed from the owner**: the HTTPS gateway origin for a live run; a simulator screenshot; the skill ID for
  the deployment record.
* **Known limitations**: no camera media by design (no authenticated camera interface exists — see
  `docs/HG_V380_09_FEASIBILITY_AND_ALEXA_READINESS.md`); capture state unreadable over HTTP until the read
  route accepts a credential; ASK CLI not installed; no public endpoint yet; no disaster prediction, ever.
* **Not claimed**: certification, publication, a deployed endpoint, or any capability this file does not
  demonstrate.
