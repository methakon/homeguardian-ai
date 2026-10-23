# HG-ANDROID-READY-14 (1) — feature separation: harness vs installed edge service

Read from the built artifacts, not from memory. `aapt dump badging` on both APKs.

```
HARNESS   org.homeguardian.harness     v0.2.0  "HomeGuardian Node"   minSdk 27  targetSdk 28
  permissions: CAMERA, RECORD_AUDIO, FOREGROUND_SERVICE, INTERNET, ACCESS_NETWORK_STATE

EDGESVC   org.homeguardian.edgeservice v1.0.0  "HomeGuardian Edge"   minSdk 30  targetSdk 30
  permissions: FOREGROUND_SERVICE, INTERNET, RECEIVE_BOOT_COMPLETED
```

## They are different applications with different jobs

| | harness (`org.homeguardian.harness`) | edgeservice (`org.homeguardian.edgeservice`) |
|---|---|---|
| declares CAMERA | **yes** | **no** |
| declares RECORD_AUDIO | **yes** | **no** |
| can capture anything | yes | **no — it cannot, structurally** |
| installed on Mi A3 | no | yes v1.0.0 |
| installed on Panasonic | **yes**, byte-identical `8e293ed4…` | no |
| SDK floor | minSdk 27 / target 28 | minSdk 30 / target 30 |

## 1. Coexistence: no camera conflict is possible

This is the part worth stating precisely, because it is stronger than "they probably won't collide":

- **The installed edge service declares no `CAMERA` and no `RECORD_AUDIO` permission.** An Android app
  that does not declare those permissions cannot open the camera or the microphone at all. It is not
  that it restrains itself — it has no capability to contend for the device.
- **The applicationIds differ**, so `/data/data/<applicationId>` differs and neither can read or write
  the other's data.
- Android grants no cross-package data access to a differently-signed or differently-named app, and
  imposes no signer constraint between *different* packages.

So installing the harness cannot cause a camera conflict **with the installed application**, because
that application never competes for the camera.

If a *third* app holds the camera when the harness starts, Android's camera is a **singleton**: the
harness receives `ERROR_CAMERA_IN_USE` and fails visibly. It does not receive shared or interleaved
frames. A silent-sharing outcome is not available in the platform.

## 2. Privacy surprise: what actually changes, stated plainly

Installing the harness **adds a camera- and microphone-capable application to a device that currently
has none**. That is a real change in capability and it should be said out loud rather than filed under
"no conflict":

- The capability is **declared** in the manifest and **gated** by runtime permission dialogs — the
  user sees the camera and microphone prompts and can refuse either.
- Capture cannot start without those grants; `NodeTransportHost` refuses to start when consent is
  missing and the transport marks `consent_missing` as suspending.
- The service is declared `exported="false"`, so no other app can drive it.
- Consent withdrawal tears the link down, releases the wake lock, and **restart is refused until
  consent is explicitly restored** (test `t4`). Restoring the link does not arm a sensor (`t5`).
- Nothing is persisted as raw media by default; the producer emits **scalar features**, and the wire
  contract caps a frame at 4096 bytes and `features` at 11 properties — a frame or an audio sample
  physically cannot be framed.

The honest summary: **no conflict with the existing app; a genuine new capability on the device, which
is declared, permission-gated, and revocable.**

## 3. Shared observation format

Both sides speak the contract at `docs/integration/observation.schema.json` (`contract_version 1.0`):

```
required : kind, protocol_version_major, protocol_version_minor, device_id, observation_id,
           sequence, source_type, observation_type, acquisition_mode, acquired_at_ms, features
provenance_claim : ["real", "synthetic"]
acquisition_mode : ["live", "synthetic"]
```

The Android producer was brought into line with this in `9275859` and its conformance is asserted by
`ProducerSchemaConformanceTest`, with `scripts/gen_cross_receiver_fixture.py` deriving the C++ side's
refusal vocabulary from source. The complete Android↔C++ refusal vocabularies match exactly (22/22).

## 4. Exact conditions for the first authorised Mi A3 physical capture

All of these, in order. Any one missing means the capture is not a valid physical test.

1. **Owner authorisation recorded**, for `apk_install` and `sensor_activation` on `edd53dbd04de`. The
   identity lists both as prohibited, and per `HG-ANDROID-SIGNING-RECOVERY-12` task prose cannot lift
   that — it needs the trusted approval path, which is a P0 owned by Antigravity.
2. **The approval guard resolved** so the authorisation is a looked-up record rather than a field
   inside the task (`scripts/propose_approval_guard_fix.py` reproduces both defects).
3. **Device identifiable and connected**: `edd53dbd04de`, `adb get-state` = `device`.
4. **Artifact verified**: `homeguardian-harness.apk`, sha256 `47f98357510e9a3a…` (android-34) or
   `ea7c27962623c91c…` (android-27), 25/25 unmangled JNI exports, and
   `bash scripts/prepare_physical_test.sh --serial edd53dbd04de` reporting **GO**.
5. **On-device consent granted** for camera and microphone at the runtime prompt. `pm grant` is not a
   substitute for the prompt: the test must record that the *user* consented.
6. **A real subject in front of the camera**, non-private, with no bystanders who have not consented.
7. **Screen-off**, because the property under test is that the foreground service keeps producing when
   the Activity is not visible. Screen-on would not exercise the change.
8. **A hub endpoint to receive the observation.** No endpoint is confirmed reachable, so the honest
   expectation today is a **local** capture test with the transport reporting its refusal/retry state,
   not a successful delivery to the C++ hub.

**What the test may claim afterwards:** that the foreground service produced observations with the
screen off. **What it may not claim:** that video or audio was delivered, or that a hub received it,
unless that was separately observed. Per `HG-ANDROID-READY-14` and the standing rules, metadata is not
evidence of media.

## Status

Prepared only. No APK installed, no camera or microphone activated, no device setting changed, no app
data touched.
