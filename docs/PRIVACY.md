# HomeGuardian AI — Privacy, Consent, Retention and Data Lifecycle

This document describes the Phase E data model, consent semantics, retention and
deletion behaviour, schema migration, and the limitations of the current
implementation. It is the authoritative reference for how profile, consent, and
routine data is stored and handled.

## 1. Data model

### FamilyProfile (`src/core/FamilyProfile.h`)

| Field | Type | Notes |
|-------|------|-------|
| profile_id | string | Stable, opaque identifier. Not derived from a name. |
| display_name | string | Human-readable label, max 128 chars. |
| age_band | optional enum | `child`, `teen`, `adult`, `senior`. Genuinely optional. |
| enabled | bool | Profile active flag. |
| created_at / updated_at | timestamp | Milliseconds since epoch. |
| schema_version | uint32 | >= 1. |

**Not collected by default:** birth dates, identity documents, face embeddings,
voiceprints, biometric templates, or any other sensitive attribute. Optional
attributes stay optional; nothing sensitive is added implicitly. A test asserts
that the serialized profile contains none of these fields.

Profiles must not be used to diagnose medical conditions or to infer
personality, dangerousness, intent, trustworthiness, or emotional state from
facial or vocal characteristics. No such inference exists in Phase E.

### ConsentRecord (`src/core/ConsentRecord.h`)

| Field | Type | Notes |
|-------|------|-------|
| consent_id | string | Unique record identifier. |
| subject_scope | string | A profile_id or a household-wide scope label. |
| purpose | string | Processing purpose, e.g. `fall_detection`, `presence`. |
| data_categories | string[] | Authorized categories, e.g. `camera`, `audio`, `sensor`. |
| decision | enum | `granted`, `denied`, `withdrawn`. |
| policy_version | string | Policy/consent version label. |
| recorded_at | timestamp | When the decision was recorded. |
| expires_at | optional timestamp | Optional expiry / review requirement. |
| provenance | string | Opaque origin label. No identifying personal data. |
| schema_version | uint32 | >= 1. |

### Routine (`src/core/Routine.h`)

| Field | Type | Notes |
|-------|------|-------|
| routine_id | string | Unique identifier. |
| profile_id | string | Owning profile. |
| label | string | Human-readable, max 128 chars. |
| schedule | optional string | `daily`, `weekly`, `weekdays`, or `HH:MM` (24h). |
| time_zone | optional string | One of `UTC`, `Asia/Kolkata`, `America/New_York`, `Europe/London`. |
| enabled | bool | Active flag. |
| created_at / updated_at | timestamp | Milliseconds since epoch. |
| schema_version | uint32 | >= 1. |

A routine records **configuration only**. It is never treated as confirmation
that a person completed a meal, medication, or activity. A missing event remains
"not confirmed"; it never becomes a negative finding. A test asserts the
serialized routine carries no completion/confirmation state.

## 2. Consent semantics

- **Default-deny.** When no consent record exists for a subject and purpose,
  processing is not authorized. `ConsentRecord::is_active(now)` returns true only
  when the decision is `granted` and the record has not expired.
- **Explicit states.** `granted`, `denied`, and `withdrawn` are distinct.
  `denied` and `withdrawn` are never active under any circumstance.
- **Expiry.** A `granted` record with `expires_at` in the past is not active.
- **Withdrawal.** A `withdrawn` record is never active, and an authorization gate
  keyed on "any active grant for this purpose" must evaluate to not-authorized
  once the latest decision for the purpose is a withdrawal. This is covered by
  the "withdrawal prevents future authorization" test.
- **Household configuration, not legal proof.** A stored consent record is
  household configuration. It is **not** interpreted as proof of legally valid
  consent. Enforcement is the responsibility of a future media-processing
  component; no such component exists in Phase E.
- **No media, biometrics, or secrets.** Consent and profile records store no raw
  media, face templates, voiceprints, or secrets.

## 3. Persistence and schema migration

- Schema version is tracked via SQLite `PRAGMA user_version`.
- **v1** created the `events` and `alerts` tables (Phase D).
- **v1 → v2** is an **additive** migration that adds `profiles`, `routines`, and
  `consent_records`. Existing `events` and `alerts` tables are untouched, so all
  prior data is preserved.
- The migration runs inside a single transaction. On any error it rolls back and
  rethrows, leaving the schema and `user_version` unchanged. It is safe to rerun
  because it only executes when `user_version == 1`.
- Foreign keys: `routines.profile_id` references `profiles(profile_id)` with
  `ON DELETE CASCADE`. `PRAGMA foreign_keys = ON` is set on every connection.
- Indexes exist on `routines(profile_id)`, `consent_records(subject_scope)`, and
  `consent_records(subject_scope, purpose)`.
- Uniqueness: every table has a TEXT PRIMARY KEY. Extended SQLite result codes
  are enabled so a duplicate PRIMARY KEY / UNIQUE violation is reported as a
  `Duplicate*Exception`, while a foreign-key violation is reported as a
  `DatabaseException`. This distinction is required for correct error handling.

## 4. Retention, export and deletion

- **Configurable retention.** `routine_retention_days` (default 730) and
  `profile_retention_days` (default 1825) are validated configuration fields.
  A value of `0` means "keep until explicit deletion".
- **Profile deletion.** Deleting a profile removes its dependent routines via
  `ON DELETE CASCADE`. Consent records are keyed by `subject_scope` and are not
  automatically deleted by the cascade; they should be reviewed and removed
  explicitly when a subject is removed, per the retention policy.
