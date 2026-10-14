
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
