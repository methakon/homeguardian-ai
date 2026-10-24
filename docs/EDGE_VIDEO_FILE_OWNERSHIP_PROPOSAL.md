# HG-EDGE-VIDEO — file ownership proposal (pending acknowledgment)

**Date:** 2026-10-24
**Workstream:** `HG-EDGE-VIDEO` (edge-video / observer)
**Status:** PROPOSAL. No implementation begun. No shared file edited.

This document is the ownership proposal required before implementation. It is a **new file** and
touches nothing shared. It is not acknowledged until the integration worker confirms it; until
then this workstream writes no code.

---

## 1. Checkout, branch, HEAD

| item | value |
|---|---|
| checkout | `~/projects/homeguardian-ai/.worktrees/alexa-main` (the existing main checkout) |
| branch | `main` |
| HEAD at time of writing | `e9b06a3` |
| working tree | clean |
| new branch/worktree | **none requested** — none will be created without owner approval |

## 2. Acknowledged boundaries

Acknowledged in full, without qualification:

* **Priority.** The integration worker's phone-to-VPS MCP recovery is first. This workstream does
  **not** preempt it and does not concurrently change the native service, MCP server, outbound
  bridge, Android service lifecycle, APK installation or deployment configuration.
* **Read-only design and review** continues; implementation does not start until both the MCP
  recovery is complete and this proposal is acknowledged.
* **Serialized integration points**, one editor at a time, other worker supplies a focused patch
  or interface request and waits:
  `apk_service/jni/hg_edge_service_jni.cpp`; Android service/lifecycle and native-engine
  bindings; shared `CameraAnalysis`/acquisition wiring; build registrations; shared observation,
  persistence and MCP interfaces.
* **Reuse, do not rebuild** — `BoundedFeatures` metrics and region discriminators;
  `CameraActivitySession` / `LocalMediaObservation`; observation/provenance/consent validation;
  alert persistence and fusion (plan §3-5).
* **No** detector-threshold changes, **no** evidence-allowlist changes, **no** promoting advisory
  detectors, **no** unapproved OpenCV/ONNX dependency.
* **Storage:** no second R2 signer, quota counter or cleanup mechanism. Only genuine
  detection-triggered clips enter the real store.
* **Discipline:** regression tests before fixes; commit only scoped changes; never stage another
  worker's edits, overwrite their files, weaken security checks or force-push. The integration
  worker coordinates deployment; neither worker independently installs an APK or restarts shared
  production services while the other is testing.

## 3. Correction to the plan: the "fetch path" is not a fetch path

The plan's stage V-1 said "acquire a luma frame from the LAN camera on the node with the existing
credential handling". That phrasing invited exactly what the instruction forbids — a second
independent camera poller, credential store or competing capture loop.

**Corrected position.** This workstream builds **no acquisition**. It defines a narrow consumer
interface that the **existing authorized acquisition** satisfies, and consumes frames/packets
from it. Concretely: no new credential handling, no new HTTP client, no second poll loop, no
second frame buffer. If the existing acquisition cannot supply what Tier-0 needs, that is a
**focused interface request to the integration worker**, not a local reimplementation.

V-1 is restated accordingly: *define and test the narrow consumer interface, and demonstrate the
existing acquisition satisfying it* — not *build a fetch path*.

## 4. Files this workstream intends to CREATE (all new)

Every file below is new. No existing file is proposed for modification in this phase.

| file | single responsibility |
|---|---|
| `src/edge/FrameConsumer.h` | narrow interface: obtain a reduced luma frame (no ownership of acquisition, credentials or transport) |
| `src/edge/Tier0Gate.{h,cpp}` | event **state only** — asymmetric trigger, latch, first-weak-hit timestamp, candidate/active/cooldown |
| `src/edge/EventRing.{h,cpp}` | bounded pre-event retention of compressed packets / reduced frames; GOP-aware retention window |
| `src/edge/EventJobQueue.{h,cpp}` | bounded queue (1-2) with coalescing; never grows |
| `src/edge/ThermalWorkController.{h,cpp}` | shrinks Tier-1 work by state; **cannot** reduce the Tier-0 rate |
| `src/edge/EventClipRecorder.{h,cpp}` | bounded MP4 clip production + duration/size/MD5 measurement |
| `src/edge/EventClipUploader.{h,cpp}` | **producer-side** reserve / PUT / complete client with durable retry and local cleanup (see §6 boundary) |
| `tests/edge/test_tier0_gate.cpp` | trigger truth table, latch, first-weak-hit ordering, false-positive adversarial fixtures |
| `tests/edge/test_event_ring.cpp` | retention bound, GOP coverage, wrap behaviour, no unbounded growth |
| `tests/edge/test_event_queue.cpp` | saturation, coalescing, no duplicate jobs, deterministic shutdown |
| `tests/edge/test_clip_recorder.cpp` | size/duration/MD5, cancellation, capture-off and consent-denial paths |
| `tests/edge/test_thermal_controller.cpp` | state transitions and the invariant that Tier-0 rate is never reduced |