- **Historical event evidence is not deleted** merely because a profile is
  deleted. Event and alert history is retained under its own event/alert
  retention policy unless that policy explicitly requires removal.
- **Export / inspection.** Profile, consent, and routine records expose
  `to_json()`. Export uses these structures and never includes secrets, media,
  or biometric data. No secret or personal data is written to default
  configuration files.

## 5. Configuration

Phase E configuration fields (`src/core/Config.h`):

| Field | Default | Notes |
|-------|---------|-------|
| media_capture_enabled | false | Camera/audio capture is OFF by default. |
| cloud_processing_enabled | false | External upload / cloud processing is OFF by default. |
| consent_policy_version | "1.0.0" | Policy label applied to new consent records. |
| routine_retention_days | 730 | Validated range 0–36500. |
| profile_retention_days | 1825 | Validated range 0–36500. |

Invalid values are rejected at load/save time. Development/test configuration
is kept separate from the shipped default (`config/homeguardian.json`), which
has capture and cloud processing disabled. A test asserts the shipped default
keeps both disabled.

## 6. Phase F1 — Consent-Gated Simulated Sensing

### Consent gate (`src/core/ConsentGate.{h,cpp}`)

`ConsentGate` is the single decision point consulted before any protected
acquisition or processing. It is deliberately narrow so the consent rule is
defined in exactly one place; components must not re-implement it. Given an
`AcquisitionRequest` (subject_scope, purpose, data_category) it returns a
`GateDecision` and fails closed. Deny reasons, in evaluation order:

1. `capture_disabled` — `media_capture_enabled` is false in configuration.
2. `no_consent_record` — no record exists for subject+purpose.
3. `decision_not_granted` — the latest decision for subject+purpose is denied or
   withdrawn (a later withdrawal/denial overrides any earlier grant).
4. `expired` — the latest decision is a grant whose `expires_at` has passed.
5. `ambiguous` — more than one active grant exists for the same purpose.
6. `category_missing` — the grant does not authorize the requested data category.
7. `none` — authorized.

Missing, ambiguous, expired, denied, and withdrawn authorization all fail closed.
The gate is a pure decision function: it performs no acquisition and no side
effects.

### Gated acquisition (`src/core/ConsentGatedAcquisition.{h,cpp}`)

`ConsentGatedAcquisition` wires the gate to a `SimulatedSensorSource`. On
`acquire()` it consults the gate first; if denied it returns immediately with no
event and the sensor is never read (`emitted_count()` unchanged). If authorized,
it pulls the next synthetic reading. This demonstrates the control flow that a
future real hardware boundary must follow.

### Simulated sensor (`src/core/SimulatedSensorSource.{h,cpp}`)

A deterministic `IEventSource` that emits synthetic observations. Every emitted
event's payload carries `"synthetic": true` and a `sensor_kind`. It supports
configurable observation timestamps, malformed input (array payload rejected by
the `Event` constructor), and simulated disconnect/reconnect (while disconnected,
`next()` yields nothing).

### Threat model (Phase F1)

- **Unauthorized acquisition.** Mitigated by the consent gate failing closed and
  by the gated-acquisition path never reading the sensor on a deny. Tested for
  denied, missing, expired, withdrawn, unknown-profile, and mismatched
  purpose/category.
- **Bypassing the gate.** The gate is the only path to acquisition in this phase.
  A future hardware component must call the gate before opening any device; that
  boundary does not exist yet.
- **Confusing synthetic data for real data.** Every simulated event is tagged
  `synthetic`. Tested.
- **Withdrawal not honored.** A later withdrawal/denial for a purpose overrides
  earlier grants because the latest `recorded_at` decision is authoritative.
  Tested.
- **Global kill switch.** `media_capture_enabled = false` denies all acquisition
  regardless of consent. Tested.

### Important limitation

This phase proves **simulated** enforcement only. There is no real camera,
microphone, biometric, or cloud component, so it does **not** prove enforcement
at a real hardware boundary. No face recognition, voiceprints, health diagnosis,
or emotion/intent inference is implemented. Camera, microphone, biometric
processing, and cloud upload remain disabled.

## 6b. Phase F2.1 — Device Interface and Lifecycle

### Interface (`src/core/IMediaDevice.h`)

`IMediaDevice` is the abstraction a real camera/audio backend must implement.
It defines explicit states (`Idle`, `Initialized`, `Capturing`, `Stopped`,
`Error`), explicit `initialize()/start()/stop()/close()/fail()`, `state()`, and
`kind()`. Resource cleanup is deterministic: `stop()` releases capture resources
and the destructor guarantees stop()+close(), so a device is never left
capturing. No hardware code lives in the interface.

### Consent-guarded lifecycle (`src/core/ConsentGuardedDevice.{h,cpp}`)

`ConsentGuardedDevice` wraps an `IMediaDevice` and a `ConsentGate`. Every
`initialize()`, `start()`, and per-acquisition `recheck_and_enforce()` consults
the gate first and fails closed. On consent withdrawal, expiry, OS permission
revocation (a permission callback returning false), device failure, or shutdown,
`recheck_and_enforce()` forces the device out of capture and releases resources,
and blocks further acquisition. If authorization or configuration cannot be
evaluated, the guard fails closed.

### Android environment finding (documented assumption)

- SDK present at `~/Android/Sdk`: platform `android-34`, build-tools `34.0.0`.
- **No NDK installed** (`~/Android/Sdk/ndk` absent), no SDK CMake, and **no
  device attached** (`adb devices` empty).
