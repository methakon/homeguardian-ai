# Real-event pipeline acceptance

## Current verdict

**NOT ACCEPTED for physical-camera event analysis or automatic Alexa notification.**
The camera-source follow-up integrates source tip
`98280bf4ffb1f3b8147f3e81256657735191adea` with main after `7e87535`.
Fetching that branch again returned the same tip. No deployment, hardware capture,
cloud invocation, sensor activation or existing service reconfiguration was performed.

The imported camera route is an operator-authorized **refreshing JPEG snapshot**
viewer, not continuous video and not an analysis worker. Its status explicitly
reports `analysis_connected=false`. Successful preview requests must not be counted
as analyzed events, persisted alerts or notifications.

## Nine-category capability assessment

These are source-level findings, not physical acceptance results. File references
are relative to the integrated checkout.

| Requested category | Existing implementation | Physical-camera status |
|---|---|---|
| People and household activity | `src/core/CameraActivitySession.cpp` measures changes between decoded luma planes; explicitly excludes identification of people, gestures and activities | JPEG preview is not decoded or submitted to this session; motion analysis is not person recognition |
| Authorized camera/video events | Snapshot fetcher plus separate `src/camera/RegistryAuthorizer.cpp` device identity/consent/revocation checks | Preview now requires operator authorization, explicit plaintext opt-in, a secure password file and capture enabled; device-registry authorization and continuous analysis are not connected to this route |
| Audio/environment sounds | `src/audio/AudioActivitySession.cpp` analyzes PCM level and statistical change; `src/hg_audio_bridge.cpp` supplies real microphone PCM | JPEG has no audio; a separately authorized microphone/audio transport is required, and semantic sound recognition is not established |
| Voice/spoken interactions | `src/voice/VoiceResponseController.cpp` produces spoken output; ASK accepts configured intents | No camera-side speech recognition; spoken output and Alexa intent handling are not local ASR |
| Visitor/delivery events | `src/core/SecurityObservation.h` represents motion, presence and contact states | No validated visitor/package recognition or delivery classifier found; generic motion must not be labelled a delivery |
| Unusual household activity | `ActivityChangeDetector`, `AnomalyMonitor` and vector drift components; ingress calls motion/audio change detectors | Statistical mechanisms exist, but portal snapshots do not reach them; a deviation does not establish a dangerous event |
| Falls/potential safety incidents | `SyntheticImpactClassifier` and `SyntheticImpactRule` demonstrate transient-then-stillness with synthetic intensities | Not physical fall detection; never route generic camera motion into synthetic critical-impact evidence |
| Routine changes | `src/core/Routine.h` stores configuration; scalar/vector baseline detectors measure numerical change | No wired household schedule-learning/evaluation path found; a missing observation means not confirmed, not a missed medication/meal |
| Animal/environment activity | `src/core/AnimalActivity.cpp` evaluates bounded hypotheses; `PrecursorSuite` requires corroborating motion/audio/vector/environment inputs | Not connected to physical portal capture; JPEG alone cannot provide all required inputs; no species or disaster prediction claim |

## First missing physical hop and downstream gates

Current preview path:

    authenticated browser request -> bounded HTTP JPEG fetch -> browser image

Required analysis path, **not implemented by this merge**:

    source identity + current consent/revocation/capture checks
    -> bounded acquisition independent of an open browser
    -> bounded decoding and feature extraction
    -> timestamped, provenance-preserving observations
    -> qualified native detector/fusion decisions
    -> durable alert commit by the authorized writer
    -> independent MCP read of the same alert ID
    -> established ASK/mailbox response

Additional unresolved gates:

- The camera session's `camera-motion-activity` detector is not in the conservative
  default fusion allowlist. Configure intended evidence explicitly; do not admit
  all detectors or lower thresholds to force alerts.
- Portal, ingress and MCP use different database defaults. The acceptance fixture
  aligns the writer/reader explicitly; that does not align a deployed installation.
- Ingress/Application policy admission precedes persistence. A failed write can
  consume cooldown; delivery cannot be guaranteed until reservation/commit and
  failure propagation are addressed and tested.
- Snapshot cadence constants are not currently enforced server-side. Blocking
  acquisition and stale/disconnected semantics need a dedicated review before
  unattended acquisition is enabled.
- Native tools are read-only at the API contract, but the current MCP provider
  constructs `PersistenceAudit`, whose database initialization can write schema.
  This is not a certified read-only database connection.

## Alexa delivery boundary

The established classic ASK/SQS lane performs **request/response**: a requested
summary is read from native MCP and spoken by the skill. Neither an internal
`alerts_delivered` counter nor a successful summary proves unsolicited delivery
to an Echo. This merge adds no proactive-event sender, delivery outbox, supported
notification permission or consumer receipt. Those are separate requirements,
not a reason to replace the established working lane.

## Executed local acceptance

- Full native suite: **76,430 assertions / 790 cases passed**.
- Targeted HTTP/scenario/anomaly/portal suite: **1,093 assertions / 87 cases**.
  An initial union filter included an unmatched `[synthetic-impact]` tag; no
  separate coverage is claimed for that tag. The full suite and runtime catalogue
  include the actual registered synthetic critical-impact case.
- Camera/portal regression verifier: **6 tests passed**, with an authenticated
  loopback fixture peer, not a physical camera. RED runs exposed missing password
  loading, unauthenticated fetch, missing plaintext opt-in, malformed-port abort
  and capture-switch bypass before their corrections.
- Portal smoke: **38 checks passed**, in a temporary working directory and store.
- Complete runtime catalogue: **10 IDs**, executed with capture on and off,
  **20 cases / 40 actual Python ASK SDK responses verified**. Each case stops its
  writer before comparing exact rule-output IDs/count/severity with SQLite, a
  separate native MCP process, and both configured alert intents. No backend or
  speech-response mock substitutes for this chain.

Capture-on outcomes:

| Scenario | Durable alerts | Highest severity |
|---|---:|---|
| default | 2 | warning |
| normal | 0 | none |
| unusual | 1 | warning |
| inconclusive | 0 | none |
| anomaly-warmup | 1 | info |
| anomaly-normal | 1 | info |
| anomaly-deviation | 1 | info |
| anomaly-missing | 1 | info |
| synthetic-critical-impact | 2 | critical |
| consent-denied | 0 | none |

All capture-off cases produced zero alerts. Anomaly scenario names describe the
advisory anomaly stage; their info alerts come from the permitted activity rule,
not proof that warm-up or normal anomaly states are emergency detections.

Reproduce after building the current native targets:

    python scripts/verify_camera_portal.py
    python scripts/verify_scenarios_to_alexa.py --build-dir hg_build --output result.json

The second command needs the ASK dependencies specified in
`alexa_skill_python/lambda/requirements.txt` and an existing `TMPDIR`. It uses only
loopback peers, temporary credentials/stores and synthetic observations. Its
result does not certify the deployed Lambda, AWS transport, physical camera or
Echo speech. Valid observations should all be evaluated; normal, ambiguous,
unconsented and insufficient evidence must not be forced into alerts.