**Design constraints applied:** RAII for every owned resource; explicit ownership; bounded queues;
deterministic shutdown; clocks, frame sources and transports injected via interfaces so tests run
without hardware, network or a live gateway. Dependencies point inward — the gate does not know
about R2, the recorder does not know about MCP, the uploader does not know about frame sources.

SOLID is applied where it earns its place, not to claim compliance: the narrow `FrameConsumer`
and the injected clock/transport exist because tests need them; no interface will be introduced
whose only purpose is to demonstrate a principle.

## 5. Shared files — patch requests, not edits

For each of the following this workstream will supply a **focused patch or interface request and
wait for the handoff**, never edit directly:

| shared file / area | what this workstream needs from it |
|---|---|
| `apk_service/jni/hg_edge_service_jni.cpp` | a call site that constructs the observer and pumps it; no logic added here |
| Android service / lifecycle bindings | start/stop hooks for the observer's deterministic shutdown |
| shared `CameraAnalysis` / acquisition wiring | the adapter that satisfies `FrameConsumer` from the existing authorized acquisition |
| build registrations (CMake) | registering the new `src/edge/*` sources and test targets |
| shared observation / persistence / MCP interfaces | entry point for an observer observation to enter the existing pipeline as an ADVISORY detector id |

## 6. Public interfaces needed from the integration worker

1. **Observation entry point.** The existing contract by which an observer observation enters the
   pipeline with correct provenance and consent context, under a **new ADVISORY detector id**
   (no allowlist promotion). Needed before V-4.
2. **Acquisition adapter point.** Where the existing authorized acquisition can satisfy
   `FrameConsumer` — or confirmation that a focused patch in the shared wiring is required, and
   from which side.
3. **Producer endpoint binding.** The gateway base URL and the **provisioning path** for the
   producer's machine bearer credential (not the value). The routes themselves are already
   specified in `docs/EVENT_VIDEO_STORAGE.md`.
4. **Ownership ruling on the producer-side uploader.** See §7 — this is the one genuine ambiguity.
5. **D-05 ownership ruling.** Who owns "real media-derived observations" (`ROADMAP.md` D-05). This
   plan assumed the observation/fusion workstream; if it is assigned to the integration worker,
   the interface in item 1 governs and this workstream does not touch that path.
6. **Native-service stability signal.** Explicit notification that MCP recovery is complete and
   the native service is free to receive additive observer wiring.

## 7. The one ambiguity requiring a ruling

`docs/EVENT_VIDEO_STORAGE.md` assigns to **"the producer"**: requesting the reservation, PUTting
the bytes, calling completion, and owning durable retry/backoff and local cleanup. The integration
instruction assigns to the **integration worker**: "R2 upload authorization, accounting and
retention".

Read together these are consistent — producer-side calls vs gateway-side authorization — but the
boundary is not stated explicitly, and it is precisely where two workers could both build a
client.

**Proposed split, for confirmation:**

* **This workstream (producer side):** the thin client that *consumes* the documented contract —
  reserve, PUT to the returned URL with the returned headers unchanged, complete, with durable
  retry/backoff and local cleanup, and the documented 507 behaviour (stop cloud uploads and
  report; **no** hot-loop retry, **no** discarding the local clip as uploaded, **no** rotating
  event IDs to evade accounting).
* **Integration worker (gateway side):** the routes, signing, quota accounting, the storage-
  pressure guard, retention/cleanup of published clips, portal records and playback.

If the integration worker prefers to own the producer-side client too, this workstream will not
build one and will request the client as an interface instead. **Either is acceptable; both is
not.**

