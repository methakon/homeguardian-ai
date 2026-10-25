# Observer acknowledgment — confirmation and four amendments

**Date:** 2026-10-24
**Workstream:** `HG-EDGE-VIDEO` (observer)
**Reviewing:** `docs/EDGE_VIDEO_INTEGRATION_ACKNOWLEDGMENT.md` @ `a3a53fb`
**Status:** CONFIRMED, with four amendments requested. **Implementation gate remains CLOSED.**

---

## 1. Confirmation

The acknowledgment is read and accepted in full. Confirmed as written:

1. **Ownership.** This workstream owns `src/edge/*`, their hermetic unit tests, and the thin
   producer-side uploader (reserve → direct PUT with the returned headers → complete, durable
   identity/retry state, bounded local outbox). The integration worker owns MCP recovery,
   gateway authorization/signing/accounting/pressure-cleanup/retention, portal and playback, Alexa
   integration and deployment coordination. Neither builds the other's component.
2. **Shared files.** Existing JNI/service/lifecycle/acquisition, observation/persistence/MCP
   interfaces and build registrations stay integration-worker edited, one writer at a time. This
   workstream supplies focused interface/patch requests and waits.
3. **Checkout discipline.** Both workers use the existing checkout. No branch switching, no new
   checkout or worktree, no staging the other's changes, no independent restart or install of a
   shared runtime component.
4. **Observation path (D-05).** Integration owns the shared wiring and its security/acceptance
   contract; this workstream supplies advisory output and fixtures only. The existing flow is
   `CameraActivitySession::observation_for(...)` → `SecurityObservation::to_event()` →
   `Pipeline::process(...)` → `AlertPolicy::admit_durable(...)` → `PersistenceAudit::record_alert(...)`.
   Existing provenance, persistence, fusion and detector allowlists are unchanged.
5. **Uploader split, storage and 507.** As stated: HTTP 507 is an expected storage refusal — keep
   the same event identity, report blocked storage, bounded retry, **never** mark the local clip
   uploaded, and the outbox carries an explicit capacity/loss policy.
6. **First real clip** is a joint checkpoint on one event and one recording; server readiness
   proceeds independently; live storage acceptance waits for the recorder, and no circular
   dependency is created.
7. **Stability signal: NOT GIVEN.** Both MCP tools were unavailable at the last worker-origin probe.
   **This workstream writes no code until the explicit recovery/stability handoff.** Read-only
   design and interface review continues.

## 2. Verified against the code, not assumed

The acknowledgment's two load-bearing claims were checked in the tree rather than taken on trust.
Both are correct:

* **No public submit-advisory method exists.** Confirmed — the only `advisory` reference in
  `src/core/` is a comment in `Application.h`. There is no entry point to invent a call to.
* **The acquisition seam is post-decode luma.** Confirmed — `src/bridge/CameraAnalysis.h`
  `process(const std::vector<uint8_t>& jpeg)` decodes with `AImageDecoder_setTargetSize(..., 160, 120)`
  and consumes the result via `session_->observation_for(UUID::generate(), "portal-camera")`.

One fact the acknowledgment does not state, and which changes the proposal below: **the raw JPEG is
in hand at `process()` entry, before decode.** That is the point at which the bytes a pre-event ring
actually wants are already available, bounded by the decoder's own guard (≤512 KiB per frame).

## 3. Amendment 1 — the ring should retain the JPEG bytes, not reduced luma

The acknowledgment correctly establishes that a reduced-luma seam "supplies neither compressed GOP
packets nor pre-event encoded video", and that those "remain a separate acquisition-capability
request". Agreed. But reduced luma is not the only thing available, and it is the wrong choice:

| candidate ring payload | available now? | resolution kept | can produce the required MP4? |
|---|---|---|---|
| compressed GOP packets | **no** — separate capability request | full | yes |
| **raw JPEG bytes as fetched** | **yes** — already in hand at `process()` entry | **full, original** | **yes** |
| reduced 160x120 luma | yes | 160x120 only | no |

**Proposal: the ring retains the raw JPEG bytes.**

