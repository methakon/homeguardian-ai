# Producer observation schema — producer-side implementation notes

**Component:** Panasonic Android service / producer
**Status:** SUBSIDIARY to the canonical contract. Read that first.

> ## The line shape is NOT defined here
>
> **Canonical contract: `docs/NODE_OBSERVATION_SCHEMA_V1.md`.** It declares itself *"the
> single source of truth for the line shape"*, and it is. The field table, the closed vocabularies,
> the provenance rules, the refusal vocabulary and the versioning policy all live there.
>
> This file previously duplicated some of that and **drifted from the wire**. It asserted *"there is
> no per-line version field"* while the producer was emitting `schema_version`; it showed a
> `source_type` of `camera` after the vocabulary had closed to `motion`; and it documented a
> `provenance` default of `real_device` after that default had been corrected to `synthetic`. Two
> documents disagreeing about one wire format is worse than one document, because each side can cite
> the one that suits it.
>
> **If this file and the canonical contract ever disagree, the canonical contract wins and this file
> is the defect.** Nothing about the line shape may be defined or redefined here.

What remains is only what a producer-side implementer needs and the canonical contract does not
cover: **how the emitter actually behaves, and the constraints that imposes on a receiver.**

---

## 1. What the producer emits

Example of a valid synthetic observation (the native authorized camera/audio
callbacks use the canonical `real` claim):

```json
{"schema_version":1,"source_type":"motion","observation_type":"motion-changed-fraction",
 "value":0.42,"unit":"fraction","state":"ok","confidence":1.0,
 "acquired_at_ms":1761000000000,"provenance":"synthetic"}
```

- `schema_version` is emitted as the **first** field, value `1`.
- Field order is not significant; a receiver must not depend on it.
- The vocabulary in use today is a subset of the canonical closed lists:
  `motion` / `motion-changed-fraction`, `presence` / `audio-rms`, `presence` / `audio-peak`,
  `presence` / `audio-zero-crossing-rate`.
- The `provenance` default for an absent value is **`synthetic`**, never `real_device`. The canonical
  contract §6.2 names the old default a producer-side defect; it is fixed.
- That absent-field Java fallback is separate from the native acquired-observation
  default. Native acquired observations now emit **`real`**, which Java forwards as
  `provenance_claim`; `real_device` remains rejected by the wire decoder. A claim
  never substitutes for authenticated identity, pairing, consent or capture gates.
  Source-derived JVM and real host-decoder regressions are recorded in
  [the provenance checkpoint](PROVENANCE_WIRE_FIX.md). No updated APK installation
  or physical-delivery acceptance is implied.
- One line per observation. A drain may carry **zero or more** lines; zero is normal and means the
  node had nothing to report this tick.

## 2. How the emitter parses — the constraints that follow

These are properties of the producer's field extractor. They are the reason this file still exists:
they are not discoverable from the field list.

### 2.1 String fields carry no escaping

The extractor is a substring scan, not a JSON parser: it reads a value as everything up to the next `"`.

```
{"source_type":"cam\"era", ...}   ->  parses as "cam\"   (truncated)
```

**A receiver must treat an embedded quote as a hard parse failure, not attempt to un-escape it.** The
current values come from closed enumerations, so this cannot bite today — but a future value
containing a quote would silently **truncate** rather than fail, which is the dangerous direction.

### 2.2 Numeric fields are plain decimal

The scanner accepts `[0-9] [+\-] [.] [eE]` only. `NaN`, `Infinity` and hex are never emitted and must
be **refused rather than coerced**.

### 2.3 Absence is `null`/`NaN`, never `""`/`0.0`

Absence must stay distinguishable from a real zero, because a dropped line that reads as `0.0` is a
fabricated measurement. This is the producer-side counterpart to the canonical contract's *"absence is
never real"* rule.

### 2.4 A line is DROPPED locally when a required field is absent

`source_type`, `observation_type` and a finite `value` are required. A line missing any of them is
dropped by the producer and counted in `parse_failures` — **it never reaches the receiver.** A
receiver should therefore expect sequence gaps; see §3.

## 3. Sequencing and bounds

| property | producer behaviour |
|---|---|
| `observation_id` | `<device_id>-<sequence>`, or `node-<sequence>` before the id is known |
| `sequence` | strictly increasing per device, starts at 1, never reused or rewound |
| queue depth | **bounded at 128** |
| overflow policy | **oldest is dropped**, counted separately as `dropped` |

**A gap is lost data, not corruption.** It means a local drop (§2.4) or a bounded-queue overflow.
`dropped` (discarded locally before sending) and a permanent refusal (the peer was told the line is
unusable) are different events and must not be conflated.

## 4. Conformance

`apk/tests/ProducerSchemaConformanceTest.java` pins the producer-side rules in §2 — required-field
rejection, no-escape, plain-decimal, absence-not-zero — and asserts the emitter emits
`schema_version`. Run with:

```bash
bash apk/tests/run_isolated_tests.sh
```

**Cross-language seam rule.** A seam spanning two languages must be verified by counting **both**
sides. A Java compile and a JVM suite both pass while the JNI half is missing, because injected fakes
never load the native library. That happened on this workstream: the producer's `native`
declarations were committed without their JNI definitions and would have thrown
`UnsatisfiedLinkError` on the first pump tick. The real check is `apk/build_apk.sh`, which compares
every Java `native` declaration against the compiled ELF.

## 5. Change policy

Additive by default. An incompatible change bumps the major version **in the canonical contract
first**, then in `kNodeSchemaVersion` on the receiver, then here. A change made only in code is a
defect; a change made only in a document is a wish.

**If you are editing the line shape, edit `docs/NODE_OBSERVATION_SCHEMA_V1.md` — not this file.**