- Consequence: the interface and lifecycle are implemented and tested with
  in-process mocks, but real NDK Camera2 / AAudio code cannot be compiled or run
  here. The design targets NDK API level 34 as an assumption to confirm when the
  NDK is installed. Real-device integration is a subsequent milestone (F2.2),
  not claimed complete.

### Threat model additions (F2.1)

- **Device left capturing after consent change.** `recheck_and_enforce()` forces
  stop on withdrawal/expiry/permission-revocation/failure. Tested.
- **Resource leak on abnormal exit.** Destructor guarantees stop()+close().
  Tested (mock destructor; ASan clean).
- **Capture activated without authorization.** `initialize()`/`start()` require
  a gate pass; denied operations leave the device `Idle` with no capture. Tested.
- **Fail-closed on unevaluable state.** Device `Error` or revoked permission
  denies. Tested.

Mock tests prove lifecycle and consent-gating behavior only. They do **not**
prove real hardware enforcement.

## 6c. Phase F2.2 — Real Android Device Backends

### Environment readiness (verified 2026-10-10)

- Android SDK at `~/Android/Sdk`: platform `android-34`, build-tools `34.0.0`,
  platform-tools (adb 1.0.41).
- **NDK r26d (Pkg.Revision 26.3.11579264)** installed side-by-side under
  `~/Android/Sdk/ndk/android-ndk-r26d` via the official Google download
  (`dl.google.com/android/repository/android-ndk-r26d-linux.zip`). The official
  `sdkmanager` could not be used because no Java runtime is installed and there
  is no passwordless sudo; the direct NDK zip avoids both.
- Toolchain: clang 17.0.2 (`toolchains/llvm/prebuilt/linux-x86_64`).

### API selection (verified from NDK r26d sysroot headers)

- **Camera:** NDK camera API — `ACameraManager`, `ACameraDevice`,
  `ACameraCaptureSession` (`camera/NdkCameraManager.h` etc.). Available since
  **API level 24**.
- **Audio:** AAudio (`aaudio/AAudio.h`), core capture API `__INTRODUCED_IN(26)`.
  Available since **API level 26** (newer AAudio calls 28–32 are avoided).
- Both are within the confirmed API 27 floor.

### Confirmed device (live, via ADB)

A physical device is attached and verified. The model string is **not trusted**
and was **not** corroborated:

- `ro.product.model=S25_Ultra` is cosmetic. Corroborating properties disagree:
  `ro.product.brand=alps`, `ro.product.manufacturer=alps`,
  `ro.board.platform=mt6580`, `ro.hardware=mt6580`, fingerprint
  `Ruby/Ruby/Ruby:8.1.0/...:user/release-keys`.
- Conclusion: a MediaTek **MT6580 "Ruby"** reference device, 32-bit, ~1 GB RAM —
  **not** a Samsung S25 Ultra. Recorded honestly as a discrepancy.
- Android 8.1.0, SDK/API **27** (cross-checked via two getprop sources).
- ABI: **armeabi-v7a** primary; abilist `armeabi-v7a,armeabi`; abilist64 empty →
  32-bit only (no arm64 on this device).
- RAM: MemTotal 984792 kB ≈ 962 MB.
- Cameras: 2 (Back, Front); camera2 capable. Microphone present
  (`android.hardware.microphone`, `android.hardware.audio.low_latency`).
- Device serial is recorded internally only and is **not** published.

### Backend implementation status

- `src/backend/android/NdkCameraDevice.{h,cpp}` and
  `src/backend/android/AAudioCaptureDevice.{h,cpp}` implement `IMediaDevice`
  behind `#ifdef HOMEGUARDIAN_ANDROID`. They contain **no** frame decoding,
  face recognition, voiceprints, or inference.
- **Fail-closed by default.** `initialize()` requires `set_device_confirmed(true)`,
  which must only be set after a physical device is confirmed available **and**
  hardware acceptance testing has passed. Until then `initialize()` throws and
  moves to `Error`, so real capture is never opened. `set_device_confirmed(false)`
  remains the default.
- Consent enforcement at the acquisition boundary is provided by
  `ConsentGuardedDevice`, which calls the `ConsentGate` immediately before
  `initialize()`, `start()`, and every per-frame/per-sample delivery
  (`authorize_delivery()`), closing the time-of-check/time-of-use gap at the
  interface level. A real backend must deliver a frame/sample only after
  `authorize_delivery()` returns allowed.

### Delivery-time consent gate (TOCTOU)

`ConsentGuardedDevice::authorize_delivery()` is the delivery gate a real backend
MUST call immediately before handing a frame/sample to processing. It re-runs
the full consent/permission/device evaluation and returns true only if the
payload may be delivered right now; on any denial it forces the device out of
capture and the backend MUST drop the payload. This closes the
time-of-check/time-of-use gap: authorization at `start()` is not sufficient —
every delivery is re-authorized.

### Defect found and fixed (F2.2 hardening)

`recheck_and_enforce()` previously called `close()` on a device in `Error`,
which reset it to `Idle`. A subsequent `authorize_delivery()` could then see a
healthy `Idle` device with consent still granted and wrongly re-authorize
delivery. Fixed: after releasing resources on error, the device is kept in
`Error` (via `fail()`) until it is explicitly re-initialized, so an errored
device can never be silently resurrected into a deliverable state. Proven by
the "device failure stops delivery" test. The camera `stop()` was also fixed to
release capture handles (session/device) rather than only transitioning state,
so repeated start/stop cannot leak handles.

### Verification status (kept strictly separate)