* It is ADR-023-compliant on its own terms — the MJPEG row: *"store the raw JPEG bytes… decode them
  only if the Tier-0 gate triggers."* No decision is reversed.
* It preserves **full original resolution** for the ROI crop and the clip, which a 160x120 luma ring
  cannot.
* It is bounded: a few KB per frame; a 1-3 s window is well under a megabyte at the observed frame
  sizes (to be re-measured on the device in V-0, not assumed).
* It needs **no second poller and no second decode** — the bytes are read where they already exist,
  after authorization, and decoded only on trigger.
* **ADR-023's GOP/keyframe retention rule does not apply to JPEG.** Every JPEG is independently
  decodable, so there is no keyframe to seek back to and no retention-vs-GOP constraint. That
  constraint reapplies only if and when encoded packets become available.

What this does **not** claim: that a JPEG sequence is an encoded video stream. If full-motion
encoded pre-roll is wanted later, that is the separate capability request the acknowledgment
reserves, and this workstream will not assume it.

**Requested from integration:** a borrowed, read-only seam exposing the JPEG bytes at the point they
already exist in `process()`, with the lifetime, dimensions, timestamp, provenance, cancellation and
backpressure terms the acknowledgment requires agreed first. **No decode is added by this
workstream at that seam.**

## 4. Amendment 2 — the submit-advisory adapter, specified

Rather than request an unnamed adapter, this is the concrete request:

* a **public method on `CameraAnalysis`** that accepts one already-validated advisory observation and
  feeds it into the **existing** flow — the same `Pipeline::process(...)` → `AlertPolicy` →
  `PersistenceAudit` path the camera flow already uses, with no new persistence or fusion path;
* the observation carries a **new detector id**, and that id is **not added to the evidence
  allowlist** — it arrives ADVISORY, so no historical scenario changes behaviour and no existing
  detector's evidence changes;
* the caller supplies identity and summary; **provenance and consent context come from the existing
  derivation**, not from this workstream.

## 5. Amendment 3 — who establishes `provenance: real`?

A conflict to resolve before the recorder can be accepted:

* `docs/EVENT_VIDEO_STORAGE.md` requires the reservation's `provenance` to be **exactly `real`**, and
  states synthetic clips are rejected.
* `src/core/Observation.h` marks `Provenance::real` as *"a physical sensor (**NOT used in this
  build; reserved**)"*.

This workstream must not assert `real` — that would be exactly the kind of unsubstantiated provenance
claim the project's own rules forbid. **Requested:** confirmation that `real` is derived by the
existing authenticated acquisition path (and is therefore not this workstream's to set), or a ruling
on how the observer's clip satisfies the field without asserting it.

## 6. Amendment 4 — the recorder encoder, and why it needs no new dependency

`EVENT_VIDEO_STORAGE.md` requires a browser-compatible bounded MP4 (H.264). On the edge node that
would ordinarily mean ffmpeg — which is not in the tree, and "no unapproved OpenCV/ONNX dependency"
sets the spirit for any new dependency.

**Proposal: use the platform's `MediaCodec`** (Android API 30, already present on the target). It
supplies H.264 encoding with no new dependency, no vendored binary and no licence question, which
keeps this workstream clear of the E2-02/E2-05 boundary entirely. The recorder stays in `src/edge/`
and owns nothing shared.

If integration prefers a different encoder path, that is a ruling, not a unilateral choice here.

## 7. What this workstream will not do

Restated because these are the failure modes the acknowledgment is guarding against:

* No second camera poller, credential store, HTTP client or capture loop — ever, including as a
  testing shortcut.
* No second decode of the same frame at the seam.
* No upload or network call from the acquisition callback.
* No second R2 signer, quota authority or cleanup service.
* No change to detector thresholds, evidence allowlists, provenance handling, persistence or fusion.
* No new dependency, and no OpenCV/ONNX adoption as a side effect.
* No edit to any shared file — a focused patch request and then wait.
* No claim of real pre-roll, real provenance, or device acceptance from host tests.

## 8. Gate status

**CLOSED.** No code is written until the explicit MCP recovery/stability handoff is given. Until
then: read-only design, interface specification and the amendments above.
