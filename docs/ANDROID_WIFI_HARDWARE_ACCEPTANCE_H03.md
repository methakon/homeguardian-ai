
# H-03 - Android Wi-Fi Camera and Audio Sensor: hardware acceptance

Every claim below is labelled with HOW FAR it got. Nothing here is inferred from a successful
build, and no device result is claimed without the device.

    IMPLEMENTED IN SOURCE | COMPILED | TESTED WITH A MOCK | TESTED ON PHYSICAL HARDWARE
    VERIFIED AFTER USB DISCONNECT | BLOCKED

## 0. Headline status

| item | status |
|---|---|
| Repository / branch | `feature/h03-android-wifi-node` from H-02B tip `da2d863` |
| USB / ADB discovery | **BLOCKED - DEVICE NOT ENUMERATED** (no Android vendor ID in the USB tree; `adb devices -l` empty) |
| Phone model / API / ABI | **UNKNOWN - no device to query** (must NOT be assumed; earlier docs assumed Android 8.1/API 27) |
| APK build | **COMPILED** - 882,617 B, signed, wiring verified inside the artifact |
| APK installation | **BLOCKED** - needs an ADB-authorized device |
| Camera frames / preview on hardware | **BLOCKED** |
| Microphone samples on hardware | **BLOCKED** |
| Device certificate + TLS pairing | **NOT RUN** - needs the phone's identity and the hub's real LAN IP |
| Observations over Wi-Fi | **BLOCKED** |
| Behaviour after physical USB disconnect | **BLOCKED** - the operator must unplug |
| Local TTS from device events | phrasebook + suppression ready (H-02A/B); device-event feed still needs a live device |
| Remote live CCTV viewing | **NOT YET IMPLEMENTED** (separate follow-up, section 7) |

## 1. What this milestone actually changed in the app

An audit of the real APK found the defect the milestone anticipated:

**The D-03 transport was dead code.** `ObservationTransport` (a complete mutual-TLS client with
handshake, bounded queue, retry classification and acknowledgment validation) was referenced by
NOTHING. The native acquisition path authorized and COUNTED payloads; nothing ever built an
observation, and nothing ever sent one. There was also no `<service>` in the manifest.

The wiring now exists (all COMPILED, none hardware-tested):

    Camera1Bridge preview frame
      -> JNI nativeOnPreviewFrame  (gate FIRST, then measure)
         -> extract_motion_features()          [src/core/BoundedFeatures.cpp, shared with the host]
         -> push_observation(motion_fraction, motion_mean_delta)
    AAudio data callback
      -> gate FIRST, then the frame observer
         -> extract_audio_features()           [shared]
         -> push_observation(audio_energy_rms, audio_peak, audio_zero_crossing_rate)
    MainActivity tick
      -> nativeDrainObservations()             NDJSON, numeric scalars only
         -> NodeSession.pump()  -> PendingObservation -> ObservationTransport.enqueue()
            -> mutual-TLS handshake -> compact observation over Wi-Fi -> hub -> existing pipeline

Rules the wiring holds:

* Features are computed ONLY from an AUTHORIZED payload - the gate runs first in both paths, so a
  withdrawn consent cannot even be measured, let alone transmitted.
