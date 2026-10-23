# Contract coordination with the Ubuntu ARM64 runtime

**Status:** an interface note. **Nothing was changed in the runtime producer's branch, worktree or files**, no PR was
opened, and no merge is proposed. This document is the whole of the coordination: it states what a
producer on the Ubuntu runtime must emit for this branch's consumer to accept it as evidence, and what it
can consume if it wants the Linux adapters.

## What this branch already has (usable as-is)

| component | path | role |
|---|---|---|
| `SensorEventConsumer` | `src/core/SensorEventConsumer.*` | validates an incoming observation event; produces the refusal codes below |
| `AlsaCommandAudioSource` | `src/backend/linux/` | microphone adapter: bounded windows, PCM over a pipe, nothing stored |
| `V4l2CameraDevice` | `src/backend/linux/V4l2CameraDevice.*` | V4L2 camera adapter, no external media library |
| `Observation`, `FusionEngine` | `src/core/` | the normalized type and the engine that consumes it |
| contract | `docs/SENSOR_EVENT_CONTRACT.md` | the field table and the producer rules |

The engine consumes **normalized observations**, never frames or samples. Media never crosses into the
control plane, and the JSON below is the only thing a producer has to get right.

## What a producer must emit

```json
{
  "type": "inference",
  "schema_version": 1,
  "observation_id": "…unique per observation…",
  "source": "android:camera2 | device:/dev/video0 | alsa:default | <your prefix>",
  "provenance": "real | synthetic",
  "status": "detected | not_detected | inconclusive | unavailable",
  "detector_id": "…", "model_id": "…",
  "observed_at": 1800000000.123,
  "confidence": 0.0,
  "consent": "granted",
  "features": { "…": "bounded scalars only" }
}
```

Rules that are enforced, not advisory:

1. `provenance` is `real` or `synthetic`. **`mixed` is refused**: mixed evidence is a property of a set and
   is derived by the fusion engine from its members, never asserted by a producer.
2. `observed_at` is **unix seconds as a number** and must not go backwards within a session: an older
   timestamp is refused as out-of-order or a replay, and one beyond the clock-skew allowance (default 5 s)
   is refused as future.
3. `observation_id` must be unique per observation. A repeat is refused, so replaying a stream cannot
   manufacture a run of observations where none occurred.
4. `consent` must be `"granted"` while the consumer requires consent. Revocation stops capture: the source
   is marked unavailable and nothing enters the window. **Omitting the field is not consent.**
5. `status: "unavailable"` means *no evidence*. A disconnected sensor is a gap, never a quiet scene.
6. `source` must be non-empty and must match a prefix the consumer is configured to trust. **Tell me the
   prefix your runtime will use** and it gets registered; until then, an unknown source is refused rather
   than trusted.

Refusals are closed codes (`IngestCode`): `not_an_event`, `unsupported_schema`, `malformed`,
`source_identity`, `provenance_invalid`, `provenance_conflict`, `future_timestamp`, `stale_or_replay`,
`duplicate`, `consent_missing`, `source_offline`. `provenance_conflict` is the one to know about: a source
that identifies itself as a fixture cannot claim `real` evidence, whatever its fields say.

## What this branch would need from the Ubuntu runtime

Nothing to compile against, and no shared header: the boundary is JSON. Two things would make a real
integration possible without either side changing the other:

1. **A producer**: the runtime emits the JSON above over a pipe or a socket. The consumer validates it and
   the engine fuses what survives.
2. **The prefix and the clock**: the `source` prefix it will use, and confirmation that its timestamps are
   unix seconds from a synchronised clock. Drift is measured against the harness clock, and a producer
   whose clock drifts silently degrades evidence into gaps.

If the runtime would rather consume than produce, the two Linux adapters are the reference implementations
of `IAudioObservationSource` and the camera path, and `docs/SENSOR_EVENT_CONTRACT.md` defines the
normalized observations they hand the engine.

## What is explicitly out of scope

No V380 media integration (still blocked), no Android camera API in core, no media in the control plane, no
authentication claims (the consumer authenticates nothing: it validates), and no species or disaster claim
of any kind. Nothing in this document changes the runtime producer implementation, and no coordination is assumed to have
happened until confirmed.