| Layer | Status | Evidence |
|-------|--------|----------|
| Compile (Android, arm64-v8a / API 34) | VERIFIED | `libhomeguardian_android_backend.a` builds with NDK r26d; objects reference `ACameraManager_create`, `ACameraManager_getCameraIdList`, `AAudio_createStreamBuilder` (unresolved — a static archive, NOT a runnable app) |
| Compile (Android, armeabi-v7a / API 27) | VERIFIED | `hg_ondevice_selftest` builds for the confirmed device ABI/API |
| Mock integration (host, fake backend) | VERIFIED | `test_fake_device_delivery.cpp`: delivery gate delivers only while authorized; withdrawal/expiry/permission-revocation/failure each stop delivery; repeated start/stop/close releases resources; destructor releases a capturing device |
| On-device non-capture self-test | VERIFIED | `hg_ondevice_selftest` pushed via `adb push` to `/data/local/tmp` and executed on the device: 17/17 checks pass (consent-gate decisions, lifecycle, fail-closed guards, cleanup). No camera/mic opened; no CAMERA/RECORD_AUDIO permission needed. |
| Camera1 adapter contract (host) | VERIFIED | `test_camera1_adapter.cpp`: 6 cases (authorized delivery; withdrawal drops frames + stops; permission revocation denies; device error denies + no resurrection; repeated start/stop/close idempotent; capture-disabled-by-default). Host only — not real capture. |
| APK build with Camera1 backend | VERIFIED | Debug-signed APK builds for armeabi-v7a / API 27; `Camera1Bridge` in DEX (0 invokedynamic); native `nativeOnPreviewFrame` delivery-gate symbol exported; signature valid. |
| On-device launch with Camera1 backend | VERIFIED | App installs, launches, self-test runs, no crash, camera NOT active (Active Camera Clients empty). Camera not opened. |
| Physical hardware capture (Camera1) | NOT YET RUN | Camera1 capture path is implemented and delivery-gated, but no Camera1 frame has been captured on the device. Awaiting explicit approval. Hardware-level capture enforcement is NOT claimed. |

The Camera1 adapter (`apk/.../Camera1Bridge.java` +
`nativeOnPreviewFrame` in `hg_harness_jni.cpp`) is used because the live
diagnosis (friction log incident 10) established this MT6580 / Android 8.1
device exposes cameras only via the Camera1 compatibility path. Backend
selection is **explicit** (`USE_CAMERA1` in `MainActivity`), never a silent
fallback; `NdkCameraDevice` is retained for camera2-native devices. Both reuse
the same `ConsentGuardedDevice` delivery gate (`authorize_delivery()`), called
per preview frame immediately before processing. On consent withdrawal,
permission revocation, or device error the gate returns false, the frame is
dropped, and the bridge stops preview and releases the camera.

Camera and microphone remain disabled by default (`media_capture_enabled =
false`). `set_device_confirmed(false)` is the default on both backends, so real
capture is never opened without a documented on-device acceptance test.

## 7. Limitations

- Consent is enforced by the simulated gate in Phase F1, but only at a simulated
  boundary. No real hardware component exists, so real-hardware enforcement is
  not yet proven.
- A stored consent record is configuration, **not** proof of legally valid
  consent.
- No raw media, face templates, voiceprints, or secrets are stored anywhere.
- Schedule parsing is intentionally minimal (`daily`/`weekly`/`weekdays`/`HH:MM`)
  and time zones are a small allow-list. It is not a general cron engine.
- The routine model does not confirm activity completion from any data source.

## Ubuntu (Linux) sensor backend — privacy notes (added 2026-10-10)

The Ubuntu backend adds no new data-retention surface. It reuses the same
consent gate and delivery-boundary authorization as the Android backend:

| Item | Status | Evidence |
|---|---|---|
| Ubuntu sensor discovery (V4L2/PipeWire metadata) | VERIFIED (non-capture) | `SensorDiscovery` enumerates device nodes and sound-server metadata only; opens no device, reads no frames/samples. |
| Ubuntu consent enforcement (host) | VERIFIED (non-capture) | 4 host cases reuse `ConsentGuardedDevice::authorize_delivery()`: capture-disabled-by-default blocks start; withdrawal stops per-frame delivery; permission revocation stops delivery. Host only — no real sensor opened. |
| Ubuntu camera/mic/speaker capture | NOT IMPLEMENTED | Awaiting explicit approval; no sensor has been opened. |

Capture remains disabled by default; consent is rechecked immediately before
every frame/audio-buffer delivery; withdrawal, permission revocation, or device
error stops delivery and releases resources. No recording to disk, no uploads,
no retention of personal media. No automatic camera/mic access at launch or
during tests. The computer's camera and microphone have NOT been opened.

## Simulator-first backend — privacy notes (added 2026-10-10)

The deterministic simulator (`SimulatedSensorBackend`) produces synthetic,
in-memory-only frames and samples:

| Item | Status | Evidence |
|---|---|---|
| Simulator consent/lifecycle/delivery tests | VERIFIED (non-capture) | 12 host cases via `ConsentGuardedDevice`: denial, withdrawal, expiry, permission revocation, delivery-boundary, bounded buffers, error recovery, shutdown. |
| Real sensor capture | NOT RUN | Simulator opens no device file, makes no hardware syscalls, writes no media, no network. |

Simulated frames/samples are never persisted, uploaded, or written to disk.
They exist only in a bounded in-memory ring during a test. The simulator must
not be substituted for a real backend in production capture. No physical camera
or microphone was opened, and no real sensor data was produced.

### Realistic simulation — privacy notes (added 2026-10-10)

