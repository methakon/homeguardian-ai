# Offline animal-feature replay: implementation and limits

This utility implements an isolated, testable part of the local
`animal-calamity-precursor-detection` skill: explicit quiet baselines, sustained
cosine-vector drift and experimental Dempster-Shafer fusion. It does **not**
implement all of that skill or the `animal-language-recognition` model catalog.

## Scope and evidence

The implementation is `scripts/animal_feature_replay.py` (`Replay.observe`,
`combine`); synthetic regressions are in `tests/test_animal_feature_replay.py`.
The CTest name is `animal_feature_replay`, registered when tests are enabled and
Python 3.8+ is available. No third-party Python packages are required.

| Skill capability | Status for this utility | Evidence / remaining work |
|---|---|---|
| Quiet baseline and cosine embedding drift | Implemented offline | Unit vectors, frozen per-source centroid, consecutive-window gate in `Replay.observe` |
| Missing/stale evidence handling | Implemented offline | Absent sources and excessive timestamp gaps reset streaks; only current sources enter fusion |
| Ignorance and conflict-aware fusion | Implemented experimentally | Pairwise K is recorded; replay accumulates joint K and returns ignorance for joint K >= 0.9; disagreement also vetoes `activity_change` |
| Bounded inputs/state, invalid-input rollback | Implemented offline | Input/schema limits and copy-before-commit; synthetic validation regressions |
| YAMNet/log-mel or other media embeddings | Not implemented here | Caller must supply vectors; no audio decoder, model loading or inference |
| Optical flow / YOLO / swarm kinematics | Not implemented here | `vision` is only a source name in fixtures, not camera inference |
| Species, call type, distress, emotion or animal-language classification | Not implemented here | No DogSpeak, Meow_Net, BirdNET, NatureLM or other classifier is loaded |
| Environmental correlation / sensor liveness | Not implemented here | A timestamp gap resets a streak; it is not a liveness monitor or environmental model |
| MQTT / Home Assistant / webhook / Alexa delivery | Not implemented here | Decisions go only to stdout; no network, tokens, subscription or delivery code |
| Durable decisions and restart recovery | Not implemented here | State is memory-only and a new process needs a new explicit baseline |
| C++ hub / Android / MCP integration | Not implemented here | Standalone offline Python utility, not an ingest adapter or alert-fusion rule |
| Animal-data evaluation / real-world acceptance | Unverified | Regression fixtures are synthetic; thresholds and masses are uncalibrated |
| Earthquake, storm or other disaster prediction | Deliberately excluded | Hypothesis frame contains only `routine` and `activity_change` |

Branch boundaries matter. On `experiment/hg-v380-ec39-integration`,
`src/core/SyntheticActivityDetector.cpp` is a synthetic energy-threshold detector,
not a species classifier. Separately, the inspected
`release/hackathon-2026-mcp-candidate` tree has C++ precursor components, but
`src/net/IngestDetectors.h` explicitly leaves vector/full-suite fusion unwired
because its ingress contains scalar observations. This offline utility does not
close that runtime integration gap. No branch checkout or merge is needed to use it.

## Input contract

One JSON object per line, with exactly these fields:

```json
{"timestamp_ms":0,"phase":"baseline","sources":{"audio":[1,0],"vision":[1,0]}}
```

- `timestamp_ms`: strictly increasing integer in [0, 2^53], shared across the
  whole replay. Booleans, repeated timestamps and backward timestamps are rejected.
- `phase`: `baseline` for explicitly selected quiet windows; `observe` otherwise.
  The utility cannot establish that an input is quiet or that an animal is present.
- `sources`: 1–8 named vectors per record; at most 8 distinct ids per process.
  Ids contain 1–48 ASCII letters, digits, `_` or `-`.
- Each vector contains 2–1024 finite numeric components in [-1,1], with Euclidean
  norm strictly greater than 1e-12. Dimension is fixed per source. Components are
  normalized before use.
  The producer must supply compatible features from the same model, version and
  preprocessing throughout the run; matching dimensions alone do not prove this.
