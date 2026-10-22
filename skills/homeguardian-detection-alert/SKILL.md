---
name: homeguardian-detection-alert
description: Use when working on HomeGuardian detection, alerts or sensor evidence.
version: 1.0.0
author: swarna-sekhar-dhar
license: UNLICENSED
metadata:
  hermes:
    tags: [homeguardian, cpp, detection, fusion, mcp, sqlite, security]
    related_skills: [homeguardian-ai, home-security-sensor-nodes, native-cpp-cmake-sqlite-apps]
---

# HomeGuardian detection and alert guidance

Worker guidance and validation for detection/alert work in `homeguardian-ai`. This is **not** a runtime and
**not** a replacement for the native C++ application: the engine in `src/` is the only detector. Every claim in
this skill is anchored to a file:line in this repository, and `scripts/skill_lint.py` fails the build if an
anchor stops resolving. Read the code before repeating anything here.

## 1. Trigger / use cases

Load this skill when you are:

* adding or reviewing a detector, an observation source, or an evidence rule;
* changing anything that can raise or suppress an alert;
* writing a fixture that must produce (or must NOT produce) an alert;
* investigating animal-activity or environmental-precursor ideas (see §11 — hypotheses only);
* answering "why did no alert fire?" — the answer is almost always a schema or evidence question, not a
  threshold question (§3, §5, §6).

## 2. Architecture and ownership

| component | where | owns |
|---|---|---|
| C++ analyzer (ingress + detectors + fusion + alert policy + SQLite) | `src/`, runs as `hg_device_ingress` | the detection decision and the alert record |
| Ubuntu/Docker Alexa-device simulator host | `deploy/docker/`, `sim/` | hosting the analyzer; the device twin |
| Android/phone node | simulated camera/audio **ingress only** | producing observations; no detection authority |
| MCP server | `hg_mcp_server` | read-only projection of what the writer committed |
| Oracle VM / VPN / auth | out of band | VPN and auth only — **NOT a media relay and not a detection host** |

The analyzer is the single writer of alerts; the MCP side is a separate process that opens the store
**read-only**. Keep those two roles separate in every fixture, because that separation is what makes an alert
reported independently verifiable.

## 3. Observation schema and allowed combinations

Framing and the wire contract: see `references/observation-schema.md` (4-byte big-endian length prefix, body
<= 4096 bytes, `hello` then `observation` frames, `src/net/DeviceProtocol.h:29,49`).

The combination that decides everything downstream:

* `source_type` must be a value of `SecuritySourceType` (`motion`, `presence`, `camera_availability`,
  `microphone_availability`, `contact`, `tamper`, `temperature`, `humidity`, ...). An unknown value is refused
  as `unknown_source_type` — never coerced.
* `observation_type` must be in the ingest allowlist (`src/net/ObservationIngest.h:51-53`):
  `motion-changed-fraction, audio-rms, audio-peak, audio-zero-crossing-rate, activity-level, availability,
  contact-state, temperature, humidity, pressure, illuminance, tamper-state`.
* `state` selects the detector status through `security_status_for(source_type, state)`
  (`src/core/SecurityObservation.cpp:75-110`). **This is the field that decides whether a frame is evidence.**

| source_type | `state` -> **detected** (a positive vote) | `state` -> not_detected | anything else |
|---|---|---|---|
| `motion` | `on`, `detected`, `active` (`:78`) | `off`, `clear`, `idle` (`:79`) | `inconclusive` (`:80`) |
| `presence` | `on`, `present`, `occupied` | `off`, `absent`, `clear` | `inconclusive` |
| availability types | `offline`, `unavailable`, `error`, `unknown` (`:95`) | `online`, `available`, `ok` (`:97`) | `inconclusive` (`:98`) |
| `tamper` | `tamper`, `detected`, `unexpected_disconnect` (`:100-101`) | `clear`, `ok` (`:103`) | `inconclusive` (`:104`) |
| threshold types | `crossed`, `above`, `below` (`:106`) | `normal`, `within` (`:107`) | `inconclusive` (`:108`) |

Valid vs invalid examples: `examples/valid-observation.json`, `examples/invalid-observation.json`.