The extended simulator adds only in-memory synthesis: deterministic camera
frames, deterministic PCM test signals (noise/sine/square/silence), and
speaker playback of already-generated payloads. All of it is synthetic and
local:

| Item | Status | Evidence |
|---|---|---|
| Realistic simulation tests (config/signals/speaker) | VERIFIED (non-capture) | 11 more host cases (23 total `[sim]`): dimensions + frame-size, frame-rate pacing, configurable ring capacity, stereo sizing, silence/sine/square determinism, speaker play/error/recovery, and play independent of the capture consent gate. |
| Real sensor capture | NOT RUN | No device file opened; no hardware syscall; no media written; no network. |

- Speaker `play()` is output-only and never touches the capture buffer, disk, or
  hardware; it is not gated by the capture consent delivery boundary (it is not
  a capture path).
- Configurable ring capacity keeps memory bounded under overflow (test-verified).
- No physical camera or microphone was activated; no kernel module was loaded;
  no system permission was changed.

## Dual logical-device + shared audio broker — privacy notes (added 2026-10-11)

The dual-device simulation adds no new data-retention or capture surface. It is
the deterministic in-process simulator only:

| Item | Status | Evidence |
|---|---|---|
| Dual-device + shared broker tests | VERIFIED (non-capture) | 12 `[dual]` cases / 74 assertions; ASan+UBSan clean. |
| Real sensor capture | NOT RUN | No device file opened; no hardware syscall; no media written; no network. |

- **One shared mic, many consent-guarded consumers.** Each fan-out is authorized
  per consumer via `ConsentGate::check`; denied/withdrawn/revoked consumers
  receive nothing and never stop the shared source for others. If no consumer is
  authorized, the shared source produces nothing.
- **Speaker output is an OUTPUT path**, not gated by the capture-consent
  boundary; the bounded, arbitrated speaker queue carries only synthetic
  responses. Memory is bounded (queue overflow rejects the new job; capture
  buffers bounded).
- Logical devices are HomeGuardian simulation entities; they do **not** run
  Amazon's Alexa OS and make no Alexa-product claim. No AWS resources,
  credentials, or Alexa skill deployment are involved. Camera remains a separate
  simulated peripheral with no raw access from a logical device.
- No physical microphone, speaker, or camera was activated; no kernel module
  loaded; no system permission changed.

### End-to-end simulator workflow — privacy notes (added 2026-10-10)

The end-to-end test (`test_e2e_simulator.cpp`) uses a **synthetic test
scenario**:

| Item | Status | Evidence |
|---|---|---|
| End-to-end happy path + failure/consent scenarios | VERIFIED (non-capture) | 9 `[e2e]` cases / 73 assertions; ASan+UBSan clean. |
| Real sensor capture | NOT RUN | No device file opened; no hardware syscall; no media written; no network. |

- The `SyntheticTestProcessor` only ever receives payloads that already passed
  `authorize_delivery()`. Denied capture data never reaches it (asserted in the
  denial/withdrawal/capture-disabled/failure cases).
- The generated response is synthetic and aggregate, explicitly labelled "not a
  real-world alert"; it carries no sensitive family/sensor detail. Speaker
  playback is an output path and is independent of the capture-consent gate.
- The scenario does **not** detect real-world danger, identify people, or
  provide medically or emotionally sensitive conclusions. All data is in-memory
  placeholders. No personal media or sensor data is produced, persisted, or
  uploaded. No physical sensor was activated.

## Runtime hardening — privacy notes (added 2026-10-11)

The `AlertPolicy` and lifecycle hardening add no new data-retention or capture
surface; they operate on synthetic alerts and simulator state only:

| Item | Status | Evidence |
|---|---|---|
| Alert policy + lifecycle + hardening tests | VERIFIED (non-capture) | 33 hardening cases (`[alertpolicy]` 11, `[lifecycle]` 17, `[hardening]` 5); ASan/UBSan clean. |
| Real sensor capture | NOT RUN | No device file opened; no hardware syscall; no media written; no network. |

- Alert dedup/cooldown governs **notification frequency** only; it never
  suppresses a first occurrence or an unrelated (different rule/severity) alert,
  and a suppressed duplicate remains an active condition. It is not a safety
  detector.
- Retained dedup state and event/alert histories are bounded (oldest evicted);
  memory does not grow without bound under sustained load.
- Configuration remains fail-closed: capture disabled by default; invalid or
  contradictory config is rejected before components start and cannot enable
  capture or bypass consent.
- The alert is a **synthetic, detection-driven activity notice** (raised only when
  the fused detector evidence qualifies), never a real-world danger detection. No
  raw audio/video/biometric data is persisted. No physical sensor was activated.

## Durable audit + observability — privacy notes (added 2026-10-11)

`PersistenceAudit` and `RuntimeStatus` add durable metadata audit and bounded
observability without any new capture or retention surface:

| Item | Status | Evidence |
|---|---|---|
| Audit + observability tests | VERIFIED (non-capture) | 10 cases (`[audit]` 7, `[status]` 3); ASan/UBSan clean. |
| Real sensor capture | NOT RUN | No device file opened; no hardware syscall; no media written; no network. |

**Persisted fields (approved metadata only), in the existing `alerts` table:**

| Field | Purpose | Retention / deletion |
|---|---|---|
| alert_id | stable identifier | deleted by `prune_older_than` (age-based) |
| severity, message | alert classification | same |
| contributing_event_ids | correlation/audit trail | same |
| detected_at (timestamp) | when the alert fired | same (drives retention) |
| confidence | rule confidence | same |
| rule_name | which rule fired | same |
| details (JSON) | rule metadata (window, contributing events) | same |

