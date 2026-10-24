# HG-EDGE-VIDEO — implementation plan and overlap boundaries

**Date:** 2026-10-24
**Repository:** `methakon/homeguardian-ai`
**Workstream:** `HG-EDGE-VIDEO`
**Depends on:** `docs/EDGE_VIDEO_EVENT_DETECTION.md` (design), ADR-020 … ADR-024
**Status of this document:** PLAN. No implementation is claimed.

**This is a PLAN. No vision code is written or committed under this workstream**, and nothing in it
is claimed as implemented. It exists so the implementing milestone cannot drift from the project's
architecture, and — the reason it was written — so that it does not rebuild what the repository
already has.

---

## 1. Why this document exists: the overlap it prevents

The design document (`EDGE_VIDEO_EVENT_DETECTION.md`) describes a Tier-0/Tier-1 cascade as though
it were a new build. It is not. Reviewing the repository against that design found that **the
change metric at the centre of Tier-0 already exists, is host-tested, is allocation-free, and has
already been validated on real frames** — as does the specific discriminator the design listed as
a control to be newly built.

Reimplementing any of §3 would be duplicate work, would introduce a second source of truth for the
same measurement, and would risk a divergence between two components that both claim to score
"how much changed".

**Before writing any code under this workstream, read §3 and §4.** If a piece of intended work
appears in §3, it is already done; if it belongs to another workstream per §4, it is not this
workstream's to build.

## 2. Scope and non-scope

**In scope** (this workstream owns these):

1. A Tier-0 **gate and state machine** — the asymmetric trigger, the latch, the first-weak-hit
   timestamp, the candidate/active/cooldown states (ADR-020).
2. A **pre-event ring** retaining compressed packets, with GOP-aware retention (ADR-023).
3. A **bounded event queue** with coalescing, and a sleeping Tier-1 worker.
4. A **thermal controller** that shrinks Tier-1 work and never degrades Tier-0.
5. The **fetch path** from the LAN camera into a luma frame, on the edge node (ADR-022).

**Out of scope** (already owned elsewhere — see §4):

* The change metric and its region-aware discriminators — **already implemented** (§3).
* Cameras, capture plumbing, capture consent and failure-closed behaviour — the existing camera
  and observation path.
* Observation schema, provenance, consent context, persistence, alert rules and fusion — the
  existing pipeline. Tier-0 output enters it; it does not extend it.
* OpenCV/ONNX runtime adoption as a general capability — that is a separate deferred roadmap
  item (§4), not a side effect of this workstream.
* Any Tier-1 model choice or training. Tier-1 is out of scope until Tier-0 is measured.

## 3. What already exists — DO NOT REBUILD

These are implemented, in-tree, and host-tested. The right move is to **call** them.

| what the design proposes | where it already exists |
|---|---|
| The Tier-0 scalar: "changed pixels / pixels" | `MotionFrameFeatures::changed_fraction` — `src/core/BoundedFeatures.h` (`changed_pixels / pixels`), produced by `extract_motion_features()` |
| Per-pixel change threshold | `FeatureBounds::kDefaultPixelDelta = 16` (`src/core/BoundedFeatures.h`) — the design's proposed "~18" is within a rounding of an existing, tested constant |
| "Reject global brightness change / auto-gain response" — listed in the design as a control to build | `RegionChangeFeatures::gain_correlation` — Pearson correlation between the signed delta and each pixel's own previous value; a gain/exposure change approaches +1, a body sits near 0. **Measured on real frames and already used to separate camera auto-gain from movement** (`docs/CHANGE_CHARACTER_SEPARATION.md`) |
| "Compact subject vs whole-scene change" — the design's tree/light discriminator | `RegionChangeFeatures::peak_cell_share` and `spread_radius` — a body is a small normalised radius, a light switch is a large one; scale-free |
| Motion **ROI** — the design proposes computing it with `cv::boundingRect` | `RegionChangeFeatures::centroid_x/centroid_y` (+ `spread_radius`) already give the changed mass and its extent, allocation-free and without OpenCV |
| Direction of travel | `centroid_x/centroid_y` compared between consecutive frames gives travel; a static global change has none |
| Brightness-step polarity | `RegionChangeFeatures::brighter_share`, `darker_share`, `bipolar_balance` |
| Bounded work per call | `FeatureBounds` — `kMaxPixels` (VGA luma plane), `kMinPixels`, `kMaxRegionCells` (64), `kMaxVectorDim`, audio bounds |

Consequence for the design: **Tier-0's decision logic is largely a composition of existing
descriptors, not new image processing.** The new work is the gate/state machine, the ring, the
queue and the fetch path — not the measurement.

The one measured constant to respect: `kMaxPixels` is a **VGA-sized luma plane (640x480)**. Tier-0
at 160x90 or 80x60 is comfortably inside it; a 1080p luma plane is not, and the extractor refuses
an out-of-range plane with a reason rather than sampling it.

## 4. Roadmap reconciliation — who owns what

