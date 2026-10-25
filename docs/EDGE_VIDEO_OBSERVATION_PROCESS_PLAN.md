# HG-EDGE-VIDEO — observation process plan

**Date:** 2026-10-24
**Workstream:** `HG-EDGE-VIDEO` (observer)
**Basis:** `docs/EDGE_VIDEO_INTERFACE_SPECIFICATION.md`; rulings at `docs/EDGE_VIDEO_INTEGRATION_ACKNOWLEDGMENT.md` @ `498ccbd`
**Status:** PLAN. **Implementation gate CLOSED** — no code, no shared-file edit. This document is the
work order that executes the moment the gate opens.

---

## 1. The process in one view

The observation process turns an authorized camera frame into a persisted, playable event clip,
without ever putting semantic inference on a timer.

```
  ┌─ integration-owned ────────────────────────────────────────────────────────────┐
  │  camera fetch → registry authorization → JPEG bytes                            │
  │                                            │                                   │
  │                    (A) pre-decode callback │ borrows immutable bytes           │
  │                                            ▼                                   │
  │         existing path: validate → decode → 160x120 luma → CameraActivitySession │
  │                                            │  findings: motion_level,           │
  │                                            │  change_character, travel,         │
  │                                            │  idle band                         │
  └────────────────────────────────────────────┼───────────────────────────────────┘
                                               │
  ┌─ observer-owned ─────────────────────────┼───────────────────────────────────┐
  │                                           ▼                                   │
  │  (1) EventRing ──── bounded JPEG copies, original timestamps                  │
  │        ▲                     │                                                │
  │        │                     │ on fire, from first_weak_hit - pre-roll        │
  │  (2) Tier0Gate ◄── findings  │                                                │
  │        │  strong→fire now    │                                                │
  │        │  weak→2 of last 3   │                                                │
  │        │  latch 0.8-1.5 s    ▼                                                │
  │        └──────► (3) EventJobQueue ──bounded 1-2, coalescing──► (4) EventClipRecorder
  │                        ▲                                          │ decode (existing
  │                        │                                          │ facilities) → H.264
  │                 (5) ThermalWorkController                          │ (MediaCodec) → MP4
  │                 shrinks 3/4 only; NEVER the gate                   │ (MediaMuxer)
  │                                                                    ▼ atomic finalize
  │                                                        size + MD5 on FINAL mp4
  │                                                                    │
  │                                                                    ▼
  │                                                        (6) EventClipUploader
  │                                                        reserve → PUT → complete
  │                                                        durable identity, bounded outbox
  └────────────────────────────────────────────────────────────────────────────────┘
                                               │
  ┌─ integration-owned ───────────────────────┼───────────────────────────────────┐
  │  (B) submit-advisory ── advisory detector id, no allowlist entry               │
  │        → existing Pipeline → AlertPolicy → PersistenceAudit → MCP/portal/Alexa  │
  └────────────────────────────────────────────────────────────────────────────────┘
```

Two crossings, both integration-owned: **(A)** the borrowed JPEG callback in, **(B)** the
submit-advisory call out. Everything between them is observer-owned and host-testable.

## 2. Stage by stage

### Stage 0 — Acquisition and authorization (integration-owned, unchanged)

Fetches, authorizes against the registry, validates the bytes, then decodes to reduced luma for the
existing session. **The observer adds nothing here and decodes nothing here.**

### Stage 1 — `EventRing` (observer)

| | |
|---|---|
| trigger | (A) callback invocation, borrowing immutable JPEG bytes |
| in | borrowed bytes + checked header dimensions + original capture timestamp |
| out | bounded retained copies, addressable as `[t_start, t_end]` |
| bounds | **total bytes** (capacity in bytes, not frames), frame count, age, per-callback work |
| copy rule | the callback borrows; **the ring owns the retained copy** |
| honesty rule | gaps stay gaps — history is **never interpolated** and presented as captured frames |
| invalidation | consent withdrawal · source/enrollment change · lifecycle stop → **clear retained data, then invalidate queued work**, deterministically |
| authority | rechecked before retaining, not only at admission |
| forbidden | any network or disk encoding inside the callback |

### Stage 2 — `Tier0Gate` (observer, pure)

Consumes the **existing** findings; extracts nothing.

| | |
|---|---|
| in | `motion_level` (changed fraction), `change_character`, region travel, the session's **own calibrated idle band** |
| strong | above the calibrated high threshold → **fire on one frame** |
| weak | above the calibrated low threshold → fire only if **2 of the last 3 observations** agree |
| latch | open 0.8-1.5 s past the **most recent valid hit**; further hits extend the same event |
| anchor | **`first_weak_hit` is recorded on the first weak hit, not at confirmation** — it anchors the look-back |
| states | `QUIET → CANDIDATE → ACTIVE → COOLDOWN → QUIET` |
| invariant | **no transition may reduce the Tier-0 rate** |
| forbidden | hardcoded fractions that ignore the session's idle-band calibration |

