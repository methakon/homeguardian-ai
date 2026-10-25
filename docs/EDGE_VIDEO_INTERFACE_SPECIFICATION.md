# HG-EDGE-VIDEO — interface specification

**Date:** 2026-10-24
**Workstream:** `HG-EDGE-VIDEO` (observer)
**Basis:** `docs/EDGE_VIDEO_INTEGRATION_ACKNOWLEDGMENT.md` @ `a3a53fb`; the four amendment rulings
recorded in the integration worker's reply to `docs/EDGE_VIDEO_OWNERSHIP_CONFIRMATION.md` @ `96f58b6`.
**Status:** SPECIFICATION FINALISED. **Implementation gate remains CLOSED** — no code, no shared-file
edit, pending the MCP recovery/stability handoff.

All four amendments were accepted. Three corrections were returned that change claims made in this
workstream's earlier documents; they are applied here and the earlier text is superseded.

---

## 1. Corrections accepted

**C1 — the JPEG validation guard is not at the seam.** This workstream described the fetched frame as
"bounded by the decoder's own ≤512 KiB guard, in hand at `process()` entry". That was wrong about
*where* the guard runs: the size, header and dimension checks live **inside `decode_jpeg_luma()`**,
so they do not protect bytes handed to a pre-decode callback. Integration will place **equivalent
validation before the new callback**. No bound is assumed to exist on the seam by this workstream.

**C2 — allowlist exclusion does not mean zero behavioural impact.** This workstream claimed that an
advisory detector id means "no historical scenario changes behaviour". That is too strong. Even for
an advisory id, `Pipeline::process()` inserts the event into bounded history and evaluates rules, so
**history eviction, alert output and cooldown behaviour can all be affected**. Treating advisory as
inert was wrong. The test obligations in §7 now cover mixed-traffic history eviction explicitly.

**C3 — "advisory" has no enum.** There is no advisory `DetectorStatus`. Advisory means *ineligible for
escalation evidence*; acceptance of an observation does **not** imply a persisted alert. The design
must not model an "advisory status".

Two further constraints accepted without correction and carried into the spec: the provenance comment
in `Observation.h` is **stale for the physical acquisition variant**, and the gateway trusts its
authorized producer's provenance report rather than attesting camera authenticity itself.

## 2. Interface A — JPEG pre-roll seam (integration supplies, this workstream consumes)

The ring's payload is the **fetched JPEG bytes**, read before decode through a borrowed, read-only
callback.

| term | specification |
|---|---|
| borrowed bytes | immutable, **valid only during invocation**; the ring owns any retained copy |
| validation | performed by integration **before** the callback (C1) — never assumed by the ring |
| what the ring must bound | **total bytes, frame count, age, and callback work** |
| invalidation triggers | authorization withdrawal, source change, shutdown → clear retained data **and** invalidate queued work |
| decode policy | the acquisition seam adds **no** decode. Existing analysis continues to decode acquired frames for Tier-0. Decoding retained JPEGs for **event encoding** is a separate, permitted decode |
| timestamps | **actual capture timestamps preserved**; the ring must not resynthesise them |
| cadence | snapshot cadence. **Must never be described as continuous or full-rate video** |

**Terminology constraint for all documents and alerts:** this is *snapshot pre-roll*. The words
"continuous", "full-rate", and "video pre-roll" are not to be applied to it. A clip assembled from
snapshots is honest about being snapshot-derived.

## 3. Interface B — submit-advisory (this workstream calls, integration owns)

Integration owns the addition to `CameraAnalysis` and the shared pipeline wiring. This workstream is
a **caller** of it.

**This workstream supplies, bounded:**

* observation identity;
* the agreed new detector ID;
* a bounded summary.

**Integration supplies (never the caller):**

* source identity;
* consent context;
* provenance;
* trusted timing.

**Rules the caller must respect:**