* The wire carries COMPACT NUMERIC SCALARS. No pixels, no PCM, no text derived from media, ever.
* The queue is bounded (128, matching the transport's own bound) and every drop is counted.
* Emission is interval-sampled (default 1000 ms, bounded 100-60000 ms) so a 15 fps camera cannot
  flood the link.
* A non-finite value is refused BEFORE queueing, on both sides.
* No new application, no second CCTV stack: the existing harness gained the missing link.

## 2. Verified inside the shipped APK (COMPILED + artifact-inspected)

    APK            882,617 B  (was 802,667 B in D-03.2: +79,950 B for the wiring)
    classes.dex    NodeSession, NodePanel, ObservationTransport, nativeDrainObservations present
    libhg_harness.so  extract_motion_features, extract_audio_features, push_observation present
    invoke-custom count 0  (the API 27 dex verifier rejects it)

What this proves: the code compiles for the project's target and is really inside the artifact.
What it does NOT prove: that a device runs it, that a frame arrives, or that anything is sent.

## 3. Operator setup - exact steps (nothing here can be done from this machine)

### 3.1 Attach the phone (USB is for INSTALLATION ONLY)

1. Use a **data-capable** USB cable (charge-only cables enumerate nothing).
2. Unlock the phone; open **Settings > About phone** and tap **Build number** seven times to
   enable Developer options.
3. In **Developer options** enable **USB debugging**.
4. Set the USB mode to **File transfer / MTP** (not charge-only).
5. Watch the phone: accept the **"Allow USB debugging?"** prompt for this computer.
6. Then tell me it is attached.

I will NOT bypass authentication, root the phone, or change its USB trust settings.

### 3.2 What I will run once it is attached (all read-only identification first)

    adb devices -l
    adb shell getprop ro.product.model
    adb shell getprop ro.build.version.release
    adb shell getprop ro.build.version.sdk
    adb shell getprop ro.product.cpu.abi
    adb shell getprop ro.product.cpu.abilist

The reported SDK decides the build target; the reported ABI decides the shipped `.so`. The current
build is **armeabi-v7a / API 27**, which runs on arm64 phones through 32-bit compatibility, but the
real values will be read rather than assumed.

### 3.3 Install, then grant permissions normally

    adb install -r hg_apk_build/homeguardian-harness.apk      # -r keeps app data; never uninstalls another package
    adb shell am start -n org.homeguardian.harness/.MainActivity

Then on the phone: **grant Camera and Microphone** when the app asks. The app does not record
anything until you press a start control.

### 3.4 Provision the device certificate, then pair

Reuse the D-03.1/D-03.2 split (offline CA / runtime / device keys) - never the old combined
directory. Per device:

    tools/provision_ca.sh --ca-dir <offline> --runtime-dir <runtime> --device-dir <device-keys> \
        --device-id <the id the app reports> --server-names "DNS:ingress,DNS:localhost,IP:<HUB LAN IP>"

Then copy ONLY that device's PKCS#12 and the CA bundle into the app's private storage as
`device.p12`, `device.p12.pass`, `ca.p12`, `ca.p12.pass` (app-private `filesDir`; never public
storage, never logged). The hub's `--server-names` must contain the address the phone actually
dials, or the connection is correctly refused.

### 3.5 The USB-disconnect acceptance (the operator performs step 11)

1-2 attach + ADB recognises the device; 3 install; 4 grant permissions; 5 camera preview frames;
6 microphone samples; 7 press **Start All Monitoring**; 8 the hub authenticates the phone (its
registry entry is authorized); 9 observations arrive over Wi-Fi; 10 the hub's detection pipeline
processes them; **11 the operator physically unplugs USB**; 12 the phone keeps acquiring;
13 new observations arrive after the disconnect; 14 reconnect after a Wi-Fi interruption is tested;
15 **Stop Monitoring** stops acquisition; 16 consent withdrawal blocks further delivery; 17 the app
stops cleanly. Timestamps and counters are captured before and after the unplug.

`adb disconnect` / turning off USB debugging is NOT accepted as equivalent to unplugging.

## 4. Controls the operator gets (IMPLEMENTED, COMPILED)

* **Hub link**: host + port fields, **Connect to hub** / **Disconnect**, and a connection state that
  reports CONNECTED only after the verified TLS handshake and an acknowledged delivery.
* **Status panel**: Wi-Fi state, credentials present/MISSING, camera and microphone permission
  state, transport counters (queued / sent / refused / dropped / permanent rejections), frames
  written, observations queued by this app, a **measured** camera FPS (from the native frame
  counter over wall time - never estimated), battery percentage and charging state, and thermal
  status where the API exposes it.
* **Acquisition controls** already present in the harness: grant/withdraw consent, start/stop
  camera, start/stop microphone, stop-all, front/rear camera selection.

Recording remains entirely operator-controlled: no covert activation, no hidden recording, no
permission bypass, and every start requires both the OS permission and the consent grant.

## 5. Known gaps, stated plainly

1. **No acknowledgment counter.** `ObservationTransport` parses and validates the hub's ack but
   keeps no counter for it. The panel therefore reports **frames written** and says the
   acknowledgment counter is not exposed. Calling a written frame "acknowledged" would claim more
   than the code knows. Small fix, tracked.
2. **No live preview surface yet.** Camera1 delivers preview callbacks (which is what the feature
   path consumes), but a `SurfaceView` preview for the operator's eye is not wired. The brief's
   "show camera preview" item is therefore PARTIAL.
3. **No foreground service.** Sustained screen-off operation needs one; the manifest has none.
   Adding it is an OS-version-sensitive decision that should be made and tested with the device
   present, not guessed at.
4. **No camera/audio "availability" observations yet** (the start/stop state facts). The UI shows
   them from the native stats; emitting them as observations is a small follow-up.
5. **Provisioning not run**: it needs the phone's identity (from step 3.2) and the hub's real LAN
   address.
6. **Live remote CCTV is NOT implemented** (section 7). Nothing here is an Alexa-compatible camera.

## 6. Privacy position (unchanged and re-verified by construction)

* Observations are numeric scalars: frame counts, motion fraction, RMS, peak, zero-crossing rate,
  timestamps, provenance. No imagery, no audio, no transcription.
* Nothing is transmitted or persisted that a classifier could turn back into a picture or a voice.
* The camera path reads the Y plane once for a bounded motion descriptor and drops it
  (`JNI_ABORT` - the buffer is not even copied back into Java).
* Consent gates every payload; a refusal is fail-closed (deny on an empty authorize callback).
* Device credentials live in app-private storage; their existence is checked, their contents are
  never read, displayed or logged.
* TLS verification is not negotiable: there is no disable switch, and a missing certificate means
  no connection rather than an unverified one.

## 7. Independent follow-up: live CCTV viewing (NOT this milestone)

Authenticated live video is a separate design: it needs a video path (not compact observations),
per-viewer authentication, explicit recording controls with retention limits, bandwidth management,
and an operator-visible recording indicator. It must not be published unauthenticated on the LAN.
Until that exists: `REMOTE LIVE CCTV VIEWING: NOT YET IMPLEMENTED`.

## 8. PHYSICAL DEVICE (verified 2026-10-14) - and a specification warning

The phone enumerated, ADB authorized, and the app installed and ran. **The reported model name is
spoofed and must not be used to infer anything.** Verified evidence:

| item | value | how it was verified |
|---|---|---|
| USB vendor | `0e8d:201c` **MediaTek** (NOT Samsung `04e8`) | `lsusb` |
| reported model | `S25_Ultra` | `getprop ro.product.model` (a lie) |
| manufacturer / board | `alps` / **`mt6580`** | `getprop` |
| Android | **8.1.0 / API 27** | `getprop ro.build.version.{release,sdk}` |
| ABI | **armeabi-v7a, armeabi** | `getprop ro.product.cpu.abilist` |
| faker properties | `lys.faker.ramrom=[1,16]`, `persist.sys.fakeandroidversion=15.0`, `lys.camera.hidehdr` | `getprop | grep -i lys` |
| REAL RAM | **MemTotal 984,792 kB (~962 MB)** | `/proc/meminfo` (kernel cannot lie) |
| REAL storage | **12,686,848 KiB (~12.1 GB)** on `mmcblk0p23` | `/proc/partitions` |
| Camera 0 | **preview `yuv420sp` (NV21)**, 640x480 available, fps 15/24/30 | `dumpsys media.camera` |
| Sensors present | accelerometer, light, proximity (MTK) | `dumpsys sensorservice` |

So the truthful compatibility target is **API 27 + armeabi-v7a on an MT6580 with ~1 GB RAM**, which
is what this build targets - and the "S25 Ultra" name, "16 GB RAM" and the Android-15 faker value
must never appear in a capability claim. `dumpsys media.camera` reporting `yuv420sp` validates the
NV21 assumption in the feature path.

### 8.1 What passed on the physical device

* `minSdk=27`/`targetSdk=27` (manifest) - **compatible** with the device's API 27.
* Packaged ABIs: `lib/armeabi-v7a/*` only; **0** arm64/x86 entries.
* Fresh APK **883,891 B**, signed; `aapt` badging: `org.homeguardian.harness` v0.2.0, sdk 27,
  `native-code: 'armeabi-v7a'`, permissions CAMERA + RECORD_AUDIO.
* **Install: `Success`** (`adb install -r`; nothing uninstalled, app data kept).
* **Launch: clean.** `HGNode: identity set: node_id=hg-443a03b54537c21f name="Node S25_Ultra"
  platform=android api=27 abi=armeabi-v7a` then `Ready. Nothing is capturing. Grant consent and
  press Start.` - the native library loaded and the node is idle by design. FATAL/ANR count **0**.
* Permissions CAMERA and RECORD_AUDIO report `granted=true`.

### 8.2 Hub side verified with THIS device's certificate (before the phone dials)

The ingress runs as the documented least-privilege container (`--cap-drop ALL`,
`no-new-privileges`, 128 MB, only 8443 published) and logs
`hg_device_ingress ready: bind=0.0.0.0 port=8443 registry_devices=1 paired=1 tls=mutual(min 1.2)`.

From the compile container, a client presenting **this device's** certificate:

    client: connected, hub CN=homeguardian-hub fingerprint=21c1c8b7e1e3c65b...
    client: welcome for device hg-443a03b54537c21f
    client: resuming at sequence 1 (hub watermark 0)
    client: seq=1 accepted=1 reason=ok   (seq 2 and 3 likewise)
    client: done accepted=3 rejected=0

and the same certificate with a DIFFERENT claimed device id:

    client: hub refused the hello: reason=identity_mismatch

That is the security property demonstrated, not asserted: **the device identity comes from the
certificate, not the payload.** The durable replay watermark advanced to
`last_sequence: 3` for this device. No alert/event was created, which is correct: the
`evidence_detector_ids` allowlist is deliberately EMPTY, so device observations are recorded and
audited but cast no fusion vote.

**Correction to my own documentation:** section 3.4 previously showed
`--device-id <id>`, which is NOT a flag this script has. The real interface is subcommands:
`tools/provision_ca.sh init --server-names "..."`, then `issue <device-id>`, then
`show <device-id>` (prints the credential id the registry needs). The device CERTIFICATE lands in
`<runtime-dir>/devices/<id>.crt` while only the PRIVATE KEY stays in `--device-dir` - which is the
design, and is why the certificate must not be looked for beside the key.

### 8.3 Operator steps that remain (the phone is locked and only the owner can act)

1. **Unlock the phone.** The app is running but behind the keyguard; nothing can be verified
   through the lock screen, and I will not attempt to bypass it.
2. Open **HomeGuardian Node** and press **Import credentials** (the four files are already staged
   in `Android/data/org.homeguardian.harness/files/hg_provisioning/`; the app copies them into
   private storage and deletes the source copies).
3. Enter host **192.168.1.6** and port **8443**, press **Connect to hub**.
4. Press **Grant consent (camera + microphone)**, then **Start camera**, then **Start microphone**.
5. Leave the phone awake on Wi-Fi (`Airtel_Dhar`, got `192.168.1.4`).

I will then verify: camera callbacks and a rising frame counter, microphone samples, and
observations ACCEPTED by the ingress over Wi-Fi - and only after that will I ask for the physical
USB unplug.

### 8.4 Still NOT verified (no claim is made)

* No frame with real pixels, no audio sample, no camera/mic figure on the device yet.
* Nothing from the phone has reached the hub over Wi-Fi yet.
* USB-unplug behaviour, reconnect, screen-off/foreground-service behaviour: untested.
* Remote live CCTV: **NOT YET IMPLEMENTED** (this transport carries compact scalars).

## 9. CRITICAL DEFECT: UnsatisfiedLinkError on the first UI tick (fixed)

### 9.1 The defect and its root cause

On the device the app died on its first periodic tick:

    java.lang.UnsatisfiedLinkError: No implementation found for
      org.homeguardian.harness.MainActivity.nativeDrainObservations()
      at MainActivity$1.run(MainActivity.java:97)

Root cause, exactly as the brief located it: the two H-03 entry points were appended AFTER the
file's `extern "C"` region, which opens at line 260 and closes at line **593**. The functions sit at
lines **601** and **622**, i.e. outside it, so they compiled with **C++ linkage** and their symbols
were mangled to forms like

    _Z52Java_org_homeguardian_harness_MainActivity_nativeDrainObservationsP7_JNIEnvP8_jobject

JNI looks up the plain name `Java_..._nativeDrainObservations`, finds nothing, and throws on the
first call. Both functions are now inside their own `extern "C"` region; method names were NOT
changed.

### 9.2 Why this shipped, and the verification that was wrong

The wiring was previously reported as "verified inside the shipped artifact" on the basis of
`strings classes.dex | grep -c nativeDrainObservations`. **That checks the Java DECLARATION, which
is present in the DEX whether or not the native side can ever satisfy it.** It was the wrong
artifact for the claim. The artifact that proves JNI linkage is the ELF's dynamic symbol table, and
the build now reads exactly that.

### 9.3 The gate (build-time, ELF-based)

`scripts/verify_jni_exports.sh` compares every Java `native` declaration with the unmangled symbols
**actually defined** in the compiled `libhg_harness.so`, using the NDK's `llvm-nm`. `apk/build_apk.sh`
runs it before packaging, so a mangled or missing entry point fails the BUILD, not the phone. It can
also be run on its own:

    bash scripts/verify_jni_exports.sh hg_apk_build/lib/libhg_harness.so \
         apk/src/org/homeguardian/harness $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm

Current result: **16 Java native declarations, 16 unmangled exports, exact match.**

    Java_org_homeguardian_harness_MainActivity_nativeDrainObservations          PRESENT (unmangled)
    Java_org_homeguardian_harness_MainActivity_nativeSetObservationIntervalMs  PRESENT (unmangled)
    (no _Z...Java_* mangled variants remain in the .so)

The gate is deliberately NOT vacuous: it fails when it finds ZERO declarations (a broken pattern
must not read as a pass) and it fails against a library with no JNI entry points - both cases were
exercised.

### 9.4 Reachability of the second entry point

`nativeSetObservationIntervalMs` is called by the app itself at identification and by an
**Apply observation interval** control in the panel (the native side clamps to 100-60000 ms and
returns what it applied), so neither new entry point can hide behind an untested path.

### 9.5 Physical status after the fix

    APK            884,425 B, signed, lib/armeabi-v7a only (0 arm64/x86 entries)
    reinstall      adb install -r -> Success (app data preserved; nothing uninstalled)
    launch         process alive, native library loaded, node identity reported
    faults         UnsatisfiedLinkError 0 | No implementation found 0 | FATAL EXCEPTION 0
                   signal 11 0 | signal 6 0 | ANR in 0

**The 30-second ticker proof is NOT yet established and is not claimed.** The app's ticker posts in
`onResume` and is removed in `onPause`; the device is currently on the **lockscreen**
(`mDreamingLockscreen=true`, activity `state=STOPPED, visible=false`), so `onResume` never runs and
the tick never executes - confirmed by the decisive counter: the tick's own log line
(`observation interval:`) appears **0** times. With the screen merely woken but still locked, that
counter stays 0, and it is the only honest proof that the tick path works.

**Operator action required: unlock the phone and keep it awake.** Waking the screen is not enough;
the lockscreen must be dismissed by the owner, and this agent does not bypass it. Once unlocked, the
tick runs every 500 ms, and the expected evidence is a non-zero count of the interval line with
zero faults - after which the remaining H-03 acceptance steps (import credentials, connect, consent,
camera/microphone, Wi-Fi delivery) can proceed.

### 9.6 30-second stability: PASS (with the measurement pitfalls recorded)

The JNI fix is verified at runtime, not just in the symbol table. Filtering the log by the app's own
tag (see the pitfall below):

    10-14 20:27:15.767  20412  I HGNode : tick #20
    10-14 20:27:26.019  20412  I HGNode : tick #40
    10-14 20:27:36.264  20412  I HGNode : tick #60
    10-14 20:27:49.527  20412  I HGNode : tick #80   (after a background -> foreground cycle)

* The tick posts every 500 ms; the heartbeat fires every 20 ticks, and the gaps are 10.25 s, i.e.
  **exactly 20 x 500 ms** - the loop runs at the coded rate, repeatedly, for the whole run.
* Process alive for the entire run and still alive after backgrounding and returning
  (the ticker is removed in `onPause` and re-posted in `onResume`).
* `UnsatisfiedLinkError` **0**, our app's `FATAL EXCEPTION` **0**, `signal 11` **0**, `signal 6` **0**.
* Both new entry points resolved at runtime: the tick calls
  `nativeSetObservationIntervalMs(1000)` (logs `observation interval: 1000 ms`) and, on the same
  pass, `nativeDrainObservations()` - the exact call that produced the crash before the fix.

Heartbeat line added for this proof (diagnostics only, no behaviour change: the tick already ran
every 500 ms). APK 884,530 B, JNI gate 16 declarations / 16 exports, `adb install -r` Success.

#### Two measurement traps found here (both produced FALSE results first)

1. **A full-buffer `logcat | grep` reports false negatives on this device.** SurfaceFlinger logs
   BufferQueue lines at ~56 fps, so the ring buffer rotates in seconds and an app line from 45 s ago
   is already gone. The decisive counter read `0` twice for this reason and briefly looked like a
   dead ticker. Filter by the app's tag (`logcat -d -s HGNode`) and dump soon after the event.
2. **`uiautomator dump` crashes on this device.** It died with
   `FATAL EXCEPTION IN SYSTEM PROCESS: UiAutomationService ... already registered!` from
   `com.android.commands.uiautomator` - a tooling crash, NOT the app's (our app's FATAL count is 0).
   It also returns an empty tree while a stale session exists, which is why the panel scrape came
   back blank. Do not use UI scraping as the sole evidence here; the heartbeat exists for that
   reason. `mCurrentFocus` confirmed the app's window owned the screen.

