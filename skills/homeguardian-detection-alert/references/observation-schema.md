# Observation schema: valid vs invalid

## Frame shape (this transport)

4-byte big-endian length prefix + UTF-8 JSON body <= 4096 bytes (`src/net/DeviceProtocol.h:29,49`). `hello`
first, then `observation` frames on the same mTLS connection.

```
{"protocol_version_major":1,"protocol_version_minor":0,"kind":"observation",
 "observation_id":"<unique>","device_id":"<must equal the authenticated peer>",
 "source_type":"motion","observation_type":"motion-changed-fraction",
 "acquisition_mode":"synthetic","acquired_at_ms":<LIVE epoch ms>,"sequence":<increasing>,
 "features":{"motion-changed-fraction":0.9,"state":"on"},"unit":"fraction",
 "confidence":0.9,"provenance_claim":"synthetic"}
```

Omit `schema_version` on this transport: it exists in the NODE contract, and present-and-0 is refused there.

## The three questions to ask of any frame

1. Is `source_type` a real enum value? (No coercion; unknown is refused.)
2. Is `observation_type` in `ObservationIngest.h:51-53`?
3. Does `state` map to `detected`/`not_detected` for **that** source_type (`SecurityObservation.cpp:75-110`)?
   If it maps to `inconclusive`, the frame is accepted and audited and **casts no vote**.

## Rejection vocabulary you will actually see

| symptom | meaning |
|---|---|
| `unknown_source_type` | `source_type` is not an enum value (refusal, not a throw) |
| `malformed_timestamp` | `acquired_at_ms` is pinned/stale or in the future |
| `replayed_sequence` | `sequence` did not increase |
| `acks=0, accepted=0` | revoked/unknown device, or consent/capture not enabled |

## States that are easy to get wrong

* motion: `on` is a detection; `present` is NOT (inconclusive).
* presence: `present` is a detection; `on`/`occupied` also are.
* availability: the *problem* states are the detections; a healthy report is a negative vote.
