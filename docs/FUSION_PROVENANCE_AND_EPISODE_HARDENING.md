# Fusion provenance and activity-episode hardening

**Isolated branch:** `experiment/hg-v380-09-secure-camera-integration`. **Not merged into PR #20.**
Nothing here alters the release's default behaviour; Task 3 required the change to stay isolated, and
the one behavioural switch added is **off by default**.

The two gaps this closes were found by the evidence audit and the recorded-scalar replay:

1. `DetectionFusionRule` stamped a fixed `"provenance": "synthetic"` into every alert. True while the
   simulator was the only source; false the moment a physical observation joins the window.
2. A short sustained change could not be reported at all. In the recorded consented run, six
   consecutive confirmed rises lost to their own window: 47 confirmed changes against 68 baseline
   observations put agreement at 0.5913 against the engine's 0.6 threshold, so the fusion returned
   `inconclusive` and no alert was reachable from evidence the detector had confirmed six times.

---

## 1. Task 1 — provenance derived, never asserted

`FusionEngine` now takes the provenance of the evidence from the votes it actually fused:

```cpp
enum class EvidenceProvenance { real, synthetic, mixed, uncertain };
EvidenceProvenance derive_evidence_provenance(size_t real_inputs, size_t synthetic_inputs);
```

`FusionResult` gained `provenance`, `real_inputs` and `synthetic_inputs`; the default for a
default-constructed result is **`uncertain`**, so a result that was never computed cannot be read as
real evidence. The rule of the function is one line: both kinds present → `mixed`; real only →
`real`; synthetic only → `synthetic`; nothing → `uncertain`. There is no path from synthetic
evidence to a `real` verdict, and a `real` observation that is stale or otherwise unusable does not
count, because only votes are counted.

`DetectionFusionRule` builds every alert in one place (`build_alert`), so the message and the fields
cannot drift apart:

| evidence | `details["provenance"]` | message prefix |
|---|---|---|
| synthetic only | `synthetic` | `synthetic activity detected: fusion confidence …` *(byte-identical to before)* |
| real only | `real` | `real-sensor activity detected: …` |
| mixed | `mixed` | `mixed-provenance activity detected: …` |
| nothing usable | — (no alert) | — |

Additive fields: `provenance_sources {real, synthetic}`, `provenance_derived: true`,
`evidence_mode: "fusion" | "episode"`. The all-synthetic wording and the `provenance` key's position
and type are unchanged, so every existing assertion and quoted log line in the documentation stays
true — including `docs/DEMO_SIMULATOR.md` and `docs/FRICTION_LOG.md`.

## 2. Task 2 — a bounded, opt-in activity episode

`src/core/ActivityEpisode.{h,cpp}` selects the strongest run of consecutive confirmed changes:

* a run shorter than `min_span` (3) is not an episode — "one anomalous frame is a door slamming";
* a run of confirmed **falls** is not an activity episode when `require_rising` is set (the default):
  the recorded still-scene run's only confirmed changes are falls, and calling that "activity" would
  be a false positive invented by the improvement itself;
* a **direction change closes the run** — the rises and the settling falls are two episodes, not one
  long mixed one (this was a real bug in the first implementation; the recorded-run test caught it);
* the confidence gate is `min_mean_confidence = 0.5`, the same gate the rule already used. **No
  threshold was lowered**, and a test asserts that all three thresholds are untouched
  (`min_confidence == 0.5`, `min_mean_confidence == 0.5`, `agreement_threshold == 0.6`).

`DetectionFusionRule::Config::use_episode_evidence` (**default `false`**) selects whether an episode
may qualify. When it is on and no episode exists, the rule falls through to the classic decision and
refuses exactly as before.

## 3. Root cause

Two independent causes, and neither is "the threshold is too high":

1. **A constant where a derivation belonged.** Provenance was a literal in the alert builder. Nothing
   in the code could notice when the input stopped matching it.
2. **A vote model that cannot represent a short event.** `FusionEngine::fuse` treats every usable
   observation in the window as an independent vote and requires `max(positive, negative) / total ≥
   0.6`. A ~2-second event at ~2.8 observations/second inside a 60-second window is ~6 evidence votes
   against ~109 others, so agreement is arithmetically decided by the window, not by the event. The
   recorded run measured 0.5913 — a 0.009 miss. Lowering `agreement_threshold` would have "fixed" this
   one case while weakening every other decision, which is why the change selects better evidence
   instead of relaxing a gate.

## 4. Before/after fusion results (recorded scalars, replay-labelled `synthetic`)