## 10. Consent lifecycle defect: repeated grants made the gate refuse capture

### 10.1 The symptom and the actual denial reason

On the phone: consent appeared GRANTED in the UI, both Android permissions were granted, and
`mic: initialize denied by gate` (camera frames 0, microphone samples 0).

The gate's reason, from `ConsentGate::check()`:

    DenyReason::ambiguous - "multiple active grants for the same purpose"

### 10.2 Root cause

Two correct-looking pieces combining badly:

    ConsentGate::check() step 4:
        if (active_grants > 1)  ->  DenyReason::ambiguous   (fails closed: refuses to guess)

    MemoryConsentRepo::grant()   (Android JNI):
        save(ConsentRecord::create_granted(...))   // APPENDS a granted record on EVERY press

So the SECOND press of "Grant consent" produced a second active grant for the same (subject,
purpose), and from then on the gate refused the ambiguity for ever. The gate was right; the record
store feeding it was wrong. Note the gate's ordering also means a withdrawal always wins: it returns
`decision_not_granted` before it ever counts grants, so a withdrawn purpose can never be authorized
by a leftover grant.

### 10.3 Fix

`src/persistence/SingleActiveGrantConsentRepo.h` (new, header-only, stdlib-only so it compiles
identically in the host suite and in the Android JNI):

