# HomeGuardian AI — Alexa Custom Skill (Simulator-First Prototype)

A minimal Alexa custom skill that reports HomeGuardian AI status, alerts, and
routines using **mock data**. It is designed to be tested in the **official
Alexa Developer Console Simulator** with text input, at **zero cost**.

This skill is deliberately **separate** from the C++ core and the Linux/Android
sensor backends. It never accesses the Ubuntu computer's camera or microphone,
and it never claims real-time sensor analysis.

## What this answers up front

**Q: Can the Alexa Developer Console Simulator test this without deploying a
backend?**

**No.** Amazon's simulator requires a configured endpoint and deployed skill
code. The lowest-cost supported path is an **Alexa-hosted skill**, which
auto-provisions an AWS Lambda function **without you creating an AWS account or
paying anything** (Lambda's free tier covers the tiny request volume of testing;
see Cost below). This repository's handler is written for exactly that path
(`index.js` exports `exports.handler`, the standard Lambda entry point).

The unit tests in `test/handler.test.js` run **without any deployment** — they
invoke the handlers directly — so the skill logic is fully verified locally
before you ever touch the console.

## Architecture

```
Alexa (simulator / device)
        |  IntentRequest (JSON)
        v
  Alexa endpoint (Alexa-hosted Lambda by default)
        |  index.js  -> intent handlers
        v
  HomeGuardianBackend interface (src/backend-interface.js)
        |  mock implementation today
        |  (a real implementation would call an authenticated
        |   local HomeGuardian service — see docs/ARCHITECTURE.md)
        v
  (C++ core — NOT contacted by this prototype)
```

- The intent handlers depend only on the `HomeGuardianBackend` interface, not on
  any backend, network, or sensor. Swapping mock → real backend requires no
  handler changes.
- No credentials are stored in this repository. No AWS resources are created by
  these files. No sensor is activated.

## Intents

| Intent | Utterance examples | Response (mock) |
|---|---|---|
| `HomeStatusIntent` | "what is the home status", "status" | "HomeGuardian AI is running. Camera and microphone capture are disabled." |
| `GetAlertSummaryIntent` | "are there any alerts", "alert summary" | "There are no active alerts." |
| `GetRoutineStatusIntent` | "what is my routine status", "routines" | "There are no configured routines." |
| `AMAZON.HelpIntent` | "help" | help text + reprompt |
| `AMAZON.StopIntent` / `AMAZON.CancelIntent` | "stop", "cancel" | "Goodbye." |

Invocation name: **home guardian**.

## Files

- `index.js` — Lambda entry point (`exports.handler`) and intent handlers.
- `src/backend-interface.js` — `HomeGuardianBackend` contract + mock impl.
- `interactionModels/custom/en-US.json` — interaction model (import into console).
- `test/handler.test.js` — 13 unit tests (Node.js built-in test runner).
- `package.json` — deps: `ask-sdk-core` ^2.14.0 (from public npm, no auth).

## Run the tests (no deployment, no AWS, no simulator)

```bash
cd alexa_skill
npm install
node --test test/handler.test.js
```

Expected: `pass 13`.

## Exact manual steps to test in the Alexa Developer Console

You need an Amazon Developer account (free) at
<https://developer.amazon.com/alexa/console/ask>. No AWS account and no payment
method are required for an Alexa-hosted skill.

1. **Create the skill**
   - In the Alexa Developer Console, click **Create Skill**.
   - **Skill name:** `Home Guardian` (any name is fine).
   - **Default language:** English (US).
   - **Experience / model:** choose **Custom**.
   - **Hosting:** choose **Alexa-Hosted (Node.js)**. (Amazon provisions the
     Lambda automatically; you do not touch the AWS console.)
   - Click **Create Skill**, then **Choose a template** → **Start from scratch**
     (or "Hello World"), then **Continue with template** / **Create skill**.

2. **Set the interaction model**
   - Open the skill, go to **Build** → **JSON Editor** (left nav: "Interaction
     Model" → "JSON Editor").
   - Replace the contents with the contents of
     `alexa_skill/interactionModels/custom/en-US.json` from this repository.
   - Click **Save Model**, then **Build Model**. Wait for the build to finish
     (status shows "Build succeeded" / a check mark). A build error must be
     resolved before testing.

3. **Deploy the handler code**
   - Go to the **Code** tab.
   - Replace `index.js` with the contents of `alexa_skill/index.js` from this
     repository.
   - Create `src/backend-interface.js` with the contents of
     `alexa_skill/src/backend-interface.js` (use **Add file** / folder `src/`).
   - Ensure `package.json` lists `ask-sdk-core` (the template's package.json
     already includes it; align the version with this repo's `package.json`).
   - Click **Deploy**. Wait for "Deployment successful".

4. **Enable testing and run the simulator (text)**
   - Open the **Test** tab.
   - Set the dropdown from **Off** to **Development** ("Skill testing is enabled
     in Development").
   - In the **Alexa Simulator** panel, make sure input is **Text** (not voice).
   - Type sample utterances and press Enter, e.g.:
     - `open home guardian`
     - `what is the home status`
     - `are there any alerts`
     - `what is my routine status`
     - `help`
     - `stop`
   - Inspect the **request** (Device Log / JSON) and the **response** shown in
     the simulator.

5. **Record results**
   - The simulator shows Alexa's spoken/written response for each utterance.
   - Use the **Device Log** (or the JSON output) to confirm the request reached
     the endpoint and the response payload. Note any errors verbatim.

### If you cannot or do not want to use Alexa-hosted hosting

- **Local proxy (no public endpoint, no AWS):** the ASK Toolkit for VS Code can
  route requests to skill code running on your computer via an Alexa proxy.
  This requires installing the ASK Toolkit extension and linking the skill ID;
  it tests local code without deploying to Lambda. This is the zero-cost option
  that avoids the hosted Lambda entirely, but it is not the plain developer
  console simulator flow and needs the VS Code extension.
- **Self-hosted HTTPS endpoint:** possible but requires public HTTPS with
  request-signature validation; not recommended for this prototype and not
  implemented here.

## Cost and account requirements (verified current info)

- **Amazon Developer account:** free, required. No credit card needed to create
  skills or use Alexa-hosted skills.
- **Alexa-hosted skills:** Amazon auto-provisions the AWS Lambda (and other AWS
  resources) for you. You do **not** create an AWS account or enter AWS
  credentials. Testing volume is far within AWS Lambda's free tier (1M requests
  / 400,000 GB-seconds per month), so **expected charge: $0** for this
  simulator-first prototype. (AWS pricing is subject to change; no paid tier is
  enabled by this project.)
- **Node.js runtime:** Lambda's Node.js 16 is **deprecated**; Alexa-hosted
  skills default to a current Node.js runtime (18/20/22) or Python. The
  `ask-sdk-core` v2 SDK is compatible. (Verified against current AWS Lambda
  runtime and ASK SDK deprecation docs.)
- **No Echo device, no microphone, no camera** are needed. Text-based testing
  only. Do not enable voice/mic in the simulator.

## Safety

- No camera or microphone status is exposed or controllable from this skill.
- No personally identifiable or sensitive family/sensor detail is returned
  (mock data is aggregate placeholders only).
- The backend interface is the only integration point; the C++ core is not
  contacted. Real backend access would require an authenticated interface and
  separate approval.
- No secrets, keys, or credentials are stored in this repository.
