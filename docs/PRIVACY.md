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

## Alexa+ integration — privacy notes (added 2026-10-10)

Alexa integration, if built, would be a separate authenticated client (Alexa
custom/Smart Home skill → HTTPS endpoint → C++ service). The event pipeline
stays independent of Alexa. The Alexa Developer Console simulator cannot reach
the Ubuntu computer's camera or microphone. No Amazon credentials have been
requested; none will be stored in source or Git. No Alexa+ interaction has been
demonstrated, so no integration is claimed.