* **Invariant:** at most ONE `granted` record per `(subject_scope, purpose)`, whatever the click
  sequence.
* `grant(...)` supersedes (removes) any existing granted record for that (subject, purpose) and
  appends exactly one fresh grant. **Idempotent**, and it returns how many it superseded.
* `withdraw(...)` removes EVERY granted record for that (subject, purpose) and appends one
  withdrawn record, so a later withdrawal cannot leave an earlier grant effective, and the
  withdrawal remains as the audit record.
* Purposes stay independent - granting camera never authorizes microphone.

The JNI now uses it in place of its append-on-press store (`using MemoryConsentRepo =
SingleActiveGrantConsentRepo;`), so the call sites are unchanged.

**The fail-closed gate is NOT weakened.** `ConsentGate` was not modified. A test deliberately
bypasses the lifecycle (raw `save()` of two grants) and asserts the gate still reports `ambiguous`,
so this defect class cannot be "fixed" later by deleting the check.

### 10.4 Denial reason exposed to the operator (fix item 1)

Every denial now carries the gate's sanitized reason and explanation, in the returned string and in
the HGNode log - for camera and microphone, initialize and start:

    camera: initialize denied by gate [reason=no_consent_record detail=no consent record for subject+purpose]
    mic: initialize denied by gate    [reason=ambiguous detail=multiple active grants for the same purpose]

