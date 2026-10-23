# Real-event pipeline acceptance

## Current verdict

**WIRED for motion and statistical change only; still NOT physical or production accepted.**
The camera-source follow-up integrates source tip
`98280bf4ffb1f3b8147f3e81256657735191adea` with main after `7e87535`. Fetching that
branch again returned the same tip.

When analysis is explicitly enabled for a named, registry-authorized source, each
fetched JPEG is bounded-decoded to a 160x120 luma plane, measured by the existing
`CameraActivitySession`/`ActivityChangeDetector`, submitted to the existing
`Pipeline` with `camera-motion-activity` as the explicit evidence allowlist, and a
qualifying alert is committed to the durable store **before** cooldown advances.
The existing MCP reader and the existing ASK intents then report it.

Everything below remains true: this is a refreshing snapshot source, not continuous
video; no identity, visitor, fall, speech, animal or emergency classifier exists;
and no deployment, hardware capture or cloud invocation was performed here. With
analysis off, the route is still a preview only and reports `analysis_connected=false`.

## Nine-category capability assessment

These are source-level findings, not physical acceptance results. File references
are relative to the integrated checkout.

| Requested category | Existing implementation | Physical-camera status |
|---|---|---|
| People and household activity | `src/core/CameraActivitySession.cpp` measures changes between decoded luma planes; explicitly excludes identification of people, gestures and activities | **Wired**: fetched frames are decoded and analysed when analysis is enabled — reported strictly as motion, never as a person or activity |
| Authorized camera/video events | Snapshot fetcher plus `src/camera/RegistryAuthorizer.cpp` identity/consent/purpose/revocation checks, re-read before every acquisition and before every commit | **Wired** for snapshots: unauthorized, unpaired, revoked, consent-withdrawn or capture-disabled sources are refused before any fetch, and withdrawal resets baseline/policy/history |
| Audio/environment sounds | `src/audio/AudioActivitySession.cpp` analyzes PCM level and statistical change; `src/hg_audio_bridge.cpp` supplies real microphone PCM | JPEG has no audio; a separately authorized microphone/audio transport is required, and semantic sound recognition is not established |
| Voice/spoken interactions | `src/voice/VoiceResponseController.cpp` produces spoken output; ASK accepts configured intents | No camera-side speech recognition; spoken output and Alexa intent handling are not local ASR |
| Visitor/delivery events | `src/core/SecurityObservation.h` represents motion, presence and contact states | No validated visitor/package recognition or delivery classifier found; generic motion must not be labelled a delivery |
| Unusual household activity | `ActivityChangeDetector` (level/trend) plus `AnomalyMonitor` (z-score, advisory only — not in the evidence allowlist) | **Wired** as statistical change over real decoded frames; a deviation is reported and does not by itself establish a dangerous household event |
| Falls/potential safety incidents | `SyntheticImpactClassifier` and `SyntheticImpactRule` demonstrate transient-then-stillness with synthetic intensities | Not physical fall detection; never route generic camera motion into synthetic critical-impact evidence |
| Routine changes | `src/core/Routine.h` stores configuration; scalar/vector baseline detectors measure numerical change | No wired household schedule-learning/evaluation path found; a missing observation means not confirmed, not a missed medication/meal |
| Animal/environment activity | `src/core/AnimalActivity.cpp` evaluates bounded hypotheses; `PrecursorSuite` requires corroborating motion/audio/vector/environment inputs | Not connected to physical portal capture; JPEG alone cannot provide all required inputs; no species or disaster prediction claim |

## Implemented analysis path, and what is still missing

Implemented, opt-in (`--camera-analysis` and an explicitly named source):

    operator-authorized browser request
    -> source identity + consent/purpose/revocation/capture re-read from the registry
    -> bounded HTTP JPEG fetch (server-enforced cadence)
    -> bounded ffmpeg decode to one 160x120 luma plane
    -> existing CameraActivitySession/ActivityChangeDetector observation
    -> existing Pipeline + detection rule (explicit allowlist: camera-motion-activity)
    -> durable commit BEFORE cooldown state advances (admit_durable)
    -> independent MCP read of the same alert ID
    -> existing ASK intents speak the same count/severity

Configuration contract:

    hg_alexa_bridge --http CONFIG CREDS --port P --secret-file S --uploads-dir D \
      --camera-host H --camera-port P2 --camera-allow-plaintext \
      --camera-password-file PW --camera-analysis --camera-registry REG.json \
      --camera-device-id ID --camera-credential-id <64 hex>
    HG_BRIDGE_AUDIT_DB=<same file as HG_MCP_AUDIT_DB>

Missing any of the analysis flags exits 3 before serving. Acquisition is triggered by
the authorized frame request; there is no unattended background capture loop, so
nothing is acquired while nobody is viewing.

Still missing or unproven:

- **Physical acceptance.** No real camera was contacted. The verified fixture is a
  generated JPEG peer, and its provenance is derived as `synthetic` from the reserved
  registry id — a fixture cannot be labelled a physical observation.
- **Deployment.** The portal is now published at `https://homeguardian.berhampore.in/`
  (loopback-bound native bridge behind a Cloudflare tunnel). That publishes a page and
  its read-only APIs; it does not deploy the camera analysis, configure a source, or
  change the classic ASK/SQS/phone lane. No camera frame has been fetched.
- **Proactive Alexa delivery.** The established path is request/response. An
  `alerts_delivered` counter or a spoken summary is not unsolicited notification.
- **Continuous video.** Snapshot polling only; the H.264 path still needs a streaming
  client and muxer.
- **Categories without an implementation** (audio, speech, visitors/deliveries, falls,
  routines, animals, emergencies) are reported `unavailable` in the status payload
  rather than synthesized from motion.

Additional unresolved gates:

- The camera path uses the documented episode opt-in because the default rule needs
  two usable votes that one real sensor cannot produce. Thresholds were not lowered;
  an empty allowlist ("accept all") is still refused.
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
- Camera analysis focused target: **4 cases / 146 assertions passed** (CTest 1/1):
  bounded decode, server-side cadence, fail-closed authorization, durable commit and
  post-revocation reset.
- Independent end-to-end camera check (**generated JPEG peer, not a camera**):
  **40 frames acquired → 40 observations → 2 durable commits** with
  `observations_processed=40`, `decode_failures=0`, `commit_failures=0`,
  `suppressed_alerts=4`; the same alert IDs, count and severity were read back from
  SQLite and the separate MCP process, and both ASK intents spoke
  "There are 2 alerts recorded. The highest severity is warning." Five authorization
  negatives (capture disabled, consent withdrawn, revoked, unpaired, credential
  mismatch) each refused with **zero** camera requests reaching the peer.
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
