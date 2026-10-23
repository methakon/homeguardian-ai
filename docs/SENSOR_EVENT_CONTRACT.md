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
| `provenance` | `real` \| `synthetic` — **stated by the producer, never inferred** | producer |
| `status` | `detected` \| `not_detected` \| `inconclusive` \| `unavailable` | detector |
| `confidence` | bounded `[0,1]` for this observation alone | detector |
| `detector_id`, `model_id` | which rule and which version produced it, so a verdict is explainable | detector |
| `features` | bounded scalars only (motion fraction, region spread, polarities, audio RMS/peak/ZCR) | detector |
| `uncertainty` | what the observation explicitly does **not** mean | detector |

Rules a producer must follow:

1. **Provenance is a claim about where the data came from.** A fixture is `synthetic` even when it
   models a real event. A capture is `real` only when it came off a sensor in this session. **An
   observation is one or the other, never `mixed`:** mixed evidence is a property of a SET of
   observations, derived by the fusion engine from its members (`EvidenceProvenance::uncertain`), and a
   producer that asserts it is refused rather than believed.
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

## The consumer, as implemented

`SensorEventConsumer` (`src/core/SensorEventConsumer.*`) is the boundary that decides whether an incoming
event becomes evidence. It is deliberately not a transport and not an authentication layer; its job is
validation, and every refusal has a closed code so a caller can act on it rather than parse prose:

| code | what it means |
|---|---|
| `not_an_event` | `type` is not `inference` |
| `unsupported_schema` | `schema_version` this consumer does not know |
| `malformed` | a required field is missing, or has the wrong JSON type |
| `source_identity` | the source is empty or is not a prefix this consumer trusts |
| `provenance_invalid` | not `real` or `synthetic` — including a `mixed` claim |
| `provenance_conflict` | a `real` claim from a source that identifies itself as a fixture |
| `future_timestamp` | stamped beyond the clock-skew allowance |
| `stale_or_replay` | older than the last accepted observation |
| `duplicate` | this observation id was already accepted |
| `consent_missing` | consent required and not granted |
| `source_offline` | the producer reports its source unavailable |

Order matters and is tested: schema and types, then identity, then provenance, then consent, then
timestamps and replay. Identity before provenance is deliberate — an untrusted source is refused as
untrusted even if its provenance claim is also wrong.

Three behaviours are worth stating plainly because they are the ones a system usually gets wrong:

- **Consent stops capture.** A revoked consent is not a validation warning: the source is marked
  unavailable and nothing enters the window.
- **An offline source yields no evidence, not a quiet scene.** A disconnected sensor is a gap, and a gap
  is never fused as "nothing happened".
- **A replay is refused, not fused twice.** Duplicate ids and non-monotonic timestamps are both refused,
  so a replayed stream cannot manufacture a run of observations where none occurred.

What the consumer accepts reaches the fusion engine as a normalized `Observation`, and the engine's
verdict provenance is derived from the members: two real observations produce a real verdict, two
synthetic ones a synthetic verdict, and an empty window produces no verdict at all rather than a default.

## The Linux microphone adapter, as implemented and measured

`AlsaCommandAudioSource` (`src/backend/linux/`) implements `IAudioObservationSource` for Linux. It drives
the distribution's own ALSA front end (`arecord`) and reads PCM from a PIPE: `-t raw -` is the last two
arguments of the command it builds, so there is no file argument anywhere and no audio is stored. The
window is the unit - one bounded capture process per `read_window()` - so a device that disappears is
contained to one window rather than wedging a session, and a device that appears is picked up without a
restart. The cost is process startup per window, which is measured rather than hidden.

The command is injected, so every refusal is reachable in a test without a microphone: the tool missing
(exit 127), no capture hardware listed, an unbounded window, a window longer than the extractor accepts, an
odd byte count, an empty capture, a stream that hit the byte bound, a failed capture process, and reading
before open.

**A real bug this caught.** The first version defaulted to a 2-second window and produced 96000 frames,
which `extract_audio_features` refuses: its bound is `FeatureBounds::kMaxAudioFrames` = 48000 per call. The
adapter now validates the window at `open()` and names the bound ("use repeated windows"), and a test pins
both the refusal and the accepted 1-second window. A longer measurement is repeated windows, which keeps
memory per window bounded and makes the window length an explicit choice.

**Measured on this machine** (`hg_audio_probe --seconds 1 --check-refusal`, workstation HDA ALC897):

```
window  : 48000 frames, 1 ch, 48000 Hz, 1158 ms wall for 1.00 s of audio
features: rms=0.1311  peak=0.2203  zcr=0.0051        provenance: real
refusal : device hw:99,99 -> open=ok read=NO WINDOW  PASS: a nonexistent device produced no window
```

Note what the refusal test shows: `open` succeeds, because the tool and the hardware exist; the *capture*
is what fails, and it fails as "no window" rather than as a plausible quiet room. That is the behaviour the
contract requires of a disconnected source.

**Still not done, and not claimed:** the physical consent-revocation, silence-versus-known-sound, and
noise-floor-calibration checks are exercised by fixtures and by the bounded manual capture recorded in the
evidence log, not yet by an automated hardware test. There is no speech recognition, no speaker
identification and no species inference in this path, and no raw audio is retained anywhere.