A new native `nativeConsentState()` is polled every tick and shown in the panel plus logged, so the
reason is visible without pressing anything:

    consent: camera=no_consent_record (no consent record for subject+purpose) | microphone=no_consent_record (no consent record for subject+purpose)

It contains only the gate's closed-vocabulary reason word and its fixed explanation - no
credentials, no media, no device identity.

### 10.5 `wi-fi: unknown` - diagnosed, not guessed

`NodePanel.wifiState()` caught a `RuntimeException` and returned a bare `"unknown"`, hiding the
cause. The cause was a missing **normal** permission: `ConnectivityManager.getActiveNetworkInfo()`
requires `ACCESS_NETWORK_STATE`, which the manifest never declared, so it threw
`SecurityException`. The manifest now declares it (verified inside the built APK with
`aapt2 dump permissions`), and the catch names its cause instead of hiding it
(`unknown (ACCESS_NETWORK_STATE not granted)` / `unknown (SecurityException)`).

The **SSID is deliberately not read**: on API 27+ that requires a location permission, which is a
privacy trade the owner has not agreed to. The panel reports connectivity state and transport type
instead.

### 10.6 Tests

New `tests/core/test_consent_lifecycle.cpp`, tag `[consent_lifecycle]`: **13 cases / 45 assertions**.
Covered: single grant authorizes; **5 and 100 repeated grants still authorize with exactly one
active grant** (the H-03 regression); `grant()` idempotence and supersede count; **the gate still
refuses two genuine active grants as `ambiguous`**; withdrawal removes every granted record and
denies; withdrawal is final even when the whole history is replayed into a naive store; repeated
withdrawals stay denied; consent can be granted again after withdrawal; camera/microphone
independence; a category the grant does not cover is refused; the global capture switch overrides
any grant; concurrent presses from 4 threads keep the invariant.