| scenario | before: `FusionEngine` + rule (default) | after: episode evidence enabled |
|---|---|---|
| six recorded rises, alone | detected, **0.6965**, contributing 6 → **alert** | episode 6 @ 0.6965 → alert |
| + 1 quiet observation | detected, **0.5117** → alert (margin 0.012) | episode 6 @ 0.6965 → alert |
| + 2 quiet observations | detected, **0.3918** → **no alert** | episode 6 @ 0.6965 → alert |
| the recorded 600-frame run (47 detected / 68 baseline / 3 inconclusive) | **inconclusive**, **0.0884** (0.0415 with the more forgiving settling assumption) → **no alert** | episode 6 @ 0.6965 → **alert**, with `fusion_verdict: inconclusive` retained alongside |
| recorded still-scene run (5 confirmed falls) | not_detected, **0.0237** → no alert | **no episode** → no alert |
| brief isolated spike (1–2 confirmed changes) | not_detected → no alert | **no episode** (< `min_span`) → no alert |

The classic confidence figure depends on what the settling observations are assumed to carry; with the
recorded fall confidences it is 0.0884, with a forgiving 0.10 it is 0.0415. Both are far below the gate
and both are `inconclusive`, so the conclusion does not rest on that assumption — which is why the
tests assert the range.

**This is a replay of recorded scalars, not a physical end-to-end result.** No test here claims that a
real camera produced a delivered alert.

## 5. False-positive analysis

What the episode path newly admits: a run of ≥3 consecutive confirmed **rising** changes whose mean
confidence is ≥0.5, anywhere in the window, regardless of how much quiet baseline surrounds it.

| risk | assessment | evidence |
|---|---|---|
| still scene alerts | refused twice over: no rising run exists at all, and the recorded falls' mean (0.329) is below the gate | `still-scene` tests, both modes |
| a run of falls read as activity | refused by `require_rising`; asserted separately with high-confidence falls (0.8), which the gate would otherwise pass | `strong_falls` test |
| a brief spike | refused: 1 and 2 confirmed changes are below `min_span`; 3 is the boundary and is asserted in both directions | `spike` tests |
| **a sustained environmental shift** (lighting step, blanking) confirmed for ≥3 windows at mean ≥0.5 | **newly reachable** — this is the honest cost of the change: a slow, sustained, high-confidence change in view now alerts where the classic path would have diluted it away | not measured on hardware; the direction is *rising*, so the falling-scene guard does not apply |
| severity inflation | none: severity still comes from the confidence, and the recorded peak (0.6965) stays `info` (warning band ≥0.8 untouched) | before/after test |

## 6. Regression tests and results

| suite | command | result |
|---|---|---|
| new provenance regressions | `./build/homeguardian_tests "[fusion][provenance]"` | **9 cases / 50 assertions, passing** |
| new episode regressions | `./build/homeguardian_tests "[episode]"` | **6 cases / 59 assertions, passing** |
| recorded-scalar replay (existing) | `./build/homeguardian_tests "[camera][replay]"` | 5 cases / 253 assertions, passing |
| camera suite (existing) | `./build/homeguardian_tests "[camera]"` | 22 cases / 217 assertions, passing |
| evidence validator (existing) | `ctest -R evidence_provenance` | 2/2 passing |
| **full suite** | `HG_SOURCE_DIR=$PWD ./build/homeguardian_tests` | see the commit message — all passing |

Provenance cases covered: all-real, all-synthetic, mixed, none-usable, stale-real-beside-synthetic
(which must stay `synthetic`), the one-way door (synthetic evidence can never yield `real`), the
default-constructed result, and the payload label surviving into the event unchanged.

## 7. Impact on production code and schemas

| change | kind | release risk |
|---|---|---|
| `EvidenceProvenance` + `derive_evidence_provenance` + 3 `FusionResult` fields | additive to a header | none: existing fields and semantics untouched |
| fire-and-forget `DetectionFusionRule` message/detail wording for **non-synthetic** evidence | behavioural fix | none for the current build (all inputs synthetic); removes a false statement for a future one |
| `DetectionFusionRule::Config::use_episode_evidence` (**default false**) + `episode` config | additive, opt-in | none while off — the default path is byte-for-byte the old decision |
| `src/core/ActivityEpisode.{h,cpp}` + CMake entries | new files | none: not instantiated unless the flag is on |
| alert `details` keys `provenance_sources`, `provenance_derived`, `evidence_mode`, `episode` | additive JSON | safe: no consumer enumerates details exhaustively; no schema migration, no column change, no validator change |
| event/observation schema | **unchanged** | the `provenance` string, its position and its values are exactly as before |

No persistence schema, migration, event schema or validator was touched.

## 8. Recommendation

**Safe to keep in the release as an inert, documented capability; NOT safe to enable by default yet.**

- The provenance fix (§1) should be merged wherever the camera path goes: it is a correctness fix with
  no behaviour change in the current build.
- The episode path (§2) should be treated as an **opt-in experiment**. It restores the ability to
  report a short sustained change without lowering any threshold, and it refuses every still-scene and
  spike case measured — but it introduces the environmental-shift risk in §5 that has not been measured
  on hardware, and enabling it would change alert reachability for the release's synthetic scenarios
  too (the demo's short events would alert where they were previously diluted).
- Before any enablement: run the demo scenarios with the flag on and off and compare alert counts, and
  measure an environmental step (a lighting change with a still subject) to bound the risk in §5.
- **No physical delivered alert is claimed by this work**, and nothing here may be presented as one.
