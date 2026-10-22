# Evidence allowlist, window and fusion

## Who may vote

An ingested observation's detector id is `sensor.<source_type>` (`SecurityObservation.cpp:124`).
`DetectionFusionRule::Config::evidence_detector_ids` defaults to `{"activity-energy"}`
(`DetectionFusionRule.h:50`), so with defaults, ingested evidence never votes. The operator opt-in is naming the
ids:

```json
"evidence_detector_ids": ["sensor.motion", "sensor.presence"]
```

Empty list = allow all (`:79-80`). Adding an id admits a *class of evidence*; it does not lower a gate.

## What counts as a vote

`FusionEngine.cpp:23-38`: usable = `detected`/`not_detected` **with a confidence**. Excluded: `inconclusive`
("inconclusive is not a vote", `:26`), `unavailable`, and **stale**. So three ways to have accepted frames and
zero evidence: wrong state, missing confidence, or timestamps outside the window.

## The window

`Pipeline.h:15` -> default **5000 ms**, applied as `now - observation_timestamp <= window`
(`Pipeline.cpp:28`). Evidence from two sources must land in one 5-second span. Because the rule is evaluated as
events arrive, one locally-favourable window is enough; a whole-run `inconclusive` does not preclude an alert
that already fired.

## The arithmetic, so nobody has to guess

* `min_usable = 2` usable events (`:36`);
* `agreement = max(positive, negative) / total` must clear the rule's agreement threshold;
* fused `confidence >= 0.5` (`:37`).

A tail of `not_detected` frames *lowers* agreement, so a long quiet baseline before a step can dilute the very
evidence you added. Keep the detection-mapped frames together inside one window.

## When no alert fires

Work in this order — it is ordered by how often it is actually the cause:

1. state -> status mapping (§3 of SKILL.md);
2. evidence allowlist;
3. window (both sources inside 5 s, live timestamps);
4. confidence present on the frames;
5. agreement arithmetic inside the window;
6. only then, consider the policy cooldown (`main_ingress.cpp:271`).

**Do not** change any threshold listed above to make a fixture pass.