**The gate fires on the project's own definition of motion, not on a raw fraction.** The repository
already carries a closed `ChangeCharacter` vocabulary that separates movement from lighting, and a
helper that tests exactly "motion, not merely change". The gate composes both:

    fire only on   is_motion(character)  ==  localized_motion | diffuse_motion
    never on       global_illumination      a brightness step
                   illumination_settling    the camera's own decay after that step
                   localized_unipolar       an object appearing, or a light switching on IN the scene
                   quiet, unknown

Thresholds are **multiples of the session's calibrated idle band**, not absolute constants — the
sensor's own idle floor is a property of that camera, exposure and lighting (the value measured on
the target camera is ~0.0098), so an absolute fraction would either label noise as motion or hide a
small body. `diffuse_motion` additionally requires travel consistency, because that class includes
rain and foliage.

**Prototype status (2026-10-24).** The gate was prototyped out-of-tree against the repository's real
headers and host-tested: **21/21 checks pass**, covering illumination rejection, single-strong-hit
firing, the K-of-N weak path with an interrupting quiet observation, the anchor preceding
confirmation, candidate expiry, the diffuse travel guard, stalled-vs-quiet, event extension instead
of duplication, and latch release. **The first draft failed 2 of 18 checks** — a shared timeout field
made the candidate state cancel itself on the first non-motion observation, silently missing any
crossing with a gap between hits. That is the primary target case, and review had not caught it
(FRICTION_LOG Incident 85). The prototype, not the plan, found it.

Nothing from the prototype is committed to the repository; it lives in the scratch workspace and is
re-expressed here so the implementation inherits the fix rather than the bug.

### Stage 3 — `EventJobQueue` (observer)

| | |
|---|---|
| in | one job per event: `first_weak_hit`, event end, ROI |
| bounds | **1-2 jobs, hard**; overlapping activity **coalesces into the current event** |
| worker | sleeps on a condition variable; no polling |
| shutdown | `stop()` returns promptly; no thread outlives its owner |
| forbidden | enqueueing duplicates for one continuous event |

### Stage 4 — `EventClipRecorder` + injected encoder (observer)

| | |
|---|---|
| in | the ring window `[first_weak_hit − pre-roll, event end]` |
| pure part | clip policy, window selection, bounds, cancellation, final measurement |
| platform part | `AndroidVideoEncoder`: `MediaCodec` H.264 + `MediaMuxer` MP4, **injected** behind the pure recorder |
| decode | event-only, **reusing the existing Android image-decoding facilities** — no second decoder |
| output | `duration_seconds`, `size_bytes`, `content_md5` — **measured on the finalised MP4** |
| finalisation | **atomic** — the clip becomes visible only when complete and measured |
| failure | fail closed; **no partial clip is ever published**; no silent fallback, no dependency install |
| forbidden | asserting or upgrading provenance |

### Stage 5 — `EventClipUploader` (observer, thin producer client)

| | |
|---|---|
| flow | reserve → direct PUT with the returned headers unchanged → complete via the gateway's returned `complete_path` |
| identity | durable, and **a retry reuses the same event identity** |
| outbox | **bounded, with an explicit capacity AND loss policy** — a bound without a stated loss policy is not a policy |
| transport | injected and reviewed; **no unapproved dependency**; injected credential provider in tests |
| `HTTP 507` | expected storage refusal: keep the identity, report blocked storage, bounded retry, **never mark the local clip uploaded** |
| forbidden | hot-loop retry · event-ID rotation · discarding the local clip as uploaded · a second signer, quota authority or cleanup service |

### Stage 6 — Submit-advisory (integration-owned; the observer is a caller)

| | |
|---|---|
| observer supplies | bounded observation identity · the agreed detector ID · a bounded summary |
| integration supplies | source identity · consent · provenance · trusted timing |
| rules | caller validation is **not authority**; no reserved-detector impersonation; **no caller-selected authorization or severity**; an accepted observation **is not** a persisted alert |
| signature | final signature and admission/rate limits are set by **interface review** |

### Stage 7 — Read surface (integration-owned, unchanged)

Existing MCP tools, portal records and playback, Alexa adapter. The observer exposes nothing new here.

### Cross-cutting — `ThermalWorkController` (observer)

Reads the device's own trip points; the **minimum** passive trip across all zones is the envelope (on
the target, the binding zone is skin/quiet, not CPU). Steps down **semantic** work in stages and
**never** the Tier-0 rate — degrading the gate to save heat is the one thing the architecture exists
to prevent.