* "already validated" confers **no** bypass — revalidation at admission applies;
* reserved-detector impersonation is forbidden;
* caller-selected authorization is forbidden;
* a submitted observation is **not** a persisted alert (C3).

**Behavioural contract the caller must be tested against, not assumed into (C2):**

* mixed-traffic history eviction — an advisory event entering bounded history can evict another;
* alert output — advisory must not create one;
* cooldown — advisory traffic must not suppress or extend a real detection's cooldown.

## 4. Interface C — provenance context (integration supplies, this workstream must not promote)

`CameraAnalysis::process()` already derives provenance after registry authorization —
`synthetic_ ? Provenance::synthetic : Provenance::real` — and rechecks authorization before
persistence. Integration owns the focused comment correction in `Observation.h` and its regression
tests; **no enum semantics change**.

The recorder therefore receives an **acquisition-derived, revocable context** and:

* **must not promote synthetic or unknown input to real**;
* must not assert `real` itself, in any field, on any path;
* must carry the context through to the reservation, where the gateway trusts the authorized
  producer's report.

**Acceptance limit, stated plainly:** the gateway does not independently attest physical camera
authenticity. **Actual installed-phone verification remains required** before any claim of real,
detection-triggered capture. A green host suite proves nothing here.

## 5. Component specifications (this workstream owns all of these)

### 5.1 `EventRing`

* Owns the retained copy of the borrowed JPEG bytes (Interface A).
* Bounds enforced, all of them: total bytes, frame count, age, per-callback work.
* Ring capacity is expressed as **bytes**, not frames, so a size spike cannot overrun it.
* On authorization withdrawal, source change or shutdown: **clear retained data and invalidate
  queued work**, in that order, deterministically.
* Retention is sized on the measured frame size in V-0 — **not** on an assumed figure.
* Every retained frame carries its **original capture timestamp**.

### 5.2 `Tier0Gate`

* Pure logic, no platform dependency, no I/O. An asymmetric trigger, a latch, a first-weak-hit
  timestamp and the candidate/active/cooldown states.
* Consumes the **existing** measured signals — it does not extract features. The idle-band
  calibration already present in the existing session is used in preference to any hardcoded
  fraction.
* Invariant, asserted by test: **no state transition may reduce the Tier-0 rate.**
* Pure by construction so it is host-testable without a camera, a phone or a clock.

### 5.3 `EventJobQueue`

* Bounded to 1-2 jobs; the bound is a hard invariant, not a target.
* Overlapping activity **coalesces into the current event** rather than enqueueing a duplicate.
* `stop()` returns promptly; no thread outlives its owner; shutdown is deterministic.

### 5.4 `ThermalWorkController`

* Reads the device's own trip points; the **minimum** passive trip across all zones is the envelope
  (the binding constraint on the target is the skin/quiet zone, not the CPU zones).
* Steps down semantic work only. **Never** the Tier-0 rate.

### 5.5 `EventClipRecorder` — pure logic

* Owns clip assembly policy, bounds, cancellation and the measurement of the **finalised** MP4.
* **Size and MD5 are computed on the finalised MP4**, never on a partial or predicted stream.
* Fail closed when the clip cannot be produced; no partial clip is ever published.

### 5.6 `AndroidVideoEncoder` — platform glue, deliberately separate

Android codec code is kept in its own unit so §5.5 stays pure and host-testable. Platform H.264
`MediaCodec` plus `MediaMuxer` for MP4 packaging; **no ffmpeg executable and no new dependency.**

At runtime the encoder must verify, and **fail closed if unsupported** with no silent fallback:

* encoder availability and a usable configuration;
* supported dimensions and colour formats, and any required conversion;
* timestamp handling;
* EOS / drain completion;
* cancellation mid-stream;
* bounded memory;
* successful MP4 finalisation.

**API 30 does not guarantee a suitable codec configuration.** Encoder acceptance is a **device**
result; host tests cannot establish it.

### 5.7 `EventClipUploader` — thin producer client

