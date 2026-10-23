# HomeGuardian AI — Alexa custom skill (Python, Alexa-hosted)

A drop-in Alexa-hosted (Python) skill for HomeGuardian AI. It answers five things and refuses to
invent a sixth.

## What it does

| you say | intent | behaviour |
|---|---|---|
| "Alexa, open home guardian" | `LaunchRequest` | greeting, then the same status read as HomeStatus |
| "what's the status of my home" | `HomeStatusIntent` | `get_homeguardian_status` |
| "are there any alerts" | `RecentAlertsIntent` | `get_alert_summary` |
| "alert summary" | `GetAlertSummaryIntent` | same handler (kept for model compatibility) |
| "help" | `AMAZON.HelpIntent` | what it can do |
| "stop" / "cancel" | `AMAZON.StopIntent` / `AMAZON.CancelIntent` | "Goodbye." |

When the hub cannot be reached or is not authenticated:

> **"HomeGuardian is currently unavailable. I could not reach your hub, so I don't have any home
> status to report."**

**That is the point of the skill.** It does not substitute a zero, a remembered value or a guess. A
voice interface that improvises a household reading is indistinguishable, to the person listening, from
one that read it from the hub.

## What is NOT claimed

- **No live remote access.** The hub's MCP listener is loopback-only (`127.0.0.1:8790`) and the VPN
  coordinator is **not confirmed deployed**. `backend.py` therefore **refuses a non-loopback host**
  rather than quietly accepting one.
- **No household sensor data is fabricated.** If the hub returns nothing usable, the skill says so.
- **No credentials are read, echoed, logged or spoken.** The token comes from the environment; errors
  carry the exception *class* only, because a transport message can contain a URL or a header.
- **Console deployment is not claimed.** See below — that step is yours.

## Files

```
lambda/lambda_function.py               the skill handler (paste into the Code tab)
lambda/backend.py                       MCP client; loopback-only, fails closed
lambda/requirements.txt                 what it was tested against
interactionModels/custom/en-US.json     the model (paste into the Build tab)
test/test_skill.py                      tests
```

## Deploying it yourself (Build tab + Code tab)

The owner creates the skill; nothing here touches the Amazon console on its own.

1. **Create the skill.** Amazon Developer Console → *Create Skill* → name `HomeGuardian` →
   **English (US)** → **Alexa-hosted (Python)** → **Start from Scratch**. (Do not pick "Provision your
   own" unless you intend to host the Lambda yourself.)
2. **Build tab.** Paste `interactionModels/custom/en-US.json` over the JSON editor, then **Save Model**
   → **Build Model**. The invocation name is **`home guardian`**.
3. **Code tab.** Replace `lambda_function.py` with `lambda/lambda_function.py`, and add a second file
   `backend.py` with `lambda/backend.py`. Alexa-hosted keeps extra files under `lambda/`, and
   `lambda_function.py` imports `backend` as a sibling, which resolves at runtime.
4. **Environment.** If your hub requires a token, set `HG_MCP_TOKEN` — and `HG_MCP_HOST`/`HG_MCP_PORT`
   only if the hub is genuinely reachable from the Lambda. **It will not be, from an Alexa-hosted
   Lambda:** `127.0.0.1` there means the Lambda itself, not your phone. See the next section.
5. **Test.** Test tab → **Alexa Simulator**, type or say the utterances above.

### Expect "currently unavailable" in the console — and why that is correct

An Alexa-hosted Lambda runs in Amazon's cloud. `127.0.0.1:8790` there is **the Lambda's own loopback
interface**, not your phone's, so the call cannot reach your hub and the skill will say it is
unavailable. That is the designed behaviour, and it is the honest result: there is **no confirmed
remote path** to the hub today.

To get a real answer you need one of:

- the hub reachable from the Lambda (a public/authenticated endpoint — `#23`'s LAN-boundary decision), or
- a deployed VPN coordinator (the `18994`/`18995` pair is **not confirmed deployed**), or
- running the skill where the hub is local.

**Do not "fix" this by pointing the skill at a non-loopback address to make the demo work.** The
transport refuses that deliberately, and a demo that reports household data it did not receive is worse
than one that reports it cannot reach the hub.

## Running the tests locally

```bash
cd alexa_skill_python
python3 -m venv .venv
.venv/bin/pip install -q -r lambda/requirements.txt
.venv/bin/python -m pytest test/ -q          # or: .venv/bin/python test/test_skill.py
```

The suite leads with the refusal behaviour and asserts that the unavailable replies contain **no
digits** — a stray number would read as a real reading to a listener who cannot see the source.

## Design notes

- **Authorization stays server-side.** The household id (the Alexa user id) is *forwarded* as
  `X-HG-Household`; the handler never decides what a household may read. A future per-household policy
  belongs where the data is.
- **`RecentAlertsIntent` and `GetAlertSummaryIntent` share one handler.** The owner asked for
  "RecentAlerts"; the repository's existing model already defines `GetAlertSummaryIntent`. Supporting
  both keeps this skill additively compatible with `alexa_skill/` rather than forking the model.
- **Tool names are asserted in tests** against `src/mcp/McpTools.h:23-24`, so a rename upstream fails a
  test here instead of silently at runtime.