**The classic failure**: `state:"present"` on a `motion` source is `inconclusive` -> not a vote -> an endless
stream of accepted, audited, correctly-ACKed frames that can never alert. Always check the row above before
suspecting a threshold.

## 4. Live timestamps, replay, identity, consent, provenance

* `acquired_at_ms` is the **transport** timestamp and must be live (epoch ms, now). A pinned/stale value is
  refused as `malformed_timestamp`; a future value is refused too. Pinning a golden clock is a *negative*
  control, not a way to make a fixture pass.
* The receiver derives the **event** timestamp from `acquired_at_ms`; the rule windows on the event clock. Two
  clocks: do not confuse them (`Pipeline.h:15`, `Pipeline.cpp:28`).
* `sequence` must increase per device; a repeat is refused as `replayed_sequence` (negative control in
  `scripts/verify_alert_causality.sh`).
* Identity: `device_id` is cross-checked against the authenticated mTLS peer; an unknown or revoked device is
  refused (`acks=0, accepted=0`).
* Consent: a device without granted consent / capture enabled does not produce usable evidence.
* Provenance: synthetic frames carry `acquisition_mode:"synthetic"` and `provenance_claim:"synthetic"`. Never
  emit a real-provenance claim for synthetic data.

## 5. Sensor detection vs liveness vs inconclusive

Three different things, and conflating them is the most common analytical error:

* **detection** — a status of `detected` with a confidence: a usable positive vote.
* **liveness** — an availability observation that says a device is *healthy*. Healthy availability is
  `not_detected` (`:97`), i.e. a *negative* vote; it feeds the availability tracker, not an activity detector.
  Availability is only `detected` when it reports a **problem** (`:95`).
* **inconclusive / unavailable / stale** — no vote at all (`FusionEngine.cpp:26,28`).

So "availability is inert" is precise as **"a healthy availability report cannot produce a positive vote"**.
That is the negative control, and 0 alerts is the correct answer — not a bug to fix by lowering a gate.

## 6. Evidence detector IDs, operator opt-in, window, fusion

* An ingested observation's detector id is `sensor.<source_type>` (`SecurityObservation.cpp:124`).
* `DetectionFusionRule::Config::evidence_detector_ids` defaults to `{"activity-energy"}` only
  (`DetectionFusionRule.h:50`), so with defaults, ingested evidence is audited but casts **no vote**. Naming
  your ids — e.g. `["sensor.motion","sensor.presence"]` — is the **operator opt-in** (ADR-014). An empty list
  means "allow all" (`:79`).
* Gates: `min_usable = 2` (`:36`), `min_confidence = 0.5` (`:37`), `use_episode_evidence = false` by default
  (`:55`). A vote is `detected`/`not_detected` **with a confidence** (`FusionEngine.cpp:33-35`); stale,
  inconclusive and unavailable are excluded.
* Window: the correlation window defaults to **5000 ms** (`Pipeline.h:15`) applied as
  `now - observation_timestamp <= window` (`Pipeline.cpp:28`) — evidence from both sources must land in one
  5-second span.
* Verdict: `agreement = max(positive, negative) / total` must clear the rule's agreement threshold, and the
  fused confidence must clear `min_confidence`. The rule is evaluated **as events arrive**, so a single
  locally-favourable 5s window can deliver an alert even when the whole-run statistic reads `inconclusive`
  (visible in the in-repo corroboration case).
* **Never** reach for a threshold change to make a fixture pass. Fix the schema or the stimulus
  (`examples/positive-fixture.md`), or report BLOCKED with the precise condition.

## 7. Confidence, debouncing, cooldown, severity, persistence, MCP

* Confidence is carried from the evidence and calibrated by the rule; do not invent a number.
* Debouncing/cooldown: `AlertPolicy` (`main_ingress.cpp:271`, 60 s window) suppresses a repeated alert; a
  suppressed alert is still an event worth logging.
* Severity comes from the rule's severity policy — `warning` in the tested positive below.
* Persistence: the ingress is the **single writer** (`main_ingress.cpp:296-297` and `:304-305`,
  `audit->record_alert`).
