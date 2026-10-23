# The staged-scenario harness

Branch `experiment/hg-v380-09-secure-camera-integration` only; nothing here is in PR #20.

## Why it exists

Seven staged physical scenarios remain unrun, each needing a person in the room: a room light switched on
and off, a small object moving across the field, a person crossing with consent, a moving shadow, a
reflective surface, camera vibration, and curtains or vegetation. Running them by hand produced a problem
larger than the missing runs: **nothing recorded whether anyone was in the scene**, and the same controlled
illumination experiment gave 19 motion-labelled observations once and 61 another time with no way to tell
whether that was the detector or a person at the workstation.

So the harness does two things: it drives the scenarios one at a time, and it refuses to treat a run as
evidence unless a human said what the scene was.

## Running it

```
hg_scenario_runner --instructions                      # the operator procedure, printed
hg_scenario_runner --only light_on_off --interactive   # prompts, then captures
hg_scenario_runner --occupancy empty --confirmed \
    --deliberate "light on at t=8s, off at t=20s"      # one scenario, annotated
```

Captures go through `hg_camera_bridge --capture --confirm`, unchanged. **No frame is stored and no audio is
recorded**: the bridge reduces frames to scalars and retains nothing, and the harness only reads those
scalars. Each run writes `scenario-<id>-<utc>.log` and a `scenario-records-<utc>.json` carrying the
annotation alongside the numbers, so a later reader cannot separate them.

## The annotation record

| field | meaning |
|---|---|
| `occupancy` | `empty`, `person_present` or `unknown`. **The default is `unknown`**, so an absent annotation can never read as an empty room. |
| `annotation_confirmed` | the operator asserted it for *this* run. Separate from the state on purpose. |
| `deliberate_change` | what the operator actually did, in their words, with timings where they know them |

**Without a confirmed annotation, a run is reported and marked NOT ATTRIBUTABLE**, with the reason spelled
out: movement cannot be separated from the camera's own gain response, so the run supports no
false-positive or false-negative rate. A confirmed-unknown scene is attributable for reporting and carries
an explicit advisory that it cannot attribute movement.

## Scenario classes are never pooled

`ambient_none`, `lighting`, `object_motion`, `person_motion`, `camera_vibration`, `occlusion`, `occupancy`.
A lighting run contributes to nothing but lighting. The report groups by class, and **withholds a class's
rate entirely unless every run in that class is attributable** - so a bad run cannot be averaged away by a
good one.

## Frame characters and fusion alerts are reported separately

The report gives, per class: observations, motion-labelled observations, and fusion alerts. A scene can be
full of labelled movement and produce no alert; that is a result, not a contradiction, and merging the two
numbers would hide it. The alert line comes from the existing detection-fusion rule fed the capture's own
observations - no threshold is changed and no detector is added.

## What makes a run unattributable

| blocker | meaning |
|---|---|
| the capture did not finish | interrupted run; not a result |
| incomplete capture (N of M frames, < 95%) | too little of the run to conclude from |
| N frame(s) were dropped by the driver | reported, and it bounds how much of the scene was seen |
| no operator occupancy annotation | movement is indistinguishable from gain artifacts |
| the scene was confirmed as UNKNOWN | reported with an advisory; not used to attribute movement |
| the harness clock went backwards | the timing of the run cannot be trusted |
| the pipeline's timestamp differs from the harness by > 5 s | the two clocks disagree |
| the log carried no pipeline timestamp | a gap, not a pass |

Each of these is pinned by a regression test (`tests/core/test_scenario_record.cpp`), together with class
and occupancy round-tripping and the rule that a rate is withheld from a class with an unattributable run.

## Real results so far

Three consecutive runs of `ambient_baseline` - **no deliberate stimulus, occupancy not controlled or
observed** - with the current code:

| run | frames | observations | motion-labelled | fusion |
|---|---|---|---|---|
| 1 | 600/600 | 120 | 6 (5.0%) | no alert |
| 2 | 600/600 | 120 | 37 (30.8%) | no alert |
| 3 | 600/600 | 120 | 9 (7.5%) | no alert |

All three are marked NOT ATTRIBUTABLE for the reason above, so **no rate is claimed from them**. What they
do establish, and the reason the annotation layer matters:

- **The frame-character labels are not reproducible across runs on this scene** - 6, 37 and 9 of 120, a
  six-fold spread, with no deliberate change and no change in the code. A false-positive rate computed
  from any one of these runs would be wrong by a factor of six.
- **The fusion alert outcome was stable across all three: no alert.** The conservative rule did not fire on
  an uncontrolled scene, which is the behaviour that matters for a device left running.
- The character layer and the alert layer therefore disagree about how alarming this scene is, and the
  alert layer is the one that is consistent. That is an argument for keeping the alert path as it is and
  continuing to treat the character labels as a diagnostic.

One disclosure: the **first** ambient run was written to a log whose name did not include a run stamp, and
the second run overwrote it. Its numbers survive above and in the runner's output, but its log does not.
The harness now stamps every log and record with a UTC run time, so a repeat run cannot destroy an earlier
one - which is the failure this harness was built to stop happening to other people's evidence.

## Still unrun, and not claimed

All seven staged scenarios. They need an operator in the designated non-private area; none has been run,
and no result from them is reported here. `require_motion_character` and the episode-alert path remain
disabled by default.