- **Never persisted:** raw audio/video, biometric data, media payloads,
  unnecessary personal identifiers, or consent-as-authorization. Consent lives
  only in the in-memory gate and fails closed after restart.
- **Retention:** age-based via `prune_older_than` (transactional; preserves
  newer/unrelated records). Tested.
- **Observability:** `RuntimeStatus` logs only counts/states (no media, secrets,
  credentials, or personal data), one summary line per cycle (not per sample),
  and cannot grant consent or enable capture. Counters reset per process and are
  never mistaken for durable audit history.
- **Logs/records cannot be mistaken for physical sensor activation:** all media
  is synthetic; the alert is a synthetic, detection-driven activity notice, not a
  danger detection. No physical sensor was activated.

## Integration contract + consent verification (added 2026-10-11)

| Item | Status | Evidence |
|---|---|---|
| Alexa contract + auth-boundary tests | VERIFIED (non-capture, local) | 14 C++ `[alexa]` + 12 Node adapter cases; fail-closed auth; read-only ops only |
| SQLite integrity/migration/retention tests | VERIFIED (non-capture) | 7 `[integrity]` cases (v1→v2 preserves alerts/ids/indexes; rollback; idempotent; reopen; corrupt-file preserved) |
| Privacy/consent regression tests | VERIFIED (non-capture) | 7 `[privacy]` cases (capture-off, withdrawal, unauthorized-observation-blocked, restart-no-restore, metadata-only, counter semantics, retention) |
| Live Alexa-to-application integration | NOT TESTED | Requires a deployed authenticated endpoint — documented as PENDING; no deployment performed |

- **Contract cannot mutate:** the only operations are `status` and
  `alert_summary` (reads). No operation enables capture, grants consent, changes
  configuration, or activates a sensor (tested: `enable_capture`/`grant_consent`/
  `set_config`/`activate_sensor` are rejected as malformed).
- **Authorization is fail-closed:** missing/invalid/expired credentials →
  `unauthorized`; authenticated-but-not-permitted → `forbidden`; degraded app →
  `unavailable`. A client-supplied identity is never trusted as proof.
- **No sensitive data in responses:** only state, capture flag, aggregate alert
  count/highest severity, and capped alert ids. No media payloads, secrets,
  personal identifiers, filesystem paths, or internal diagnostics. Error
  messages are generic.
- **Consent is per-process and fails closed on restart:** capture authorization
  and consent live only in the running process; a restart starts capture-disabled
  with no consent (tested).
- **No raw audio/video/biometric data is persisted** by the contract or the
  audit layer; the Alert schema has no media/payload field (tested).

## Local process-boundary bridge — privacy notes (added 2026-10-11)

| Item | Status | Evidence |
|---|---|---|
| Process-boundary integration tests | VERIFIED (non-capture, local) | 14 `integration.test.js` + 4 `restart.test.js` cases drive the actual compiled `hg_alexa_bridge` |
| Live Alexa-to-application integration | NOT TESTED | Requires a deployed authenticated endpoint — documented as PENDING; no deployment performed |

- **No network listener:** the bridge is stdin/stdout only; nothing binds a
  port. It cannot be reached beyond the local process that spawns it.
- **No shell injection:** the Node client spawns the bridge with an argument
  ARRAY (no shell interpolation). Requests/responses are size-bounded; the
  client kills the child on timeout and enforces an output bound.
- **Test-only credentials:** generated per-run into a temp file, kept out of
  version control, and removed in cleanup. The credential value is never logged
  or echoed. A client-supplied identity is never trusted as proof.
- **Read-only:** the bridge only serves `status` and `alert_summary`; it cannot
  enable capture, grant consent, change config, or activate a sensor.
- **Restart safety:** capture authorization and consent are per-process and
  fail closed; the bridge reports only the real `RuntimeStatus` +
  `PersistenceAudit` state. No raw media is persisted.
- **Capture-state reflection (added 2026-10-11):** the bridge loads the real
  config and reports the actual `media_capture_enabled` kill-switch state
  (read-only). A config load failure fails safe — capture is reported disabled.
  Reporting capture state is not enabling capture; the bridge has no mutation
  path. Verified by `integration.test.js` (capture-on config → `capture_enabled:
  true`; missing config → `false`, still serving).

## Credential handling — privacy notes (added 2026-10-12)

- **Secrets never logged/returned:** `AlexaCredentialStore` never logs, echoes,
  or returns token values; load-failure reasons are generic (no path/value).
  Verified by `integration.test.js` "secure mode: no credential leak in stderr"
  and `[creds]` cases.
- **Secure provisioning:** the credential file must be owner-only (0600),
  owned by the current uid, a regular file. Broader permissions, foreign
  ownership, missing/malformed content → fail closed (store empty; every verify
  unauthorized). Verified by `[creds]` permission/malformed/missing cases and
  the `0644 → exit 3` integration case.
- **Fail-closed, no mock fallback:** if the secure store fails to load, the
  bridge exits (code 3) rather than serving with a weaker verifier. Unknown,
  expired, and revoked tokens are indistinguishable to the caller.
- **Test fixtures are synthetic and isolated:** created per-run in a temp dir
  and removed in cleanup; never committed. Production secrets live outside Git
  and ordinary config at a runtime-supplied path.
- **Authentication never grants consent or enables capture:** the store only
  authenticates and returns read permissions; authorization is separate
  (contract-level). No mutating operation exists.

## Loopback HTTP adapter — privacy notes (added 2026-10-12)