## 8. Overlap identified between the two plans — and the conflicts

Checked against the integration worker's ordered queue (`docs/CURRENT_TASK_HANDOFF.md`) and
`docs/EVENT_VIDEO_STORAGE.md`.

**Dependency, not conflict — must be scheduled:**

1. **Their live acceptance needs our recorder.** `EVENT_VIDEO_STORAGE.md` states live acceptance
   "requires ... **a genuine clip from the recording worker**", and their task 3 says "Coordinate
   genuine clips with the recording worker; do not produce synthetic evidence and present it as
   real." The recording worker is in this workstream. So their task 3's *live* acceptance is
   gated on this workstream's V-3 (clip recorder). Neither worker should improvise a clip, and
   "a signed URL match is not a live R2 PUT/HEAD/GET proof" — so the first genuine clip is a
   joint checkpoint, not either side's private milestone.

**Real conflicts to resolve:**

2. **Native service, serialized.** Their task 1 repairs MCP inside the native service and the
   Android lifecycle; this workstream needs additive observer wiring in the same service. The
   instruction already serializes this. **This workstream will not touch it until they report
   MCP recovery complete.**
3. **Gateway behaviour changes underneath us.** Their task 2 introduces the storage-pressure
   guard (bound reservations strictly below 80%, delete oldest published, refuse when removal is
   unconfirmed). That makes `HTTP 507 event_video_storage_full` a *designed* outcome, not an
   anomaly. The producer must implement the documented 507 behaviour. **Shared contract, not a
   shared file** — but the behaviour must be agreed before either side tests it, or one worker
   will read the other's correct refusal as a bug.
4. **The first genuine clip is a contested resource.** Their task 3 wants it for validation; our
   V-3 produces it. If both sides race to produce "the first real clip", one will be synthetic or
   duplicated. Propose: this workstream produces it once, on request, through the agreed
   interface, and both sides read the same result.
5. **D-05 ownership** (§6 item 5) — unresolved and must be ruled on, because both the observation
   path and observer output live near it.
6. **Both instructed not to touch the same list.** The serialized integration points are already
   named identically by both sides, so they agree; the only additions this proposal makes are
   `FrameConsumer.h` and the `src/edge/*` sources, none of which exist today and none of which is
   on the integration worker's critical path.

## 9. Tests and acceptance criteria

**Host tests (hermetic, no hardware, no network, no live gateway) — required before any device
work:**

* Gate truth table: one strong hit fires; two weak hits in three fire; one weak hit does not;
  the latch holds past the last hit and further hits extend the same event.
* `first_weak_hit` timestamp is strictly earlier than confirmation time — the property the whole
  retrospective design depends on.
* Adversarial false-positive fixtures: uniform brightness step (auto-gain), a compact body, a
  slow object producing only edge change. The first must not fire; the second must.
* Ring: retention never falls below the GOP; the decoded burst begins **before** the trigger.
* Queue: bounded under a motion storm; overlapping events coalesce rather than enqueueing
  duplicates; `stop()` returns promptly and no thread is left running.
* Recorder: size/duration/MD5 correct; cancellation mid-clip yields no partial published clip;
  capture-off and consent-denial produce no clip at all.
* Thermal controller: every state transition; and the invariant that **no state reduces the
  Tier-0 rate**.
* Uploader: idempotent retry reusing the same `event_id`; 507 stops and reports without hot-loop
  retry, without discarding the local clip as uploaded, and without rotating the ID.

**Device acceptance (after V-0 measurement):**

* Measured ms/check for the existing extractor on the edge node, in % of one core, against the
  56 °C binding trip point from `docs/EDGE_VIDEO_EVENT_DETECTION.md` §5.2.
* Real events produce a clip; a quiet scene produces none.
* 30-60 min soak logging CPU, temperature and **queue depth**; queue depth must not climb.

**Explicitly separated, per the instruction:** source implementation, host tests, phone execution
and end-to-end acceptance will be reported as four distinct states. A green host suite is not
device acceptance, and device execution is not end-to-end acceptance with a live gateway.

## 10. What will be reported on completion of each stage

Source SHA; the actual test command and its result; remaining limitations; and the four-state
separation in §9. No stage will be reported as done on a plan, a build, or an active process.