## 3. Interface requests to issue when the gate opens

Both are integration-owned additions, requested as focused patches — not edits by this workstream.

**Request A — the pre-decode callback.**
A borrowed, read-only callback exposing the fetched JPEG bytes at the point they already exist,
emitted **after registry authorization and after equivalent size/header/dimension validation** (the
existing guard lives inside `decode_jpeg_luma`, so it does not cover this seam). Terms to agree:
lifetime (invocation only), dimensions (from checked headers), capture timestamp, provenance,
cancellation, backpressure. **No decode added at the seam; no network or disk encoding in the
callback.**

**Request B — the submit-advisory method.**
A public method on `CameraAnalysis` accepting one advisory observation and feeding the **existing**
`Pipeline → AlertPolicy → PersistenceAudit` path under the existing serialization discipline
**without recursive locking**. Carries the agreed detector ID with **no allowlist entry**. Caller
supplies identity and summary only. Final signature and admission/rate limits set by interface
review.

**Request C — build registration.**
Registration of `src/edge/*` and its test target, so the components compile in the project build.
Until this lands, the components cannot be exercised by the project's own build — which is why
host verification is planned out-of-tree first (see §5).

## 4. Observation lifetime, end to end

| property | value |
|---|---|
| what is retained | JPEG bytes only, bounded |
| what crosses (A) | borrowed bytes, invocation-scoped |
| what crosses (B) | bounded identity, detector id, summary — no media |
| what leaves the node | the finalised MP4 to object storage, and the clip metadata to the gateway |
| what never leaves | raw frames in any continuous form; the app-private credential; any R2 account secret |
| revocability | consent withdrawal or source change clears the ring and invalidates queued work before anything further is encoded or published |

## 5. Verification plan

**Host (hermetic — no phone, camera, network or gateway).** Built out-of-tree against the existing
shared headers until Request C lands, then folded into the project build.

* `Tier0Gate`: truth table; latch extension; **`first_weak_hit` strictly earlier than confirmation**;
  the invariant that no transition reduces the Tier-0 rate.
* Adversarial fixtures: a uniform gain/exposure step must **not** fire; a compact body must.
* `EventRing`: bounds on bytes/count/age/callback work; deterministic clear-and-invalidate on
  withdrawal, source change and shutdown; a gap is never interpolated.
* `EventJobQueue`: saturation, coalescing, prompt `stop()`, no duplicate jobs for one event.
* `ThermalWorkController`: transitions, and Tier-0 untouched in every state.
* `EventClipRecorder` with a **fake encoder**: fail-closed paths, cancellation, atomic visibility,
  size/MD5 taken from the finalised artifact.
* `EventClipUploader` with a **fake transport and injected credential provider**: idempotent retry on
  one identity, outbox capacity **and** loss policy, and 507 stopping rather than looping.
* **Pipeline impact (required, not assumed):** mixed-traffic history eviction, alert output, and
  cooldown interaction with a real detection — advisory is **not** assumed inert.

**Device (cannot be substituted by host tests).**

* `MediaCodec` availability, a usable configuration, colour format and conversion, presentation
  timestamps, EOS/drain, cancellation, bounded memory, atomic finalisation — **API 30 alone proves
  none of these.**
* Measured ms/check for the existing extractor, in % of one core, against the device's binding
  thermal trip point.
* Real events produce a clip; a quiet scene produces none.
* 30-60 min soak logging CPU, temperature and **queue depth**, with queue depth not climbing.
* Capture authorization and registry-derived provenance exercised on the **actual installed phone**.

**Reported as four separate states, never merged:** source implementation · host tests · phone
execution · end-to-end acceptance.

## 6. Work order once the gate opens

1. Issue Requests A, B and C.
2. Prototype and host-test the pure components out-of-tree; fold into the project build when C lands.
3. V-0: measure the existing extractor on the device. **This sizes everything else** — pre-roll depth
   and duty cycle are derived from the measurement, not from a reference document.
4. V-1: exercise Request A against the real seam; confirm bounds and lifecycle with the ring.
5. V-2/V-3: gate and queue against recorded real events; validate recall before tightening precision.
6. V-4: submit-advisory through Request B, with the mixed-traffic non-regression suite.
7. V-5: recorder and uploader on the device; joint first-clip checkpoint with integration; live
   storage acceptance is theirs and waits on this.

## 7. What this plan does not claim

No implementation exists. The gate is closed. Every device-level item in §5 is unverified until run
on the handset, and every host-level item is a test to be written. Nothing in this document is
evidence of anything.