- **Loopback only:** the adapter binds `127.0.0.1` exclusively and refuses to
  serve on any other address. No LAN/public/wildcard bind path exists.
- **No TLS:** a loopback HTTP listener is not end-to-end TLS. Controlled local
  development use only; any non-local deployment requires authenticated HTTPS
  with a reviewed TLS termination design (not implemented).
- **No token leakage:** tokens appear only in the `Authorization` header, are
  forwarded to the real C++ credential store, and never appear in responses,
  the `/health` body, stdout, or stderr. Verified by `http_adapter.test.js`.
- **No persistence:** no request data or raw media is written to disk by the
  adapter or the underlying bridge.
- **Read-only:** the adapter requests only `status`/`alert_summary`, derived
  from the allowlisted route — never from client input. It cannot grant consent,
  enable capture, activate sensors, or change configuration.
- **Generic auth failures:** 401 never distinguishes missing vs invalid vs
  expired vs revoked. Rate limiting and concurrency bounds prevent floods.
- **Genuine Alexa identity verification is NOT implemented** (LWA/OAuth provider
  pending — needs Amazon credentials); the adapter verifies local bearer tokens
  only.

## Simulated Alexa+ web demo — privacy notes (added 2026-10-12)

- **It is a simulation, clearly labelled.** The UI states in a banner and in the
  footer that this is a project-built simulated Alexa+ experience, not a real
  Alexa+ device integration, with no MCP server and no live Amazon identity.
- **No physical sensor.** Every observation is synthetic and produced in-process
  by the deterministic sensor simulator. No camera, microphone, or speaker is
  ever opened.
- **Consent/capture state is visible and enforced.** The UI shows the real
  capture/consent state from the backend. With capture disabled, the scenario
  reports `events_processed: 0` and explains that the gate blocked every
  observation — demonstrated by a `web_demo` test.
- **Real results, not fabricated.** The scenario runs the actual Application
  pipeline (SimulatorRuntime → event sink → Pipeline → correlation →
  AlertPolicy) and persists the delivered alert to SQLite; the UI reads the
  alert summary back from that store. No value in the UI is hardcoded.
- **Loopback only, no persistence of request data.** The demo server binds
  127.0.0.1 exclusively and writes nothing to disk; the demo SQLite database
  lives in a temp working directory created by `demo.sh` and is removed on exit.
- **No secrets in the UI or logs.** Tokens appear only in the credential file
  (0600) and are never rendered or logged.

## Product-readiness audit — privacy notes (added 2026-10-11)

Audited build/dependency setup, config, test fixtures, privacy guarantees, and
documentation. Findings and their privacy impact:

| Finding | Privacy impact | Resolution |
|---|---|---|
| README claimed "no code implemented" and a wrong `src/` layout | Misleading; could imply features not built (or hide real state) | Corrected to reflect the implemented simulator-first core; explicit "no real capture" statement |
| README listed `libcurl` + `MCP` as implemented | Overstated capabilities | Removed from "Implemented"; Alexa/MCP documented as optional and not a core dependency |
| Hardcoded absolute CMake include paths | Portability only (no data risk) | Replaced with `${CMAKE_SOURCE_DIR}` |
| Test `.db` files not gitignored | Local scratch DBs could be committed | Added `*.db`, `*.db-wal`, `*.db-shm`, `test_*.db` to `.gitignore` (no tracked `.db` files exist) |
| `data_dir` config field unused by current app | None (no privileged write) | Documented as reserved |

Privacy guarantees re-verified: capture disabled by default; every frame/sample
passes the consent delivery boundary; no disk writes, uploads, or retention in
the simulator; no physical sensor activated. The application runs with
`media_capture_enabled: false` and `cloud_processing_enabled: false`.

## Alexa+ integration — privacy notes (added 2026-10-10)

Alexa integration, if built, would be a separate authenticated client (Alexa
custom/Smart Home skill → HTTPS endpoint → C++ service). The event pipeline
stays independent of Alexa. The Alexa Developer Console simulator cannot reach
the Ubuntu computer's camera or microphone. No Amazon credentials have been
requested; none will be stored in source or Git. No Alexa+ interaction has been
demonstrated, so no integration is claimed.

## Alexa custom skill prototype — privacy notes (added 2026-10-10)

The Alexa skill (`alexa_skill/`) is a separate, thin client that never contacts
the C++ process or any sensor:

| Item | Status | Evidence |
|---|---|---|
| Skill handler unit tests | VERIFIED (no deployment) | 13 Node cases: supported intents, malformed requests, backend error/invalid-data paths, session-ended, and a mock that exposes no capture capability and no sensitive detail. |
| Console-simulator end-to-end | NOT YET RUN | Requires manual console steps (F22-19); no deployment performed by these files. |

- The skill returns **no** camera/microphone status or control. The mock status
  hard-codes `capture_enabled: false`.
- Backend error and invalid-data paths return generic, non-sensitive responses;
  tests assert that exception text and secrets never reach the spoken response.
- No credentials, keys, or account data are stored in the repository. No AWS
  resources are created by these files. No personal media or sensor data is
  produced or retained.
- The Alexa simulator cannot reach the Ubuntu computer's physical sensors; no
  physical camera or microphone was opened.

## Detection layer — privacy notes (added 2026-10-12)

The observation/inference layer (`Observation`, `AnomalyDetector`, `FusionEngine`,
`SyntheticActivityDetector`) was built under the following privacy boundaries,
each enforced by code and tests:

- **No raw media, ever.** An `Observation` carries only bounded, validated
  feature data (numeric features, booleans, quality/confidence scores). It never
  carries raw audio, raw video, facial embeddings, biometric templates, or
  transcripts. A test asserts the serialized payload contains no
  `audio`/`video`/`embedding`/`transcript` keys.
- **Synthetic provenance is explicit and default.** Every observation produced in
  this build is `Provenance::synthetic`; the `real` variant is reserved and
  unused. No physical camera, microphone, or speaker is activated, and no OS
  permission is requested.
- **No covert identification or tracking.** The detector stores only bounded
  numeric baselines keyed by a caller-supplied string — never identities, faces,
  or behavioural profiles. It does not learn new authorization rules.
- **Consent boundary unchanged.** The detection layer sits *downstream* of the
  `ConsentGate`/`ConsentGuardedDevice` delivery boundary and does not bypass it.
  With capture disabled or consent withdrawn, no observation reaches the pipeline.
  (The synthetic detector can still be invoked in tests/demo to produce an
  Observation object, but that is a pure in-memory computation, not sensor
  acquisition — the media-delivery boundary is what consent governs.)
- **No sensitive logging.** Detector reasons are bounded, non-sensitive strings
  (numbers and thresholds). No credentials, media paths, or personal data.
- **Explainability, not verdict.** `AnomalyDetector` and `FusionEngine` surface
  statistical deviations and detector agreement as *candidates for review*. They
  never assert a fall, illness, emergency, medical condition, emotional state,
  intent, or honesty. The existing `AlertPolicy` remains the sole authority that
  turns a condition into an alert.

**Data handling summary for the detection layer:** only numeric features and
derived statistics are retained, in bounded, size-capped structures (rolling
windows and a capped key table). Nothing is persisted to the audit database
beyond the resulting `Event`/`Alert` (which is alert *metadata* only). Nothing is
uploaded or transmitted off-host.

## Privacy audit log — privacy notes (added 2026-10-13, roadmap H-03)

The `PrivacyAuditLog` implements H-03 ("all data access logged; consent
enforcement verified") under strict privacy boundaries:

- **Metadata only.** An audit entry contains a timestamp, subject scope, purpose,
  data category, the gate decision, the deny reason, and an acquired flag. It has
  **no** field for raw audio, video, frames, samples, transcripts, embeddings, or
  biometrics, and none is derived from the payload. String fields are clamped to
  128 chars.
- **Passive observer.** The log never makes or changes a decision. Attaching,
  detaching, or failing to write an entry cannot alter whether data is accessed.
  A test asserts the decision is identical with and without the audit attached.
- **Never throws.** `record()` is best-effort; a logging failure cannot widen
  access or crash the runtime.
- **Bounded.** The retained entries are capped (`capacity`, default 256, oldest
  evicted); counters are cumulative. Memory cannot grow without limit.
- **Enforcement invariant.** `verify_enforcement()` fails if any entry records an
  acquisition that was not authorized — the auditable form of "no data accessed
  without consent". The demo surfaces this as `enforcement_ok`.
- **Fail-closed visibility.** With capture disabled, no data access is attempted,
  so the audit trail is empty and reports `enforcement_ok: true` — the gate
  blocked everything.
- **No new data access.** The audit observes existing decisions at the consent
  boundary; it introduces no new sensing, no new persistence of personal data,
  and no covert identification or tracking.

## v17 — scenario demo exposure and consent isolation (2026-10-13)

**What the demo now exposes (loopback only).**
- `/api/alerts` returns the most recent alert METADATA from the local audit
  store: `alert_id`, `severity`, `rule`, `message`, `confidence`, `detected_at`,
  `contributing_events`, and a derived explanation string. No media, no
  transcripts, no embeddings, no personal identifiers. Alert messages in this
  build are synthetic simulator strings.
- `/api/scenarios` returns the scenario catalogue (static text).
- `/api/run-scenario?scenario=<id>` accepts only tokens from a fixed allowlist;
  it is loopback-bound and subject to the existing Host/Origin checks.

**The Alexa-facing contract is unchanged and remains AGGREGATE-ONLY** (`state`,
`capture_enabled`, `alert_count`, `highest_severity`, capped `alert_ids`). Per-alert
detail is available only on the local demo surface, never through the contract.

**Consent.** Each scenario run clears the synthetic in-memory consent repo first
(`reset_consent_state()`), so a previous run's grants or withdrawals cannot
influence a later run. The fail-closed gate behaviour is unchanged, and the
privacy audit (`enforcement_ok`) is evaluated per run: in every scenario,
acquisitions equal authorizations (no acquisition without consent). Denied
acquisitions are counted and surfaced.

**No change** to: `synchronous=FULL`, WAL, the bounded (256) audit ring, token
access control, loopback binding, or the consent decision path.

## L-11 — detection-driven alerts: what the alert metadata now carries (2026-10-13)

The alert `details` JSON (and therefore the `/api/alerts` projection) now carries
the evidence that caused the alert: `fusion_verdict`, `fusion_confidence`,
`contributing` (count), `inconclusive` (count), `detector_ids` (bounded to 10),
`provenance` ("synthetic") and the fusion `reason` string (a bounded, numeric
explanation such as "detected by 5 input(s), agreement 1, confidence 1").

None of this is derived from media: these are aggregate detector verdicts and
counts. The bounded contributing-event id list is capped at 10. No raw audio,
video, biometrics, transcripts or credentials are added, and the Alexa-facing
contract remains AGGREGATE-ONLY.

Consent semantics are unchanged and now also gate detection: detection runs only
after an authorized acquisition, so a denied or withdrawn consumer can never
cause a detection observation or a detection-driven alert.