Suite totals: **477 cases / 5,609 assertions**, all passing (was 464/5,564 before this milestone).

## 11. BLOCKING DEFECT FIXED: legacy Camera1 path was unreachable behind a failing NDK enumeration

### 11.1 The defect, and the evidence from the device

The operator's physical attempt failed with

    camera: initialization failed (NdkCameraDevice: no camera available)

because `nativeStartCamera()` built an `NdkCameraDeviceImpl` UNCONDITIONALLY and required its
`initialize()` (an `ACameraManager_getCameraIdList()` enumeration) to succeed before the real
`Camera1Bridge` path - `Camera.open()`, NV21 preview callbacks, per-frame native delivery gate - could
run at all. Treating the NDK device as if it were the Camera1 acquisition device made a working camera
unreachable.

Both availability views, from the device and read-only:

    adb shell dumpsys media.camera  ->  "Number of camera devices: 2"   (Camera device 0 and 1 present)
    the app's recorded NDK path     ->  "NdkCameraDevice: no camera available"  (enumerates 0)

The platform has two cameras; the NDK camera2 API cannot see them on this MT6580 / API 27 unit. A
camera count is not capture, and neither enumeration is evidence that frames are flowing - only
preview callbacks and rising counters are (section 11.5).

### 11.2 Fix