| roadmap item | status | relationship to this workstream |
|---|---|---|
| **W-02** Bounded precursor detection layer | **DONE** | **Contains the Tier-0 metric and discriminators (§3). This workstream consumes it and adds no new feature extractor.** Any change to the descriptor semantics belongs to whoever owns W-02, not here. |
| **D-05** Real media-derived observations | TODO | **The data path this workstream feeds.** D-05 is the item that takes real media-derived features into the existing pipeline with capture-off/permission/consent/disconnect failing closed. Tier-0 must deliver observations through that contract, not invent a parallel one. Coordinate before building either. |
| **D-02** Android acquisition node | SOFTWARE DONE / **PHYSICAL BLOCKED** | The node-side acquisition contract. This workstream must not re-implement acquisition; physical status remains blocked and is not unblocked by this plan. |
| **D-04** Multiple nodes | TODO | Not this workstream. Tier-0 is single-source until D-04 exists. |
| **E2-02** Camera interface + OpenCV | TODO (Phase E2, deferred) | **Overlaps the design's runtime choice.** Adopting OpenCV as a project capability is E2-02's decision, not this workstream's side effect. Tier-0 as specified needs no OpenCV if it composes §3. |
| **E2-05** ONNX Runtime integration | TODO (Phase E2, deferred) | **Same boundary.** Tier-1 inference requires this; it is a separate decision with its own licence, dependency and build implications. This plan does not adopt a runtime. |
| **E2-03** Face analysis (OpenCV DNN) | TODO (deferred) | Not related. An explicit non-goal here — the design's object classes are person/dog/cat/other, not faces. |

**Standing rule:** if work under this workstream looks like it needs to change W-02's descriptors,
or to adopt OpenCV/ONNX as a capability, **stop and report** rather than absorbing that decision.

## 5. Boundaries — files and components

**This workstream may add** new sources under a Tier-0/event-gate area and its tests, plus the
node-side fetch path. It may **read** `BoundedFeatures`, the observation model, the pipeline and
the detector registry.

**This workstream must not modify:**

* `src/core/BoundedFeatures.*` — owned by W-02; reuse only.
* `src/core/SecurityObservation.*`, `src/core/DetectionFusionRule.*`, the alert persistence path —
  owned by the observation/fusion workstream; Tier-0 output enters through the existing contract.
* `src/core/CameraActivitySession.*`, `src/core/LocalMediaObservation.*` — owned by the existing
  camera path. Read them to understand the contract; do not fork them.
* `src/net/*` — the ingress/transport surface has its own security workstream and FAIL/PASS
  verdicts attached to specific commits.
* Any detector threshold or evidence-allowlist entry. New detector ids are **ADVISORY by default**
  so historical scenarios behave identically; promoting one is a separate, explicit decision.

**Sequencing relative to the owner-ordered queue** (`docs/CURRENT_TASK_HANDOFF.md`): this
workstream does **not** preempt or reorder it. The queue's items — restore both phone MCP tools,
the storage-pressure safeguard, R2 activation, the Alexa-hosted Code update — remain first. This
plan is queued behind them, and no file this workstream touches is on their critical path.

## 6. Staged sequence

Each stage is independently verifiable and stops if its acceptance cannot be met. No stage begins
before the previous stage's evidence exists.

| stage | work | acceptance | explicitly not doing |
|---|---|---|---|
| **V-0** | Measure before building. Time a Tier-0-shaped pass over a real frame pair on the edge node using the **existing** extractor (§3): ms per check, and the same figure in % of one core. | A measured ms/check figure on the device, against the 56 °C binding trip point and the published device baseline. | No gate, no ring, no inference. This stage exists to size the rest. |
| **V-1** | Fetch path: acquire a luma frame from the LAN camera on the node with the existing credential handling; bound the fetch, bound the frame, fail closed on refusal. | Real frames acquired on the node, bounded, with a stated failure reason on each refusal path. | No gate, no queue, no retention. |
| **V-2** | Gate + state machine over the existing descriptors: asymmetric trigger, latch, first-weak-hit timestamp, candidate/active/cooldown. | Gate fires on known motion and stays silent on a quiet scene; the first-weak-hit timestamp is demonstrably earlier than confirmation; false-positive controls are exercised by adversarial fixtures. | No Tier-1, no model, no ring. |
| **V-3** | Pre-event ring (compressed, GOP-aware) + bounded event queue + sleeping worker. | An event's decoded burst begins **before** the trigger, not at it; the queue never exceeds its bound under a motion storm. | No Tier-1 inference. |
| **V-4** | Observations enter the existing pipeline through the D-05 contract, as ADVISORY detector ids. | An observation is accepted, provenance is correct, and no historical scenario changes behaviour. | No threshold change, no allowlist promotion. |
| **V-5** | Tier-1 — **only if still wanted** after V-0…V-4, and only as its own decision (runtime choice, licence, model source). | Separate decision record. | Not planned here. |

Note that V-0 through V-2 deliver a working event detector with **no neural network and no new
dependency at all**. That is the point of §3: the cheap stage was already built.

## 7. Duplicate-work anti-patterns

Recorded so a later session recognises the mistake rather than repeating it:

* **Re-implementing the change fraction.** A new `changed_ratio` alongside
  `MotionFrameFeatures::changed_fraction` creates two sources of truth for the same measurement.
* **Re-building illumination rejection.** `gain_correlation` exists and is measured. A new
  brightness heuristic on top duplicates a validated discriminator with a weaker one.
* **Introducing OpenCV to obtain a bounding box.** The centroid and spread already localise the
  change; adding a dependency to recompute it inverts the cost model this design exists to serve.
* **Defining a parallel observation path.** Tier-0 observations belong in the existing pipeline
  under the D-05 contract; a second path bypasses provenance, consent and fusion.
* **Changing a threshold to make a fixture pass.** Not permitted anywhere in this project. Fix the
  schema or the stimulus, or report BLOCKED with the precise condition.
* **Picking a Tier-1 model before Tier-0 is measured.** The duty cycle that justifies any model is
  an output of V-0.

## 8. Open items inherited from the design

Unchanged by this plan, restated so they are not lost: `EDGE_VIDEO_EVENT_DETECTION.md` §7 — the
mutual-TLS ingestion path remains unavailable on this node until OpenSSL is cross-compiled for
Android (plain-HTTP camera acquisition is unaffected); autonomous restart and on-battery behaviour
are unproven for a continuous monitoring role; the design references were never validated on the
target hardware, so V-0's measurement — not the references — settles the tuning values.
