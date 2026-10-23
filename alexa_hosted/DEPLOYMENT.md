# HG-ALEXA-53 — deployment record

Skill: **HomeGuardian AI(dhar)** · locale **English (US)** · invocation **`home guardian`**

## Identifiers

| item | value | notes |
|---|---|---|
| Skill ID | `amzn1.ask.skill.812646e4-5937-456c-88b0-b90b23d6d0b2` | **not a credential** — an identifier, visible in the Amazon console and in skill URLs. Recorded here so the deployment is traceable; it is never used as a secret. |
| where it lives locally | `~/projects/homeguardian-ai/.env` as `ALEXA_SKILL_ID` | owner-provided, mode 0600 |
| format check | **VALID** | `amzn1.ask.skill.<uuid>`, UUIDv4, RFC-4122 variant, 52 chars |

The Skill ID is **not** referenced by any file that gets uploaded to the console: the Alexa-hosted Lambda,
the interaction model and the manifest all work without it. It is needed only for the ASK CLI and for this
record.

## Stage status

| stage | state | evidence |
|---|---|---|
| 1. source implementation | **COMPLETE** | branch `experiment/hg-panasonic-cpp-receiver-08` |
| 2. local unit + bridge tests | **PASS (worker-reported)** | `python3 -m unittest discover -s alexa_hosted/test` → 13 tests, OK, API cases against the real `build/hg_alexa_bridge` |
| 3. Amazon Skill ID association | **found locally; association PENDING** | the ID exists and is well-formed, but nothing here can confirm it belongs to this skill in `en-US` — that needs an authenticated Amazon session |
| 4. Alexa-hosted deployment | **NOT DEPLOYED** | nothing has been deployed; `ask` is not installed on this workstation |
| 5. Amazon simulator acceptance | **NOT RUN** | only the owner's console/account can invoke the deployed handler |
| 6. live C++ backend connection | **NOT VERIFIED** | no HTTPS origin exists; an AWS Lambda cannot reach `127.0.0.1:8080` |

Nothing in this repository is deployed, published or certified. Do not read stage 1-2 as "operational".

## Path A — console (recommended, needs no CLI)

1. Console → **Alexa Skills** → **HomeGuardian AI(dhar)** → Build
2. **Interaction Model → JSON Editor** → paste `alexa_hosted/skill-package/interactionModels/custom/en-US.json`
   → **Save Model** → **Build Model** (must report invocation `home guardian` and 9 intents)
3. **Endpoint** → **Alexa-hosted (Python)**
4. **Code** → put `lambda_function.py` and `backend.py` in place, ensure `ask-sdk-core` is in the requirements
   → **Deploy**  (this creates the hosted repo: stage 4)
5. **Test** → *"open home guardian"*, *"ask home guardian for the alert summary"*  (this is stage 5)
6. Keep the skill **private/unpublished** until the owner authorises submission

Files and digests (verify what you pasted):

| file | sha256 (first 16) |
|---|---|
| `alexa_hosted/lambda/lambda_function.py` | `7afaafbffbb42ded` |
| `alexa_hosted/lambda/backend.py` | `877826c35c65ae74` |
| `alexa_hosted/skill-package/interactionModels/custom/en-US.json` | `895f4dec87062e21` |

## Path B — ASK CLI (optional; needs a browser login the owner must perform)

`ask` is **not installed here**, so none of this has been executed. A `.ask/ask-resources.json` for this
skill takes this shape — **read the id from the environment, never hardcode it in a tracked file**:

```json
{
  "profiles": {
    "default": {
      "skillId": "<ALEXA_SKILL_ID from ~/projects/homeguardian-ai/.env>",
      "skillInfrastructure": {
        "type": "AlexaHosted",
        "userConfig": { "runtime": "python3.12", "handler": "lambda_function.lambda_handler" }
      }
    }
  }
}
```

```bash
npm install -g ask-cli          # not installed on this workstation
ask configure                   # owner-only: opens a browser login; no password is shared with the agent
ask deploy --target skill-metadata   # associates the local files with the Skill ID above
ask deploy                      # deploys; this is stage 4 and has NOT been run
ask dialog --locale en-US       # optional simulator from the CLI
```

## Blockers

1. **Stage 6, the only real one.** The Lambda runs in AWS and cannot reach a loopback gateway. The gateway
   must be published through **Cloudflare Tunnel** (or the owner's VPN) per
   `docs/ORACLE_VPS_DEPLOYMENT_PLAN.md`, and its origin supplied to the Lambda as `HG_GATEWAY_URL`.
   A public unauthenticated gateway is explicitly **not** an acceptable solution, and the private MCP
   endpoint must stay private.
2. **`.env` is not gitignored** in `~/projects/homeguardian-ai`, and that file holds pre-existing secrets
   alongside this id. Nothing has leaked (never tracked, never staged, no commit touches it), but one
   `git add -A` would stage it. One line in that repo's `.gitignore` closes it: `.env`. This branch adds the
   same rule repo-wide so it cannot recur.
3. Stage 3's association, and stages 4-6, all require the owner's Amazon session. No agent action can
   substitute for them.

## Next smallest owner action

**Path A steps 2 and 4** — paste the model, build it, paste the two Python files, Deploy. That moves stage 4
from NOT DEPLOYED to deployed, and step 5 is the moment this stops being unverified. Everything needed is
listed above; nothing else is blocking except stage 6, which depends on the HTTPS origin.