- Every observed source needs an explicit baseline. With at least two supplied
  sources, an incomplete baseline yields `insufficient_baseline`, not `routine`.
  A single supplied source yields `insufficient_sources` first, even if its
  baseline is incomplete. Once enough quiet samples are accepted, its centroid
  is frozen; retraining requires starting a new replay.
- JSON duplicate keys, NaN/Infinity and records exceeding 128 KiB are rejected.
  Invalid records do not commit timestamps, baselines or streak updates. The CLI
  stops at the first invalid record with exit code 2; earlier stdout remains valid.

Never feed arbitrary feature vectors as proof of an animal observation. Source ids
are caller assertions, not authenticated devices or evidence of independence.
Repeated or correlated features can inflate fusion belief; there is no automated
independence check. Permission and privacy review belong upstream of any real
feature extraction. The utility neither captures media nor uploads data.

## Algorithm and output

Defaults are 8 quiet samples, 3 consecutive drift windows, cosine distance >= 0.35,
and maximum inter-record gap of 5000 ms. Override using `--baseline-samples`,
`--sustained-windows`, `--distance-threshold`, `--max-gap-ms`. These are experimental
parameters, **not measured animal-detection performance**.

The baseline centroid averages normalized quiet vectors. A cancelling zero centroid
is rejected. Above-threshold observations must persist for the configured number of
windows. A missing source, baseline record, normal observation or excessive gap
breaks the corresponding streak. Baseline samples themselves need not be contiguous.

A ready source assigns mass 0.75 to routine or sustained change and 0.25 to ignorance.
Pending, not-yet-sustained drift contributes pure ignorance. These masses are
heuristics, not calibrated probabilities. Sources are folded in sorted-id order.
`conflict_steps` records each pairwise normalization conflict, including the first
fold against ignorance; `conflict_k` is the joint conflict accumulated as
`K_new = K_old + (1 - K_old) * K_step`. The replay evaluates all current-source
masses and suppresses fusion when joint K >= 0.9, returning pure ignorance and
zero change belief. The pairwise combiner also suppresses a single step at K >= 0.9;
in that case no further normalization is attempted. Multiple contradictory sources
can cross the joint threshold even when no individual step does. Disagreement is
also separately vetoed below that threshold. K is a mass-conflict diagnostic, not
a calibrated probability of a real-world event. No claim is made that this
implements a calibrated precursor fusion system or disfusion analysis.

`activity_change` is true only when at least two currently supplied sources all
have complete baselines and sustained drift, with no suppressed fusion. It means
feature directions changed, not that animals are distressed or a disaster is likely.

Every output includes:

- `provenance: offline_feature_replay`
- `severity: info`, `alert_eligible: false`
- `parameters_validated_on_animals: false`
- Status, source readiness/drift/streaks, experimental belief, ignorance and conflict

No species, seismic, weather-prediction or critical-alert hypothesis is representable.
No result is passed into the production alert pipeline.

## Reproduction

From the repository root:

```sh
python3 -B -m unittest discover -s tests -p test_animal_feature_replay.py -v
cmake -S . -B hg_build
ctest --test-dir hg_build -R '^animal_feature_replay$' --output-on-failure
python3 -B scripts/animal_feature_replay.py --help
```

For an existing, permission-reviewed feature file (no capture is performed):

```sh
python3 -B scripts/animal_feature_replay.py < features.jsonl > decisions.jsonl
```

The CLI end-to-end regression generates synthetic quiet/change vectors, invokes a
real subprocess and checks one output per input plus the non-alert boundary. Other
regressions cover spikes, missing sources, gaps, disagreement, incomplete/frozen
baselines, cancellation, timestamp/schema/vector validation, transactional rejection,
conflict, normalization, duplicate JSON keys and record size. Passing these does not
validate real animal recognition, hardware capture, delivery or C++ runtime wiring.

## Scientific correction affecting future extraction

A 16 kHz waveform has an 8 kHz Nyquist limit. Neither YAMNet's embedding nor its
classifier can recover 22/50 kHz rodent ultrasonic calls after such downsampling.
That requires appropriately verified high-bandwidth capture and a separate
high-sample-rate extraction/model path. Do not implement the skill's ultrasonic
examples by reusing the 16 kHz YAMNet route. Vector drift also does not establish
species identity, distress, environmental causality or predictive lead time.