* MCP: a separate process opens the store **read-only** and answers `get_alert_summary`. The reader must agree
  with the writer — if it does not, the alert is not verified.

## 8. Tamper and revocation negative controls

Tamper states are mapped at `SecurityObservation.cpp:100-104` (`unexpected_disconnect` is a detection), and a
revoked device is refused before any parsing. Both belong in the negative half of any fixture: a control that
cannot fail proves nothing.

## 9. Tested positive and negative (the evidence this skill rests on)

`scripts/verify_fusion_window.sh` @ `4bc964d` — 6 passed, 0 failed, own isolated ports/stores, no threshold
touched:

```
POSITIVE  evidence allowlist = sensor.motion + sensor.presence (ADR-014 opt-in)
  motion   (source_type=motion,   observation_type=motion-changed-fraction, state=on):      acks=12 accepted=12
  presence (source_type=presence, observation_type=availability,             state=present): acks=12 accepted=12
  app DB  alert-6d5a4af5-f9de-4469-af2e-5295924c6b51 | warning | rule=detection-fusion | confidence=0.9
  READER  separate READ-ONLY MCP process -> {"alert_count":1,"alert_ids":["alert-6d5a4af5-..."],
                                             "highest_severity":"warning","read_only":true}
NEGATIVE  availability-only (state=present -> inconclusive): 12/12 accepted -> alert_count=0  (correct)
```

Before that, every "positive" attempt showed 0 alerts while all gates were satisfied — because its frames
carried `state:"present"` on a `motion` source (see §3). Reproduce the positive with
`examples/positive-fixture.md`.

## 10. Required audit trail

Any detection claim must be traceable end to end, and each hop must be a separate observation:

**frame -> per-frame ACK (`accepted`, correlated to its `observation_id`) -> detector vote (which detector id,
which status, what confidence) -> fused verdict + confidence -> alert ID in the SQLite store -> the read-only
MCP answer naming that same alert ID.**

If a link is missing, report the claim as unverified rather than asserting it.

## 11. Animal activity and environmental precursors — hypotheses only

Two categories, and they must never be merged:

* **empirically validated** — pattern detection from motion/audio analysis that this engine actually performs
  and can be tested (a detected change in motion/audio evidence is a *pattern in evidence*, not a claim about
  its cause).
* **unvalidated / speculative** — species inference, and any suggestion that animal or environmental behaviour
  predicts a disaster. Small-animal/ant/rodent/bird/audio patterns may be listed as **future hypotheses or
  feature-extraction ideas, never as implemented capability**, and **never** as causal disaster-warning claims
  without validation. Absence of evidence must be stated as absence of evidence.

Every investigation must also account for: privacy and consent (no continuous production capture without
explicit approval), false positives, sensor noise, baseline drift, ordinary household behaviour as the null
hypothesis, and species uncertainty. Physical camera/microphone activity is opt-in per task and never implied
by this skill.

## 12. Checklist for a new detector

1. source allowlist: a real `SecuritySourceType` + an `observation_type` in `ObservationIngest.h:51-53`;
2. detection state: states mapped to `detected`/`not_detected` in `security_status_for` (no default-to-detected);
3. registry: the device is registered, paired, consenting — and revocation is covered by a test;
4. synthetic provenance: fixtures label themselves synthetic;
5. privacy review: what it observes, retention, and who can read the MCP view;
6. security review: identity cross-check, replay/sequence, bind scope (loopback), no broad binds;
7. tests: unit + mutation (a near-miss input must NOT alert) + real mTLS cross-process + MCP read-back.

## 13. Changing the engine

This skill guides; it does not patch. Propose runtime changes **separately**, with tests, and never silently.
Preserving existing behaviour beats a clever improvement. Thresholds are not tuning knobs for fixtures.

## 14. Evidence discipline

Cite `file:line` from **your own checkout**; do not cite this skill as a proxy for the source. Run
`scripts/verify_skill.sh` after touching detection code or this skill: it lints the skill and re-checks that
every anchor it quotes still exists. A skill that drifts from the code is worse than no skill.