| piece | what changed |
|---|---|
| `src/backend/android/Camera1Device.{h,cpp}` (new) | a native lifecycle representation of the **Java Camera1 bridge**, implementing the same `IMediaDevice` contract as the NDK backend, so `ConsentGuardedDevice`, per-frame authorization and capture accounting work unchanged. It owns no Android handles (the bridge owns Camera/SurfaceTexture). Deliberately **Android-free**, so it is unit-tested on the host rather than asserted in prose |
| backend selection | `nativeStartCamera(cameraId, backend)` - explicit, from the Activity: `"camera1"` (this device), `"camera2"` (the NDK backend, preserved), `"auto"` (a deterministic rule, not a hardware probe). **A successful NDK enumeration is never a precondition** for the Camera1 path |
| `nativeCameraReport(count)` | the Activity reports what the LEGACY api sees (`Camera.getNumberOfCameras()`); `Camera1Device::initialize()` refuses when it is 0 |
| `nativeCameraStartResult(ok, detail)` | the Activity reports the outcome of `Camera.open()`/`startPreview()`. **On failure it forces stop+close and clears the video flag**, so a failed Java start can never leave `video=on` |
| `nativeVideoRunning()` | the Activity's reconciler asks the native layer whether capture is still on, and releases the Camera1 resources when it is not (stop, consent withdrawal, camera-permission revocation, activity pause) |
| `CaptureAccountant::note_capture_stopped(bool video)` (new overload) | clears ONE kind only: stopping the camera no longer marks the microphone stopped |
| `nativeStopCamera()` | now clears the video flag too - previously `video=on` outlived a camera stop |

Ordering, deliberately: the **native gate authorizes first** (permission + consent + lifecycle) and only
then does the bridge open the camera, so capture can never begin before consent is checked. Frames are
still authorized per payload, and a refused frame stops preview and releases the camera (existing
behaviour, unchanged).

Direction of the reconciler matters: **Java asks the native side, never the reverse**, so no native lock
is ever held while calling into Java (the callback path already avoids that).

### 11.3 Tests

`tests/android/test_camera1_device.cpp`, tag `[camera1]` - **9 cases / 46 assertions**: initialize
refuses before a count is reported; refuses with a reported count of 0; the full
Initialized -> Capturing -> Stopped lifecycle; repeated start/stop cycles; **a failed Java start can
never leave the device Capturing**; stop/close idempotent; start from Idle/Error refused; the consent
guard gating this backend (no consent refused, grant allowed, **withdrawal forces the device out of
capture**, OS permission revocation likewise); consecutive grants still authorize.

Note on tags: `[camera1]` already matched 6 pre-existing `Camera1Bridge` adapter cases, so "All tests
passed" was not proof that the new file ran. The proof is the **delta**: `[camera1]` went 6 -> **15
cases**, and the full suite 477 -> **486 cases** / 5,609 -> **5,652 assertions**.

### 11.4 Build and install

* Host suite: **486 cases / 5,652 assertions**, all passing; ctest green.
* APK **965,451 B**, `lib/armeabi-v7a` only, JNI export gate **21 declarations / 21 unmangled
  exports**, with `nativeCameraReport`, `nativeCameraStartResult`, `nativeCameraBackend`,
  `nativeVideoRunning` and the new `nativeStartCamera` all confirmed in the packaged `.so`.
* `adb install -r` -> **Success**, app data preserved; clean launch, 0 UnsatisfiedLinkError / FATAL /
  SIGSEGV; the startup line now reports `camera availability: legacy(Camera1)=2`.

### 11.5 Physical acceptance - what counts as proof (NOT yet claimed)

Enumeration is not capture. Capture is accepted only when, with operator consent and the rear Camera1
device started: real `onPreviewFrame` callbacks occur, `frames_seen` / `frames_authorized` /
`frames_processed` all RISE, real motion observations are emitted after authorized frame processing,
repeated start/stop works, camera and microphone co-exist, and withdrawal/activity pause stop delivery
immediately with no native crash, deadlock or leaked camera. `scripts/verify_h03_physical_acceptance.sh`
checks exactly those from the phone's log and the hub's watermark, and reports NOT OBSERVED (a failure)
for anything it cannot see.

