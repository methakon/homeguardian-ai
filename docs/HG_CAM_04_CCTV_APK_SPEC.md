# HG-CAM-04 — Lightweight Wi-Fi Camera APK: Next-Step Specification

Target: Panasonic ELUGA I7, Android 9 / API 28, arm64-v8a, MT6739.
Deliverable of this document: a grounded plan for the CCTV APK — what to reuse, what is
missing, and the exact order to build it in.

**Nothing here has been implemented, installed or activated.** No camera or microphone has
been opened on the Panasonic. This is preparation only, and building against this spec needs
separate permission before any capture is enabled.

## 1. Do not start a new app — extend the existing harness

This repository already contains an Android acquisition harness. Checked and verified
building on this host:

    apk/AndroidManifest.xml                                  package org.homeguardian.harness
    apk/src/org/homeguardian/harness/DeviceIdentity.java
    apk/src/org/homeguardian/harness/ObservationTransport.java   framed mutual TLS (D-03)
    apk/src/org/homeguardian/harness/NodeSession.java
    apk/src/org/homeguardian/harness/NodePanel.java              operator status panel
    apk/src/org/homeguardian/harness/MainActivity.java
    apk/src/org/homeguardian/harness/Camera1Bridge.java
    apk/jni/hg_harness_jni.cpp
    apk/build_apk.sh                                          no-Gradle JDK+SDK+NDK build
    src/backend/android/{Camera1Device,NdkCameraDevice,AAudioCaptureDevice}.cpp
    src/core/{ConsentGate,ConsentGuardedDevice,CaptureAccounting,BoundedFeatures,
              PreviewSizePolicy,NodeIdentity}.cpp
    docs/ANDROID_TRANSPORT_D03.md

Build verified on this host (2026-10-17), host-side only:

    ==> Verify JNI exports: 22 Java native declarations, 22 unmangled Java_* exports, OK
    ==> signed, cert SHA-256 869b0a27...
    APK 999,151 bytes · libhg_harness.so 1,841,536 bytes
    NOTE: not installed, no permissions granted, no capture started.

That is the honest baseline: consent gating, per-payload accounting, bounded queues,
camera, microphone capture and mutual-TLS transport already exist and compile. A parallel
CCTV app would duplicate all of it. **Extend `apk/`.**

## 2. The five real gaps

The harness is deliberately **foreground-activity-only**. Its manifest says so:

> "The app never starts capture on launch, and never captures in the background: leaving
> the activity stops and releases every device. **No foreground service is declared because
> no background capture is implemented.**"

That design is correct for an acceptance harness and exactly wrong for a CCTV node. The
gaps, in dependency order:

| # | gap | why it is required | what exists today |
|---|---|---|---|
| **G1** | **Foreground service** | Android 9 blocks camera/mic for *background* apps and names the foreground service as the fix. Without it the camera dies the moment the screen is off — which is the entire product. | none; single activity, `no FOREGROUND_SERVICE` permission |
| **G2** | **Speaker talkback** | Two-way audio needs a playback path. | `AAudioCaptureDevice` is **capture-only** (verified: the only AAudio user, input direction). No `AudioTrack`/output implementation exists. |
| **G3** | **arm64-v8a + targetSdk 28** | The Panasonic is arm64-v8a; the harness is armeabi-v7a / API 27. | `build_apk.sh` hardcodes `ABI=armeabi-v7a`, `API=27`, `PLATFORM=android-27` |
| **G4** | **Visible capture indicator** | Brief requirement, and a courtesy to anyone in frame. | none |
| **G5** | **Hub delivery from a real device** | HomeGuardian integration has never been proven on hardware. `ANDROID_TRANSPORT_D03.md` states it plainly: "Physical device → hub delivery: **NOT PROVEN**". | protocol conformance executed via a C++ mock client; no device run |

## 3. Toolchain reality, verified

| item | state |
|---|---|
| JDK 17 (`~/android-jdk`) | present |
| NDK `android-ndk-r26d` | present |
| `armv7a-linux-androideabi27-clang++` | present |
| **`aarch64-linux-android28-clang++`** | **present — G3's native compiler already exists** |
| build-tools | 27.0.3 (has `dx`), 34.0.0 |
| platforms | `android-27`, `android-34` — **`android-28` is NOT installed** |
| Gradle | not used and not needed |

Two consequences worth knowing before promising a schedule:

- **G3 is cheap.** The arm64 compiler is already on disk; it is a variable change in
  `build_apk.sh` plus a second ABI, not a new toolchain install.