* Reserve → direct PUT with the returned headers unchanged → complete, using the gateway's returned
  `complete_path`.
* **Durable identity and retry state**; a retry reuses the same event identity.
* **Bounded local outbox with an explicit capacity and loss policy** (required — a bound without a
  stated loss policy is not a policy).
* Injected, reviewed transport; **no unapproved dependency**; an injected credential provider in
  tests, so no credential is read from the device by a test.
* **HTTP 507** is an expected storage refusal: keep the identity, report blocked storage, bounded
  retry, and **never mark the local clip uploaded**. No hot-loop retry, no ID rotation.

## 6. Bounds, in one place

| bound | owner | notes |
|---|---|---|
| per-frame bytes on the seam | integration (pre-callback validation, C1) | not assumed by the ring |
| ring total bytes | `EventRing` | capacity expressed in bytes |
| ring frame count, age, callback work | `EventRing` | all four enforced |
| event queue depth | `EventJobQueue` | 1-2, hard |
| backend protocol frame | existing ingress | unchanged |
| clip duration and size | `EventClipRecorder` | gateway defaults: 30 s, 25 MiB |
| outbox capacity **and loss policy** | `EventClipUploader` | both required |
| retained evidence JSON | existing observation contract | unchanged |

## 7. Test obligations

**Host tests (hermetic — no camera, phone, network or live gateway):**

* gate truth table; latch behaviour; first-weak-hit strictly earlier than confirmation;
* adversarial fixtures: a uniform gain/exposure step must **not** fire; a compact body must;
* ring bounds (bytes, count, age, callback work) and deterministic clear-and-invalidate on
  withdrawal, source change and shutdown;
* queue saturation, coalescing, prompt `stop()`;
* thermal controller transitions **and** the invariant that Tier-0 is never reduced;
* recorder fail-closed paths, cancellation, and size/MD5 taken from a finalised MP4;
* uploader idempotent retry on one identity; bounded outbox capacity **and** its loss policy;
  507 behaviour — stops, reports, does not hot-loop, does not discard, does not rotate the ID;
* **Interface B behavioural impact (C2): mixed-traffic history eviction, alert output, and cooldown
  interaction with a real detection.**

**Device acceptance (requires the phone; host tests cannot substitute):**

* MediaCodec encoder availability, supported configuration, drain/EOS, bounded memory, successful
  MP4 finalisation, and fail-closed behaviour when unsupported;
* measured ms/check for the existing extractor, in % of one core, against the device's binding
  thermal trip point;
* real events produce a clip; a quiet scene produces none;
* 30-60 min soak logging CPU, temperature and **queue depth**, with queue depth not climbing;
* **installed-phone verification of the real, detection-triggered path** (§4).

**Four states, reported separately and never merged:** source implementation · host tests · phone
execution · end-to-end acceptance. `docs/EVENT_VIDEO_STORAGE.md` is explicit that a signed-URL match
is not a live PUT/HEAD/GET proof; the same discipline applies to every stage here.

## 8. Still gated

* **No code is written and no shared file is edited** until the explicit MCP recovery/stability
  handoff. The gate is closed.
* Integration applies the shared-file changes (JNI call site, lifecycle hooks, build registration,
  the pre-decode callback and its validation, the submit-advisory method) from focused requests.
* Interface A cannot be exercised until integration provides the callback; Interface B cannot be
  called until integration provides the method. Both are theirs.

## 9. What this workstream will not do

No second poller, credential store, HTTP client or capture loop. No second decode at the acquisition
seam. No upload or network call from an acquisition callback. No second signer, quota authority or
cleanup service. No change to thresholds, evidence allowlists, provenance, persistence or fusion. No
new dependency, and no OpenCV/ONNX adoption as a side effect. No shared-file edit. No promotion of
synthetic or unknown input to real. No description of snapshot pre-roll as continuous video. No
claim of real pre-roll, real provenance or device acceptance from host tests.