The NDK camera2 backend is **preserved but NOT hardware-verified**: its capture-session implementation
is incomplete and this device cannot exercise it. No claim is made that it works on hardware.

## 12. FIXED: 518 authorized frames produced zero processed frames (preview geometry outside the bounds)

### 12.1 The measured evidence and the exact rejection condition

The operator's log, from the real device:

    frames=518/518  dropped=0  frame_bytes=322237440
    frames_processed=0  obs_emitted=0  video=off  samples=0  transport=not-connected
    consent: camera=none | microphone=none          (both AUTHORIZED)

322237440 / 518 = **622,080 bytes per frame**, and 720 x 576 x 1.5 = 622,080 exactly: the platform
negotiated a full **720x576 NV21** preview. So the byte accounting is CORRECT - there is no dimension
discrepancy. The rejection was in the feature gate:

    nv21_geometry_ok(w, h, bytes) && pixels >= FeatureBounds::kMinPixels
                                  && pixels <= FeatureBounds::kMaxPixels

with `kMaxPixels = 640 x 480 = 307,200`. 720 x 576 = **414,720 pixels > 307,200**, so the entire block -
including `++n.frames_processed` and the motion extraction - was skipped for EVERY authorized frame.
Acquisition was genuine; processing never ran; nothing was logged because the skip branch did not exist.

The bounds were NOT relaxed, and are not: they cap CPU and allocation on a ~1 GB device.

### 12.2 Fix

| piece | what changed |
|---|---|
| `src/core/PreviewSizePolicy.{h,cpp}` (new) | the geometry policy, host-testable: `classify_geometry()` / `geometry_within_feature_bounds()` (ONE definition, reading `FeatureBounds`) and `choose_processable_preview_size()` - the closest SUPPORTED size inside the bounds, deterministic tie-break (smaller area, then smaller width) |
| `Camera1Bridge` | asks the native policy (`nativeChoosePreviewSize`) which supported size to negotiate. When **nothing** is processable it REFUSES to start and names the geometry it needs and the sizes the platform offered - it never negotiates a size the pipeline would silently skip |
| JNI frame path | uses `geometry_within_feature_bounds()` instead of an inline comparison, counts every skip **by reason**, logs the geometry once (and on change), and logs skips **rate-limited** (first + at most every 10 s) |
| counters line | now carries `proc_skip_geom=`, `proc_skip_below_min=`, `proc_skip_above_max=` and `last_geo=WxH`, so a skipped frame can never be invisible again |

**Deliberately NOT done: silently downsampling an oversized frame.** Sampling a 720x576 buffer into the
bound would change what `motion_fraction`/`motion_mean_delta` MEAN - a semantic change hidden behind a
size fix. The camera is asked for a processable size instead, and an unprocessable delivery is refused
loudly.

### 12.3 Tests

`tests/core/test_preview_size_policy.cpp`, tag `[previewsize]` - **5 cases / 43 assertions**: the bound
is exactly 640x480; **the measured 720x576 geometry is asserted NON-processable** and 622,080 B/frame is
asserted as 720x576x1.5 (so the arithmetic cannot drift); the policy picks the closest supported size
inside the bounds; a platform offering only oversized sizes yields NO choice (the caller must refuse);
ties are order-independent; and a property check that whatever the policy returns is always processable.

Full suite: **491 cases / 5,698 assertions**, all passing (was 486 / 5,652).

### 12.4 Build

APK **997,465 B**, armeabi-v7a only, JNI export gate **22 declarations / 22 unmangled exports** with
`nativeChoosePreviewSize` confirmed in the packaged `.so`; `adb install -r` -> Success, data preserved.

### 12.5 Physical acceptance - anticipated outcomes, both named

Still PENDING the operator's next Start camera. The next run reports the negotiation explicitly, and
there are exactly two outcomes:

* **a processable supported size exists** -> `camera preview size: target=... chosen=WxH from N supported`
  and then rising `frames_processed` with real `motion_fraction` / `motion_mean_delta` observations
  (acceptance met), or
* **none is processable** -> `camera preview size: NO supported size lies inside the feature bounds ...
  offered=<list>` and the bridge refuses with `camera1: no supported preview size inside the processing
  bounds (needs >= 16 and <= 640x480 pixels); platform offers: <list>`. That list decides the next step
  (pick a spinner size that is processable, or implement bounded processing as a separate, reviewed
  change) - it does NOT license relaxing the bounds.

Sensor acquisition is verified independently of the transport: `frames_processed` and the motion
observations come from the camera path, while `transport=` is a separate set of counters.