- `PLATFORM` must stay `android-27` (or move to `android-34`), because `android-28` is
  absent. That is fine: `targetSdkVersion` is set in the **manifest**, not by the platform
  jar you compile against. Setting `targetSdkVersion=28` while compiling against
  `android-27` gets the Android 11+ while-in-use gate avoidance we want. Installing
  `android-28` via `sdkmanager` is the tidier option if the owner prefers.

## 4. Design

**Foreground service (G1).** A `Service` that owns the capture lifetime, started from the
existing consent flow, with `startForeground()` and a persistent notification. Manifest
additions: `FOREGROUND_SERVICE` permission, a `<service>` with
`android:foregroundServiceType` omitted — deliberately, because targeting API 28 means that
attribute does not exist yet (it is an API 29+/30+ construct). Targeting 28 is what keeps
this simple. The service stops and releases every device on `onDestroy` and on consent
withdrawal, matching the harness's existing discipline.

**Camera selection (rear/front).** `Camera1Bridge` already wraps the legacy
`android.hardware.Camera` path. Camera enumeration and an operator-selectable facing
(rear default, front selectable) extends it. The Panasonic reports 2 cameras and a
`LIMITED` HAL with 720p output, so settings must be drawn from the device's *actual*
supported list, not from a lookup table — the existing `PreviewSizePolicy` exists for
exactly this reason and should be reused rather than reimplemented.

**Microphone uplink.** `AAudioCaptureDevice` already does this, consent-gated.

**Speaker talkback (G2, new work).** A new playback component alongside the capture one:
an AAudio output stream (or `AudioTrack`) that is **the only new native surface**. It must
be gated on the same consent path, bounded, and must not be able to open before the
service has started. Note the device has an AOSP acoustic echo canceller
(`libaudiopreprocessing.so`, effect `7b491460-8d4d-11e0-bd61-0002a5d5c51b`) — talkback
without AEC will echo the mic back, so AEC belongs with this item, not later.

**Transport.** Reuse `ObservationTransport.java` (framed mutual TLS) rather than inventing
a protocol. The CAM-02 analysis suggests WebRTC for the eventual interactive case, but the
existing mutual-TLS framing is already implemented and tested to the protocol level, and
replacing it is a much larger change than finishing it. **Recommendation: prove hub
delivery over the existing transport first (G5), then evaluate WebRTC only if interactive
latency demands it.** Adding a second transport before the first is proven on hardware is
how projects acquire two broken transports.

**Capture indicator (G4).** A visible on-device indication while capture is live — a
notification is not sufficient on its own since a screen-off node shows nothing; the
requirement should be met at the physical layer too (e.g. an always-on notification plus
the front-facing LED if the hardware exposes one).

**Privacy and safety.** Stream, do not store: `/data` has only 975 MB free and `DCIM`
alone is 2.83 GB, so any local recording fills the device. Encrypted transport only.
Loopback-only MCP, no LAN exposure, no public endpoint.

## 5. Acceptance criteria

Ordered, each independently falsifiable:

1. `build_apk.sh` produces **arm64-v8a** APKs that install and launch on the Panasonic
   (API 28), with `targetSdkVersion=28` shown by `aapt dump badging`.
2. Service starts, `startForeground()` notification appears, and `adb shell dumpsys
   activity services` shows it as a foreground service.
3. **Screen-off survival**: with the screen off, camera frames continue to arrive for
   ≥ 30 minutes with counters advancing — the single most important test, and the one the
   harness cannot currently pass.
4. Consent withdrawal stops delivery **and** releases camera and mic handles.
5. Talkback plays through the speaker with AEC active and no audible echo of the mic.
6. Capture indicator is visible whenever capture is live.
7. Hub delivery from the real Panasonic is proven (closes G5) with a recorded,
   reproducible evidence artifact.
8. Resource budget measured and recorded: CPU %, RSS, battery drain per hour.

Criterion 3 is the gate for the whole workstream. If the foreground service does not
survive screen-off on Android 9 on this specific device, nothing else matters.

## 6. What this needs before it can start

- **Separate explicit permission to activate the camera and microphone.** Not covered by
  any approval given so far, and required before criteria 3–6 can be attempted.
- Keeping stock Android 9 (confirmed as the preferred OS; Path B found no viable
  alternative).
- The factory reset is **not** a prerequisite for any of this — the harness can be
  installed and tested on the device as it stands.

## 7. Recommended order

1. G1 foreground service + G3 arm64/API28 build — both are small and unblock everything.
2. Criterion 3 screen-off test. Stop and report if it fails.
3. G5 hub delivery from the device.
4. G2 talkback + AEC.
5. G4 indicator.
6. Only then consider whether WebRTC adds enough to justify replacing the transport.
