# HG-EDGE-VIDEO — interface specification

**Date:** 2026-10-24
**Workstream:** `HG-EDGE-VIDEO` (observer)
**Basis:** `docs/EDGE_VIDEO_INTEGRATION_ACKNOWLEDGMENT.md` @ `498ccbd` — specifically its section
"Rulings on the four amendments at 96f58b6". **That committed text is authoritative**, and it is
more complete than the abridged reply it was summarised from; §1a records the reconciliation.
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

### 1a. Reconciliation against the committed rulings

This specification was first drafted from the abridged reply, before the authoritative committed
text at `498ccbd` was read. Comparing them found **eleven constraints present in the commit and
absent from the reply**. They are binding and are incorporated below; where the two differ, the
commit governs.

| # | constraint from the committed rulings | where it lands here |
|---|---|---|
| 1 | do not interpolate missing history and present it as captured frames | §2, §5.1 |
| 2 | derive dimensions from checked headers | §2 |
| 3 | **no network or disk encoding in the callback** | §2, §5.6 |
| 4 | measured input cadence and memory — not guessed frame sizes — determine supported pre-roll | §5.1, §7 |
| 5 | reuse the pipeline under the existing serialization discipline, **without recursive locking** | §3 |
| 6 | no caller-selected **severity** (as well as authorization) | §3 |
| 7 | final signature and **admission/rate limits** require interface review | §3 |
| 8 | synthetic **and uncertain** input must never enter the real video store | §4 |
| 9 | reuse the existing Android image-decoding facilities for event-only decode | §5.6 |
| 10 | **atomic** finalization of the MP4 | §5.5 |
| 11 | capture authorization and registry provenance must be tested on the **actual installed phone** | §7 |

The lesson is recorded rather than left implicit: a chat summary of a committed document is not the
document. The commit is the artifact of record, and the spec is checked against it.

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
| timestamps | **actual capture timestamps preserved**; the ring must not resynthesise them. Missing or unobserved history is **never interpolated and presented as captured frames** |
| cadence | snapshot cadence. **Must never be described as continuous or full-rate video** |
| dimensions | **derived from checked headers**, never assumed and never taken from a caller |
| callback discipline | **no network and no disk encoding inside the callback** — it borrows bytes and returns |
| authority | authority is **rechecked before retaining, encoding or publishing**, not only at admission |
| pre-roll sizing | **measured input cadence and memory determine supported pre-roll** — not a guessed per-frame size |

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
* caller-selected authorization **and caller-selected severity** are forbidden;
* a submitted observation is **not** a persisted alert (C3).
* the existing pipeline is reused under its existing serialization discipline, **without recursive
  locking**;
* the final method signature, and the admission and rate limits, are settled by **interface review**
  — not assumed by the caller.

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
* **synthetic and uncertain input must never enter the real video store**, on any path;
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
* **Missing history is never interpolated** to fill a gap and then presented as captured frames; a
  gap stays a gap, and the clip is honest about it.
* Authority is **rechecked before retaining**, not only at admission.
* The callback path performs **no network and no disk encoding**.

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
* Finalisation is **atomic**: the clip becomes visible to the uploader only once it is complete and
  measured, so a cancelled or failed recording can never be published as a finished artifact.
* Fail closed when the clip cannot be produced; no partial clip is ever published.

### 5.6 `AndroidVideoEncoder` — platform glue, deliberately separate

Android codec code is kept in its own unit so §5.5 stays pure and host-testable. Platform H.264
`MediaCodec` plus `MediaMuxer` for MP4 packaging; **no ffmpeg executable, no new model runtime and no
vendored library.** Event-only JPEG decode **reuses the existing Android image-decoding facilities**
rather than adding a second decoder.

The backend is **injected** into the pure recorder, so the recorder's policy and bounds are testable
with a fake while the platform path supplies the real encoder.

At runtime the encoder must verify, and **fail closed if unsupported** with no silent fallback and no
dependency installation:

* encoder availability and a usable configuration;
* supported dimensions and colour formats, and the JPEG-to-encoder conversion;
* presentation timestamps;
* EOS / drain completion;
* cancellation mid-stream;
* bounded working memory;
* atomic MP4 finalisation (see §5.5).

**API 30 alone does not prove a usable H.264 encoder**, a compatible colour format, the dimensions,
the throughput, the thermal behaviour or a successful MP4 finalisation. Encoder acceptance is a
**device** result; host fakes cannot prove codec execution on the phone.

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
* **capture authorization and registry-derived provenance exercised on the actual installed phone**
  — the acquisition-derived `real` claim is a device result and cannot be established by a host
  test or a fake.

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
