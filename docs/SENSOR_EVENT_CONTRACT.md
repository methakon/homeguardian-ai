# Sensor event contract — one shape for every platform

**Status:** the contract is what the core already produces and consumes; this document names it, proves
it round-trips, and states what is deliberately outside it. Branch
`experiment/hg-v380-09-secure-camera-integration` only; nothing here is in PR #20.

## Why it exists

The HomeGuardian core must stay platform-neutral. An Ubuntu ARM64 build hosting the MCP engine has no
Android camera API, no V4L2 device and no way to know how a given sensor reached it. If the engine
learned about Android, the Linux port would be a second implementation of the same reasoning, and the
two would drift. So the engine consumes **observations**, and a platform adapter's only job is to
produce them.

## The contract

An observation carries, in JSON (`Observation::to_json`, schema_version 1):

| field | meaning | who sets it |
|---|---|---|
| `type` | `inference` — a measured thing, not a command | producer |
| `source` | `device:/dev/video0`, `alsa:default`, `android:audiorecord`, `node:kitchen` | producer |
| `provenance` | `real` \| `synthetic` \| `mixed` — **stated by the producer, never inferred** | producer |
| `status` | `detected` \| `not_detected` \| `unavailable` \| `inconclusive` \| `refused` | detector |
| `confidence` | bounded `[0,1]` for this observation alone | detector |
| `detector_id`, `model_id` | which rule and which version produced it, so a verdict is explainable | detector |
| `features` | bounded scalars only (motion fraction, region spread, polarities, audio RMS/peak/ZCR) | detector |
| `uncertainty` | what the observation explicitly does **not** mean | detector |

Rules a producer must follow:

1. **Provenance is a claim about where the data came from.** A fixture is `synthetic` even when it
   models a real event. A capture is `real` only when it came off a sensor in this session. Mixed inputs
   are `mixed` and never upgraded.
2. **Refusal is an answer.** `status: unavailable` or `refused` with a reason is preferred to a
   plausible-looking number; a missing window is not evidence of quiet.
3. **`features` is bounded and closed-form.** No field may hold arbitrary payloads, raw media, frames,
   audio samples or identifiers of people, and no field names a species, a word, a person or an event
   class. The vocabulary is enumerated in code, not free text.
4. **Confidence is per observation.** Fusion decides whether a run of observations is a verdict; a
   producer never asserts a verdict of its own.

## Adapters

Any platform that can fill the contract works, and the engine has no branch for any of them:

- **Android** (Mi A3, via the MCP/edge service): produces motion observations from Camera2 and, in
  future, audio observations from `AudioRecord`.
- **Linux/V4L2** (`V4l2CameraDevice`): produces the same motion observations from a UVC device. This is
  what the workstation experiments use, and it is why the camera work needed no Android API.
- **Audio** (`IAudioObservationSource` in `src/core/AudioObservation.h`): `open` / `read_window` /
  `close` over PCM. ALSA, Android or a file are interchangeable implementations; one adapter exists as
  a test double today.
- **Network cameras**: out of scope for this contract's first version and deliberately not stubbed —
  V380 media integration remains blocked, so there is nothing honest to adapt yet.

## Media transport stays out of the control plane

Frames and audio samples never cross into the Oracle control plane, and nothing in the contract can
carry them. The control plane carries small JSON observations and verdicts; media is read by the
adapter, reduced to bounded scalars locally, and dropped. A capture stores no frame and no audio — only
the numbers and a transcript, which is what makes the evidence durable and the privacy boundary
auditable.

## What is not claimed here

The contract is not a network protocol, not an authentication boundary, and not a licence to listen or
watch anything without consent. It says nothing about species, disasters or identity, and it cannot: the
vocabulary has no room for them.
