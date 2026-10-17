# HomeGuardian AI — Distributed Deployment Guide

Status: **D-01 (Docker hub) implemented**; D-02…D-06 planned. See
`docs/RELEASE_CANDIDATE_V21.md` for the simulator-first release candidate, which
remains the tested fallback for the hackathon.

---

## 0. What changed, and what did not

The project owner changed the target architecture: the simulator-only web demo is
no longer the intended final product. The target is a **central C++20 hub in
Docker** plus **old Android phones as real camera/microphone/speaker/sensor
nodes**, with genuine Amazon Alexa integration if an officially supported runtime
exists.

What this means in practice:

| Area | v21 (fallback, preserved) | Distributed target |
|------|---------------------------|--------------------|
| Sensing | synthetic simulator only | real Android capture nodes (D-02) |
| Hub | one native process on the host | the same native process in a container (D-01) |
| Transport | in-process | authenticated, encrypted LAN protocol (D-03) |
| Devices | one | device registry, many nodes (D-04) |
| Observations | synthesised features | features derived from real frames/samples (D-05) |
| Voice | local `AlexaContract` + `src/web` "simulated Alexa+" UI (clearly labelled) | genuine Amazon Alexa if an official runtime/route is available (D-06) |

Unchanged and reused as-is: the consent gate, privacy audit, observation/event
model, detection and fusion rules, `AlertPolicy`, SQLite persistence (WAL +
`synchronous=FULL`), the native HTTP API and the vanilla-JS dashboard.
**No parallel event engine is introduced anywhere.**

The v21 simulator tests are deliberately kept: they are the reproducibility
baseline and the regression net for every milestone below.

---

## 1. D-01 — Docker hub

### 1.1 Files

```
deploy/docker/
├── Dockerfile                     multi-stage: build → test → runtime
├── docker-compose.yml             service, volume, secret, limits, healthcheck
├── docker-compose.loopback.yml    opt-in 127.0.0.1-only port publish (local use)
├── entrypoint.sh                  validates mounts, execs the hub (PID 1)
├── healthcheck.sh                 /dev/tcp probe of /api/health (no curl)
├── config/hub.json                container configuration (data_dir=/data)
└── secrets/creds.json.example     synthetic demo credential template
/.dockerignore                     build-context exclusions (build trees, .git, dbs, logs)
```

### 1.2 Build and run

```bash
cd deploy/docker
cp secrets/creds.json.example secrets/creds.json     # synthetic demo token only
docker compose build                                  # or: docker build -t homeguardian-hub:local ../..
docker compose up -d
docker compose ps                                     # health: healthy
docker compose logs -f hub
docker compose down                                   # alerts stay in the named volume
```

Prove the existing suite passes **inside the container toolchain**:

```bash
docker build --target test -f deploy/docker/Dockerfile .
```

### 1.3 Base image (and why it is pinned)

`debian:trixie-slim` for **both** stages. The project's `CMakeLists.txt`
requires CMake >= 3.28.3; Debian bookworm ships 3.25.1 and fails at configure
(observed: `CMake 3.28.3 or higher is required. You are running version 3.25.1`),
while trixie ships 3.31.6. Builder and runtime must also share a libc: a binary
linked against a newer glibc than the runtime provides cannot start. The
requirement is documented in the Dockerfile so a future base change is deliberate.

### 1.4 Persistence

`/data` is a named volume (`hg_data`) and the only writable path in the runtime
image. It holds `homeguardian_bridge_audit.db` (WAL + `synchronous=FULL`) and
`homeguardian.log`. `docker compose down` (without `-v`) keeps the volume, so a
replaced container keeps its alert history; `docker compose down -v` discards it.

Declaring `VOLUME ["/data"]` in the image means a fresh named volume inherits the
image's `hg:hg` ownership, which is what lets the container run as uid 10001 with
no root at any point. **A bind mount does not inherit that** — `chown 10001:10001`
the host directory first.

### 1.5 Shutdown

`entrypoint.sh` ends with `exec`, so the hub is PID 1 and receives `SIGTERM`
directly; `init: true` in Compose adds a minimal init that forwards signals and
reaps orphans. `stop_grace_period: 30s` gives SQLite room to close cleanly. The
hub's own shutdown path is the one verified in v21 (clean exit on SIGTERM).

### 1.6 Restart behaviour (documented)

- `restart: unless-stopped` — the container comes back after a crash or a host
  reboot, and stays down after an explicit `docker compose stop`.
- `HEALTHCHECK` failure marks the container `unhealthy` but does **not** restart
  it by itself; `restart:` policy is what restarts. Inspect with
  `docker inspect --format '{{.State.Health.Status}}' <container>`.
- Because alert storage is a volume, a restart never loses alert history, and the
  documented AlertPolicy semantics still hold: dedup/cooldown state is
  per-process, so a fresh process may re-deliver a condition it already alerted
  on before the restart (the history records both).

### 1.7 Security and isolation (D-01 configuration)

| Control | How |
|---------|-----|
| Non-root | image runs as uid/gid `10001`; no root step at runtime |
| Everything dropped | `cap_drop: [ALL]`, `security_opt: no-new-privileges:true` |
| No privileged mode, no device passthrough | neither `privileged:` nor `devices:` is set |
| No Docker socket | `docker.sock` is never mounted |
| Secrets | credentials are mounted read-only from a Compose secret at `/run/secrets/hg_creds`; **no credential is ever copied into an image layer** (`.dockerignore` also excludes `deploy/docker/secrets/creds.json`) |
| Config | `config/hub.json` is mounted/baked read-only; runtime overrides come from `HG_*` environment variables |
| Bounded resources | `mem_limit 256m`, `memswap_limit 256m`, `cpus 1.0`, `pids_limit 128` |
| Log rotation | `json-file` with `max-size 10m`, `max-file 3` |
| Network exposure | **no published ports.** The hub's HTTP server binds `127.0.0.1` inside the container and was security-reviewed for loopback only |

#### Why nothing is published

The handwritten HTTP server in this project is deliberately loopback-only
(reads are unauthenticated by design for local diagnostics; state-changing endpoints require authenticated session and CSRF token); it is not an open network service. Publishing it
would expose it to the LAN, so D-01 does not. A genuinely network-facing
boundary is milestone **D-03** (authenticated, encrypted transport with pairing,
revocation and replay protection) and will be reviewed and measured on its own.

For local viewing on the machine you are sitting at, an opt-in override binds the
**host** side to loopback:

```bash
docker compose -f docker-compose.yml -f docker-compose.loopback.yml up -d
# http://127.0.0.1:8080/  — still unreachable from the LAN
```

Do not change that bind address to `0.0.0.0`; that is an architectural decision
reserved for D-03.

### 1.8 Measured results (D-01, this host)

Build (BuildKit, `-j4`):

| Stage | Result | Time |
|-------|--------|------|
| `docker build --target test` | builds every target and runs the existing suite **inside the container**: `100% tests passed, 0 tests failed out of 1` | ~13 min cold (apt + FetchContent + full compile + ctest) |
| `docker build --target runtime` | succeeds; assembles the runtime image from the cached build stage | <1 min warm |

Image and runtime footprint:

| Metric | Measured |
|--------|----------|
| Runtime image (uncompressed) | **120.6 MiB** — ~117 MiB is the `debian:trixie-slim` base, **~9 MiB is this application** (2.99 MiB `hg_alexa_bridge`, 2.88 MiB `homeguardian`, entrypoint/healthcheck/config) |
| Compressed pull size (`gzip -1` of `docker save`) | **31.0 MiB** |
| Test image (build-only, not shipped) | 2.9 GiB |
| Idle container memory (`docker stats`) | 3.4 MiB / 256 MiB limit |
| Idle CPU | 0.01 % |
| Hub process inside the container | pid 7, **1 thread**, VmRSS = VmHWM = 6,268 kB (no growth after the scenario runs) |
| Startup → healthy | within the 10 s start period (≈3 s typical) |
| Runtime dependencies | Debian base + `bash`; no Node.js, no Python, no HTTP client, no curl |
| Process/threads in the image | one hub process (plus Compose's init) |

`scripts/verify_docker_hub.sh` is the acceptance gate — it drives all of the
above against a real daemon and exits non-zero on any failure. Last run:
**26 passed, 0 failed, 0 warned** (health, real-core responses, persisted alerts,
survival across container replacement, clean SIGTERM, entrypoint fail-closed
without mounts, no published ports, non-root, caps dropped, no-new-privileges,
not privileged, no devices, no docker.sock, secret-mounted credentials, WAL
header in the volume, loopback-only listener, footprint).

Trimming note: `/usr/share` totals only 5.6 MiB (docs 1.4 MiB, zoneinfo 2.0 MiB),
so aggressive stripping would save ≈4 MiB while removing licence texts — not
worth it. A distroless/static variant could reach ~25–30 MiB uncompressed but
would lose the shell-based entrypoint and health probe; recorded as an option,
not taken (it would add new code for no functional gain).

### 1.9 Using the hub

Everything the dashboard shows comes from the real backend inside the container:

```bash
docker compose exec hub bash -lc 'cat < /dev/null > /dev/tcp/127.0.0.1/8080'   # liveness
docker compose exec hub /usr/local/bin/hg-healthcheck && echo healthy
# query the real pipeline status:
docker compose exec hub bash -lc 'exec 3<>/dev/tcp/127.0.0.1/8080;
  printf "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" >&3; cat <&3 | tail -1'
# Note: /api/run-scenario requires authenticated POST with CSRF (see HG-SEC-01)
```

`SIMULATION MODE — synthetic data` is the honest banner for D-01: the container
has no camera or microphone. Real capture arrives with the Android nodes (D-02)
and is labelled `REAL LOCAL CAPTURE ACTIVE` only when real frames are flowing.

---

## 2. D-02 — Android acquisition node (implemented; physical capture BLOCKED)

`apk/` is the node application. One APK targets both the documented older device
(Android 8.1 / API 27 / armeabi-v7a, Camera1 path) and newer phones (Camera2 NDK
path): the camera backend is an explicit, documented choice, never a silent
fallback.

### 2.1 What was implemented

| Piece | Detail |
|-------|--------|
| `apk/src/.../MainActivity.java` | Node UI: device identity + display name, consent grant / **withdraw**, camera facing (back/front), preview size (320x240 / 640x480 / 854x480) and FPS (5/10/15), microphone rate (8000/16000/44100) and channels (mono/stereo), Start/Stop for each device, a live **RECORDING** banner, live counters and the last refusal reason, runtime permission flows, and a non-capture self-test |
| `apk/src/.../Camera1Bridge.java` | Camera1 acquisition: facing/size/FPS selection with the closest supported values, wall-clock timestamps per frame, a local frame count, per-frame delivery gate, deterministic release, safe to call from any thread |
| `apk/src/.../DeviceIdentity.java` | Non-personal identity inputs: Android ID (hashed natively), display name, API level, primary ABI, model. No accounts, contacts, IMEI, location or media content |
| `apk/jni/hg_harness_jni.cpp` | 12 JNI entry points, all exception-safe (a C++ exception must never cross the boundary): identity, permission mirror, consent grant/withdraw, camera start/stop, mic start/stop + stop-request poll, stats, stop-all, self-test. Owns one `CaptureAccountant` and one `ConsentGate` |
| `src/core/CaptureAccounting.{h,cpp}` | The bounded/authorized payload contract both driver threads call: dimension/size/rate/channel/timestamp validation, saturating counters, drop reasons, thread-safe (`counters()` returns a copy) |
| `src/core/NodeIdentity.{h,cpp}` | Opaque node id (FNV-1a over the device seed) + sanitised, bounded display labels; the D-03 registration identifier |
| `src/backend/android/AAudioCaptureDevice.{h,cpp}` | **Completed**: the backend previously opened a stream but registered no data callback, so no PCM was ever read. It now sets an `AAUDIO_FORMAT_PCM_I16` stream with the configured rate/channels/frames-per-callback, registers the data + error callbacks, authorises **every** buffer through the consent gate, accounts it, tracks negotiated rate/channels, counts xruns, and raises a stop request on consent/permission refusal or a stream disconnect |

### 2.2 Acquisition rules (unchanged philosophy, now on a real device)

- Capture is **off by default**. Nothing opens on launch; the self-test performs
  no capture.
- Starting requires all three: an explicit operator tap, the Android runtime
  permission granted through the normal flow (never `adb pm grant`), and a
  HomeGuardian consent record. Any one missing = nothing captured; the native
  gate refuses regardless of what the UI does.
- **No background capture.** There is no foreground service and no
  `FOREGROUND_SERVICE` permission: leaving the activity stops and releases every
  device (`onPause`/`onDestroy`). Screen-off operation would require a foreground
  service with a persistent notification and is deliberately left as a follow-up
  (it cannot be verified without a device).
- Every frame and every PCM buffer is re-checked against consent/permission/
  capture state immediately before delivery and then validated and counted. A
  refused payload is dropped, counted with a reason, and (for audio) raises a
  stop request the control thread acts on.
- Frames/samples live in memory for the duration of the callback only: no decode,
  no feature, no file, no upload, no inference. The node does **not** recognise
  faces, speech, falls or emergencies — that claim is not made anywhere.

### 2.3 Build and verify (no device needed)

```bash
bash apk/build_apk.sh                      # native lib + dex + package + sign
bash scripts/verify_android_node.sh        # 18 software checks + device report
```

`verify_android_node.sh` asserts: the native library builds for armeabi-v7a and
exports every JNI entry point the app calls; the APK builds, aligns and
signature-verifies; the manifest declares exactly CAMERA + RECORD_AUDIO with both
hardware features optional and **no** FOREGROUND_SERVICE; the dex contains no
`invoke-custom`/`invoke-polymorphic` (the API 27 verifier rejects invokedynamic —
this is what forces the no-lambda Java style); the `[d02]` host contracts pass;
the existing host suite still passes; and it reports APK/library sizes plus the
attached-device count.

### 2.4 Measured (host)

| Metric | Value |
|--------|-------|
| APK | 794,159 B (0.76 MiB) |
| Native library (armeabi-v7a) | 1,140,532 B (1.09 MiB) |
| Bundled `libc++_shared.so` | 1,331,388 B |
| Host suite after D-02 | 387 cases / 3,053 assertions (+18 / +212 vs v21) |
| `[d02]` contracts | 18 cases / 212 assertions |
| Fresh out-of-source Release build | all 3 host targets, 0 compile errors: `homeguardian` 3,210,648 B, `hg_alexa_bridge` 3,342,280 B, `homeguardian_tests` 6,782,120 B |
| Release host suite | 387 cases / 3,053 assertions, all pass |
| ASan + UBSan host suite | 387 cases / 3,053 assertions, all pass, **0 sanitizer reports** |

Device-side figures (RAM, CPU, camera FPS, audio latency, battery, thermal) are
**NOT measured**: no phone is attached. They are not estimated here because an
estimate is not evidence.

Two honest notes on the numbers above:

- `hg_alexa_bridge` grew 3,320,856 -> 3,342,280 B (+0.65%) because the two new core
  translation units belong to the shared core library every target links. That code
  is what D-03 needs on the hub side, so the cost buys the transport milestone its
  identity and accounting rather than duplicating them per target.
- The ASan suite covers `CaptureAccounting` and the consent/guard contracts, which
  are ordinary host code. The AAudio backend cannot run on the host at all (it is
  compiled only under `HOMEGUARDIAN_ANDROID`), so for D-02 it is
  **compile-verified for armeabi-v7a, not runtime-verified**.

### 2.5 Physical acceptance — BLOCKED

`D-02 PHYSICAL CAPTURE ACCEPTANCE: BLOCKED — NO DEVICE ATTACHED`. No frame count,
sample count, battery reading or device log is reported, because none was
observed. See §2.6 for exactly what is needed to unblock it.

### 2.6 Requirements for physical acceptance

To turn `BLOCKED` into verified, the operator needs to:

1. Attach the documented device (Android 8.1 / API 27 / armeabi-v7a, ~1 GB RAM) or
   any Android ≥ 8.1 phone, with USB debugging enabled and authorised.
2. Install the built APK: `adb install -r hg_apk_build/homeguardian-harness.apk`.
3. Launch **HomeGuardian Node**, press **Grant consent**, then **START camera
   capture** and accept the CAMERA runtime prompt; repeat for the microphone with
   **START microphone capture**.
4. Confirm on screen: the RECORDING banner, a rising `frames=` count (and
   `audio_buf=`/`samples=` for the mic), the negotiated preview size/FPS, and the
   `audio=` state in the live counters.
5. Exercise the failure paths: **WITHDRAW consent** while capturing (counters
   must stop advancing and the devices must be released), deny a permission and
   press Start (nothing may open), and background the app (capture must stop).
6. Capture the supporting evidence: `adb logcat -s HGNode HGCam1 HGAudio` and
   `adb shell dumpsys media.camera | head -40`, plus a screen recording of the
   counters.

Nothing in the app will start capture without step 3: the operator action is
required by design, so physical acceptance cannot be automated from this host.

## 3. D-03 — Secure device transport (planned)

Prerequisites from D-02 are in place: each node has a stable opaque `node_id`
plus sanitised labels (`src/core/NodeIdentity.*`), and every payload already
carries a validated timestamp. D-03 adds registration/pairing, per-device
identity and authentication, and the encrypted LAN transport.

Registration/pairing, per-device identity, mutual authentication, encrypted LAN
transport (a reviewed library — **not** custom cryptography), per-device consent,
protocol versioning, sequence numbers + timestamps, heartbeat/connectivity,
bounded queues with backpressure and frame dropping, replay/malformed protection,
explicit reconnect policy, and no unauthorised media forwarding. Default is
compact derived observations; raw video/audio only when specifically required and
authorised.

## 4. D-04 — Multiple nodes (planned)

Device registry, online/offline state, per-device media enable/disable and
consent, independent streams, device-scoped event source ids, clock-skew
handling, bounded buffering, reconnect isolation, revocation, and multi-source
correlation through the existing pipeline. Starting point: two devices (or one
real + one mock).

## 5. D-05 — Real media-derived observations (planned)

Modest, explainable features computed from actually acquired data — frame change
/ motion-energy score, audio RMS/energy, signal availability, connect/disconnect,
bounded sensor readings. No claim that motion or audio energy identifies faces,
speech, falls, injuries or emergencies; no heavy inference without a measured
justification.

## 6. D-06 — Genuine Amazon Alexa integration: feasibility

**Verdict: `OFFICIAL ALEXA DOCKER RUNTIME: BLOCKED`.** No officially supported
Amazon runtime for Alexa/Alexa+ is available for a Linux Docker container, and the
Alexa+ developer programme is partner-gated. Evidence below is from Amazon's own
pages (fetched during this milestone; HTTP status recorded because several pages
have since been withdrawn).

| # | Finding | Primary source | Observed |
|---|---------|----------------|----------|
| 1 | AVS developer tools are withdrawn for new Alexa Built-in work | `https://developer.amazon.com/en-US/alexa/alexa-voice-service` | HTTP 200. Verbatim: *"Alexa Voice Service (AVS) developer tools are no longer generally available for Alexa Built-in. Please visit the Works with Alexa program if you are interested in building devices that connect to Alexa."* |
| 2 | The AVS Device SDK is retired | `https://github.com/alexa/avs-device-sdk` | *"This repo was archived by the owner on Jan 16, 2024. It is now read-only."* |
| 3 | The AVS documentation set is being withdrawn | `…/en-US/docs/alexa/alexa-voice-service/` and `…/en-GB/docs/alexa/alexa-voice-service/deprecated-features.html` | both **HTTP 404** (the deprecated-features page that previously listed *"micless Alexa Built-in … no longer supported for new product development"* (15 Feb 2024), *"AVS connected speaker"* (15 May 2023) and *"AVS integration with AWS IoT"* (15 Nov 2022) no longer resolves) |
| 4 | The AVS developer console is not a functional AVS console | `https://developer.amazon.com/alexa/console/avs` | serves Amazon's storefront interstitial (*"Click the button below to continue shopping"*), not an AVS console |
| 5 | Alexa+ add-ons are partner-gated | `https://developer.amazon.com/docs/alexaplus/add-ons/home.html` | *"At this time, Category SDK and MCP Toolkit are available to select partners only."* |
| 6 | Alexa+ for Builders is invitation-only | `https://developer.amazon.com/alexaplus/` | *"Alexa+ for Builders is currently available to select partners working directly with our team."* |
| 7 | The Alexa+ route is a cloud service add-on, not a device runtime | `…/docs/alexaplus/add-ons/overview-category-sdk.html`, `…/set-up-your-development-environment.html` | Category SDK covers service categories (restaurant reservation, food ordering, home services, local booking, ride booking, ticketing), is **US-only**, is deployed to **AWS** via the Alexa AI CLI, and TypeScript-only for Action add-ons. Nothing here runs a local hub or a Docker container. |
| 8 | No official Amazon Alexa container image exists | Docker Hub API (`hub.docker.com/v2/search/repositories/?query=alexa`, `hub.docker.com/v2/repositories/amazon/`) | 12 959 repos match "alexa"; **none** is `is_official: true`. The `amazon/*` organisation has 80 repositories and **none** is Alexa-related. |

Considered and rejected (per the mission's constraints): untrusted
Alexa-branded community images, authentication bypass, private-API scraping, or
emulating proprietary Amazon credentials.

### 6.1 What is legitimately available instead

An **official Amazon Alexa skill** (custom and/or Smart Home) is the supported,
self-service route today:

- It needs only a free Amazon developer account
  (`…/docs/alexa/smarthome/steps-to-build-a-smart-home-skill.html`,
  `…/docs/alexaplus/add-ons/set-up-your-development-environment.html`).
- It does **not** require an Echo: *"During skill development, you can test your
  skill without a device by using the Alexa simulator in the Alexa developer
  console or in Visual Studio Code"* and the testing tools are *"the Alexa skill
  simulator, the Alexa app, and the ASK Command Line Interface (CLI)"*
  (`…/docs/alexa/ask-overviews/build-skills-with-the-alexa-skills-kit.html`).
- A real Echo (or the Alexa app) is only needed for the physical voice
  experience and for certification.
- It is deliberately **not** a Docker runtime: skill logic is hosted by Amazon or
  in the developer's own HTTPS endpoint, which is why it must be an owner-approved
  architecture change rather than the requested option.

The project's existing `AlexaContract` and skill-handler prototypes are compatible
with that route and are reused as the intent/response layer.

### 6.2 Required access to unblock a genuine local runtime

To implement what the mission asks for literally (real Alexa/Alexa+ in Docker),
the owner would need, from Amazon: partner status in Alexa+ for Builders, access
to the Category SDK and/or MCP Toolkit, a supported marketplace/account profile,
and AWS hosting for the add-on. None of these is obtainable unilaterally, and
none of them yields a local container runtime. **No substitute runtime was
implemented**, and the dashboard must not claim an Alexa connection:
`AMAZON ALEXA — Not Connected` is the truthful state until a verified
authenticated integration exists.

---

## 7. Performance measurements

Recorded per milestone; see §1 and the D-01 reports in `docs/STATUS.md` for the
current figures (image size, idle/peak RSS, CPU, startup, one-device bandwidth).

## 8. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `configuration not readable` / exit 78 | the config or credentials are not mounted; check the Compose `secrets:` entry and the `file:` path exists |
| `data directory not writable` | a bind mount replaced the named volume without ownership; `chown 10001:10001 <dir>` |
| container `unhealthy` but running | the health probe could not read `{"ok":true}` from `/api/health`; check `docker compose logs hub` — with a read-only or full `/data` the hub runs degraded and reports `unavailable` (v21 behaviour) |
| dashboard shows no alerts | nothing has run yet; trigger a scenario (§1.7) or `docker compose down -v` reset the volume |
| `port is already allocated` | you are using the loopback override and something else owns host 8080 |


## 9. Observation ingestion for the hub (added 2026-10-14, D-02x)

The hub now has a validated ingestion vocabulary for the D-03/D-04 transport to carry:

- `SecurityObservation::from_json()` is the STRICT wire parser: every field is type-checked and
  bounded, unknown enumerated values are rejected rather than defaulted, and the acquisition
  timestamp must not be after the ingestion timestamp. Its `to_json()` is the matching encoder.
  A malicious or corrupted payload cannot inflate the pipeline.
- `DeviceAvailabilityTracker` and `SensorDeduplicator` are the liveness and duplicate-suppression
  primitives a multi-node hub needs; both are bounded so a hostile source cannot grow their
  state.
- `HomeAssistantAdapter` is the reference implementation of an external read-only source:
  disabled by default, explicit allowlist, explicit authorization before any request, real
  provenance, availability tracking, and no actuation path.
- D-03 should carry `SecurityObservation` payloads (plus the precursor observations) over its
  authenticated, encrypted transport, and register the new translation units in the hub target
  at that point - they are intentionally not linked into `hg_alexa_bridge` yet (see
  `docs/SKILLS_INTEGRATION_VALIDATION.md` §6).


## D-03 — secure device transport (installation, pairing, firewall)

### The data path, end to end

```
Android node                     LAN (TLS 1.2+, mutual, private CA)        Docker host
------------                     -----------------------------------        -----------
operator START + OS permission
  -> consent gate per frame    -> bounded numeric features  >  framed observation (4 KiB cap)
  -> bounded queue (128, drop-oldest)
  -> ObservationTransport ------>  :8443/tcp  ingress container
                                     - TLS handshake + client certificate
                                     - CN = device id, fingerprint == registry credential
                                     - paired / revoked / consent / capture / capabilities
                                     - bounds, timestamp window, sequence watermark, duplicates
                                     - provenance DERIVED (registry truth, not the claim)
                                     - watermark committed
                                     - IngestDetectors (per device, advisory)
                                     - Pipeline -> FusionRule -> AlertPolicy
                                     - SQLite (WAL + synchronous=FULL)
                                 hub container (loopback only, loopback override) -> dashboard
```

### Certificate provisioning (operator, once)

```bash
# 1. Create the private CA and the hub server certificate.
CERTS=${HG_CERTS_DIR:-deploy/docker/certs}
bash tools/provision_ca.sh init "$CERTS"        # keep $CERTS/ca/ca.key offline afterwards

# 2. Issue one certificate per device. The tool prints the CREDENTIAL_ID (SHA-256
#    fingerprint) and the exact registry entry to add.
bash tools/provision_ca.sh issue hg-<device-id> "$CERTS"

# 3. Register it: write $CERTS/devices.json with the printed entry
#    (paired=true, credential_id=<fingerprint>, the capability list, and
#    consent_granted/capture_enabled as the operator decides).
#    Also write $CERTS/revocations.json (an empty array is the normal start).

# 4. Configure the boundary. The config lives INSIDE the credential directory, so
#    there is one host path to provision and one read-only mount to reason about.
cp deploy/docker/config/ingress.json.example "$CERTS/ingress.json"

# 5. Make the tree readable by the container uid (see "Secret files" below).
chmod 755 "$CERTS" "$CERTS/ca" "$CERTS/devices"
chmod 644 "$CERTS"/*.json "$CERTS"/*.crt "$CERTS"/server.key "$CERTS"/ca/ca.crt \
          "$CERTS"/devices/*.crt "$CERTS"/devices/*.key

# 6. Start. Only 8443/tcp is published; the hub dashboard stays unpublished.
#    Point compose at the directory if it is not ./certs:
HG_CERTS_DIR="$CERTS" docker compose -f deploy/docker/docker-compose.yml up -d
```

The ingress reads its registry and revocation list from the same mounted directory
(`/etc/homeguardian/certs/{devices,revocations}.json`), which is what the config template expects.
The `hub` and `ingress` services share ONE data volume, so an alert raised by an ingested
observation lands in the alert history the dashboard serves.

For the phone: export the issued certificate + key into a PKCS#12
(`openssl pkcs12 -export -out <id>.p12 -inkey <id>.key -in <id>.crt -certfile ca/ca.crt`), push it
into the app private files directory with `chmod 600`, and put the password in a separate `0600`
file next to it. `docs/ANDROID_TRANSPORT_D03.md` lists the exact steps and the Android-Keystore
hardening that is still outstanding.

Both operational paths are covered by scripts that can be run before trusting a deployment:

```bash
bash scripts/verify_device_transport.sh      # 21 checks: TLS, identity, consent, revocation, replay
bash scripts/verify_detection_end_to_end.sh  # 32 checks: observation -> detector -> alert -> the hub's own API
bash scripts/measure_device_transport.sh     # footprint, latency, throughput
bash scripts/verify_docker_hub.sh            # 26 checks: the D-01 hub container
```

### Ingress configuration: the two settings that change behaviour

Everything else in `ingress.json` is a bound (connection counts, queue depth, worker count, timeouts,
the frame cap) and the code validates each against a hard ceiling. Two settings change what the
boundary DOES:

| Setting | Effect | Default |
|---|---|---|
| `audit_db_path` | Where accepted observations' alerts are written. Set to the hub's own database (`/data/homeguardian_bridge_audit.db`) so the dashboard and its API serve them. Both processes use WAL + `synchronous=FULL` + a 5 s busy timeout. **The path must be on a local filesystem** — WAL over NFS is a corruption risk and is not supported. | the hub's database (via the template) |
| `evidence_detector_ids` | OPT-IN fusion allowlist. Absent or empty keeps the conservative default (`{"activity-energy"}`), under which device observations are recorded, audited and displayed but cast no fusion vote and can never raise an alert on their own. Listing `sensor.motion` (devices are `sensor.<source_type>`) lets that evidence participate. An empty list is never passed through, because empty means "accept every detector" — absence of a decision must not become the widest permission. | conservative (empty) |

The ingress prints the active allowlist at startup, so the operator's decision is never invisible:

```
hg_device_ingress: fusion evidence allowlist = sensor.motion (operator opt-in; default is conservative)
```

Nothing reachable from either setting can produce `Severity::critical`: severity derives from fused
confidence (info below 0.8, warning at or above), and critical stays reserved for the synthetic
impact scenario.

### Credential rotation

There is no online rotation API, and that is deliberate: rotation is an operator act with a paper
trail, not a background process. To rotate a device certificate:

```bash
# 1. Issue a replacement certificate for the SAME device id.
bash tools/provision_ca.sh issue hg-<device-id> "$CERTS"      # overwrites <id>.crt/<id>.key

# 2. Update the registry entry's credential_id to the newly printed fingerprint.
#    THIS IS THE REVOCATION OF THE OLD CERTIFICATE: the previous fingerprint no
#    longer matches any record, so a device still holding the old key is refused
#    with identity_mismatch even though its chain still verifies.

# 3. Reload the registry (the registry is read at startup; the revocation list is
#    re-read per connection) and re-provision the phone's PKCS#12.

# 4. Rotate the CA itself (e.g. suspected compromise): create a new CA in a NEW
#    directory, re-issue every device certificate, then replace the mounted
#    directory and restart. The old CA stops being a trust anchor the moment the
#    mount changes, so every device must be re-issued - there is no dual-trust
#    window, which is the honest trade for a private CA with no public PKI.
```

Certificate lifetimes are 825 days by default (`HG_CERT_DAYS`). Because the fingerprint binding is
what authorizes a device, a stolen certificate is useless the moment the registry entry changes,
and no revocation-list entry is required for that case.

### Pairing and authorization are separate acts

Pairing is a registry entry. It does **not** mean the device may collect: consent and capture are
separate flags, and both default to **off**. A freshly paired phone that connects is refused with a
consent/capture reason until the operator grants them, and a device that is authenticated at the TLS
layer is still refused if the registry says so — network authentication is not permission. Revoking
is the reverse and is immediate: add the id to `revocations.json` (or run
`bash tools/provision_ca.sh revoke <id>`), and the device is refused on its next message, without a
restart, with pairing, consent, capture and liveness all cleared.

### Secret files: what the file mode has to be, and why

The compose deployment mounts its credential table as a **file-backed secret**, which Docker Compose
implements as a **bind mount** rather than a copy. The container therefore sees the HOST file's
permissions, and the hub runs as uid 10001:

```
deploy/docker/secrets/creds.json    must be readable by uid 10001 (0644 is the practical value)
```

A `0600` file owned by the operator is the intuitive choice and it FAILS here: the container exits
with code 78 (EX_CONFIG) and reports the credentials as not readable. This is the same trap for the
D-03 certificate directory, which is mounted read-only: the ingress needs to READ `server.key`, so
that file must be readable by uid 10001 too, and the protection comes from the directory and the
host, not from the key's own mode.

For anything beyond the synthetic demo credential, do not rely on file modes on a shared host: use a
real secret store (Docker/Swarm secrets, `systemd-creds`, Vault, or the cloud provider's secret
manager) and mount the material in. The rule this deployment follows is that no credential is ever
baked into an image layer, and no credential is ever committed - both are enforced by
`.dockerignore` and `.gitignore` and verified by a probe build.

### LAN-only installation and firewall

HomeGuardian is not to be exposed to the public internet. The deployment publishes one port, and the
host firewall should restrict it to the LAN or, better, to the specific devices:

```bash
# Restrict the ingress to the home subnet (example: 192.168.1.0/24 on enp3s0)
sudo nft add rule inet filter input tcp dport 8443 ip saddr != 192.168.1.0/24 drop

# Or with ufw: allow only from the LAN.
sudo ufw allow from 192.168.1.0/24 to any port 8443 proto tcp
sudo ufw deny 8443/tcp

# Do NOT forward 8443 from the WAN. There is no public-endpoint mode, no ACME
# integration and no public CA path by design: the trust model is a private CA on
# a network the owner controls.
```

Never publish the hub's loopback port. If the dashboard is needed from the host, use the opt-in
override, which binds the host side to `127.0.0.1` only:

```bash
docker compose -f deploy/docker/docker-compose.yml -f deploy/docker/docker-compose.loopback.yml up -d
```

Publishing means `-p 8443:8443`. Publishing the HUB means `-p 8080:8080`. The deployment treats the
second as an error, not an option.

### Resource posture (unchanged from D-01, plus the ingress)

Non-root user, all Linux capabilities dropped, `no-new-privileges`, no privileged mode, no Docker
socket, no host mounts beyond the certificate directory (read-only), the registry/revocation files
(read-only) and the data volumes. Persistent SQLite volume with WAL + `synchronous=FULL`. Memory,
CPU and pid ceilings on both services. 20 s stop grace so the ingress closes SQLite cleanly on
`SIGTERM`.

### Measured D-03 results

Measured on this host (40-core x86_64 workstation) and inside the container image.
Every number below came from a run, not from an estimate; the commands are in
`scripts/measure_device_transport.sh` and `scripts/verify_device_transport.sh`.

**Test counts (actual, this milestone)**

| Suite | Result |
|---|---|
| Host suite (Debug, `hg_build`) | **426 cases / 4,461 assertions**, all passing |
| `[net]` subset (protocol, registry, ingest, detectors) | 9 cases / 396 assertions |
| `ctest --test-dir hg_build` | 100% (1/1) |
| Fresh out-of-source Release (`/tmp/hg_d03rel2`) | 0 errors; 426 cases / 4,461 assertions |
| ASan + UBSan (same suite) | 426 cases / 4,461 assertions, **0 sanitizer reports** |
| D-01 container acceptance (`verify_docker_hub.sh`) | 26/26 |
| D-03 TLS acceptance (real sockets, real CA) | **21/21, 0 failed, 0 skipped** |
| D-03 end-to-end detection (both containers, real client) | **32/32, 0 failed, 0 warned** |
| D-01 hub container acceptance (final image, restored secret) | **26/26, 0 failed, 0 warned** |

Baseline for comparison: D-02x reported 413 cases / 3,984 assertions, so D-03 adds
13 cases / 477 assertions of its own.

**End-to-end detection, measured (the decisive acceptance for D-03).** With both containers running,
a mutually authenticated test client delivered 8 observations; the hub's own alert count, read
through its dashboard API from inside the hub container, moved **0 -> 1**; the served history
contained a `detection-fusion` alert; the ingested evidence (`sensor.motion`) and the alert row were
both durable in the shared store (main file plus the `-wal` sidecar); no `critical` severity was
produced; and the alert survived container replacement (**1 -> 1**). The same run refused an
identity-mismatched device (`identity_mismatch`), a revoked device without a restart
(`device_revoked`), and an unregistered device (`device_unknown`).

**Binary sizes (Release, stripped where the image strips)**

| Binary | Bytes | Note |
|---|---|---|
| `hg_device_ingress` | 3,053,760 | new in D-03 |
| `hg_device_client` | 267,504 | new in D-03 (test tool) |
| `hg_alexa_bridge` | 3,529,496 | unchanged since D-02x increment 2 |
| `homeguardian` | 3,210,648 | unchanged |
| `homeguardian_tests` | 8,138,632 | +466,968 B (+6.1%) over D-02x |

**TLS dependency footprint**

The ingress links two OpenSSL libraries (`libssl.so.3`, `libcrypto.so.3`). In the
runtime image that is `libssl3t64` at 8,115 kB installed size, and nothing else: the
runtime image deliberately carries no interpreter and no certificate CLI, because
provisioning happens on the host. The protocol, registry, ingest and detector layers
stay OpenSSL-free and are built and tested everywhere.

**Runtime behaviour (measured, 200-observation burst over real TLS)**

| Metric | Value |
|---|---|
| Idle RSS | 12,456 kB |
| Idle threads | 3 (accept + 2 bounded workers) |
| Idle CPU (5 s average) | 0.0% |
| RSS under load | 14,952 kB (+2,496 kB) |
| Threads under load | 3 (unchanged: the worker pool is bounded, not per-connection) |
| 200 observations | 0.430 s wall, 200/200 accepted |
| Per-observation round trip | 2.15 ms (ack included, no re-handshake) |
| Sustained throughput | 465 observations/s |
| Ingress CPU during the burst | 83.6% of one core |

**Not measurable without hardware, and therefore not claimed:** the old Android
device's own CPU/memory cost, the LAN hop latency, the radio power cost of a
sustained session, and reconnect timing from a real phone. The measurements above
describe the hub side and the container, which is exactly what this host can observe.


## D-03.1 deployment model: split credential storage

The deployment mounts exactly ONE credential tree into the ingress container, the
`--runtime-dir`:

| Tree | Contents | Mounted into a container? |
|------|----------|---------------------------|
| `--ca-dir` | `ca.key`, signing materials | **Never** |
| `--runtime-dir` | `ca/ca.crt`, `server.crt`, `server.key`, `devices.json`, `revocations.json`, `ingress.json` | ingress, read-only |
| `--device-dir` | per-device private keys | Never (the device keeps its own) |

Provisioning is idempotent and refuses to overwrite a non-empty runtime tree without
`--force`. `HG_CERTS_DIR` still selects the runtime directory for compose.

New ingress configuration keys (see `deploy/docker/config/ingress.json.example`):

- `replay_state_path` — durable per-device sequence watermark. Defaults to
  `device_replay_state.json` beside `audit_db_path`; it must be on the SAME persistent
  volume as the audit database so it survives a container replacement. Empty disables it
  and the ingress says so loudly at startup.
- `authorization_refresh_ms` — how quickly a revocation, consent withdrawal or capture
  disable reaches an established connection (default 250, bounded 0…60000).

### Measured on this host (D-03.1)

| Metric | Value |
|--------|-------|
| Ingress idle RSS | 12,560 kB (D-03: 12,456 kB) |
| Ingress idle threads | 3 (unchanged) |
| Ingress idle CPU | 0.0 % |
| RSS idle -> 200-observation burst | 12,560 -> 15,032 kB |
| Burst round-trip per observation | 3.06 ms |
| Sustained throughput | 327 observations/s |
| Ingress CPU during the burst | 73.5 % of one core |
| Runtime image | 131MB |
| `hg_device_ingress` | 2777832 bytes |
| `hg_device_client` | 224288 bytes |
| Android APK | 802,608 bytes (D-03: 799,370) |

Not measurable without hardware: the phone's own CPU/memory cost, the LAN hop latency
and sustained-radio battery cost. TLS, consent enforcement and `synchronous=FULL` were
not weakened for any of these numbers.

### Test matrix (D-03.1, all executed)

| Suite | Result |
|-------|--------|
| Host C++ suite | 430 cases / 4,565 assertions |
| ctest (Debug and container Release) | 100 % pass |
| D-01 Docker hub acceptance | 26 / 26, 0 warned |
| TLS acceptance (real CA, real sockets) | 21 / 21 |
| TLS server identity matrix | 9 / 9 (1 case skipped: no IPv6 on the host) |
| CA isolation | 14 / 14 (1 skipped) |
| End-to-end detection + replay + mid-session revocation | 38 / 38 |
| Suite concurrency | PASS (both instances, no leftovers) |
| Retention regression | 8 / 8 |
| Build-context secret exclusion probe | 6 / 6 |

## D-03.2 deployment notes

`replay_state_path` is now MANDATORY (see `deploy/docker/config/ingress.json.example`):

* default: `device_replay_state.json` beside `audit_db_path`, which for the shipped compose
  file means `/data/device_replay_state.json` on the volume shared with the hub — the
  watermark must survive a container replacement, so it belongs on persistent storage;
* an unset path makes the ingress REFUSE to start, with the reason on stderr;
* `allow_ephemeral_replay_protection: true` runs without it, and is REFUSED unless
  `bind_address` is loopback (`127.0.0.1` / `::1` / `localhost`). It is an isolated-test
  escape hatch; the ingress announces it at startup when used.

At startup the ingress reports what it restored, and states the ordering rule it now
follows:

    ingress: durable replay protection: 3 device watermark(s) restored from /data/device_replay_state.json
    ingress: an observation is only ever recorded as complete AFTER it has been processed;
             an attempt that is interrupted leaves no durable trace and its retry is accepted

Measured after D-03.2 (host, Release binaries in the container):

| Metric | D-03.1 | D-03.2 |
|--------|--------|--------|
| Runtime image | 131 MB | 131 MB |
| `hg_device_ingress` | 2,777,832 B | 2,794,248 B |
| `hg_device_client` | 224,288 B | 224,288 B (unchanged) |
| Android APK | 802,608 B | 802,667 B |
| Ingress idle RSS | 12,560 kB | 12,488 kB |
| RSS under a 200-observation burst | +2,472 kB | +1,648 kB |
| Round trip per observation | 3.06 ms | 162.5 ms |
| Sustained throughput | 327 obs/s | 6 obs/s |
| Ingress CPU during the burst | 73.5 % of one core | 1.3 % of one core |

The extra bytes are the two-phase record, the fsync/directory-sync path and the policy
check. No TLS, consent or SQLite-durability setting was weakened.

**The throughput change is the cost of making the durability real, and it is stated rather
than hidden.** D-03.1 claimed an fsync it never performed, so its 3.06 ms round trip measured
a page-cache write. D-03.2 performs `fsync(file)` + `rename` + `fsync(directory)` for the one
durable write per accepted observation, which on this host costs ~160 ms per observation -
the same order as the ~93 ms SQLite fsync whose cost the project already accepted for alert
durability. CPU stays at 1.3 %, i.e. the boundary is I/O-bound, not compute-bound. An earlier
revision of this milestone also persisted the in-flight marker, which doubled that cost
(200 ms / 5 obs/s) for no benefit; the marker is now in memory only. A device sending a burst
is bounded by the 64-observation queue, which applies back-pressure rather than growing.

## H-03: the Android node as a Wi-Fi device (pending hardware acceptance)

This is the node-side counterpart to the deployment described above. Two deployment facts must be
settled WITH THE DEVICE PRESENT; they are recorded here as requirements rather than as done:

1. **The hub's real LAN address.** The ingress publishes exactly 8443/tcp. The phone dials the hub's
   LAN IP, and that address must be present as a SAN in the server certificate
   (`tools/provision_ca.sh --server-names "DNS:ingress,DNS:localhost,IP:<HUB LAN IP>"`). Provisioning
   with the wrong address produces a correct refusal, not a fallback.
2. **The device's own certificate.** One PKCS#12 per phone, in the device directory that no hub
   container mounts, copied into the app's private storage as `device.p12`, `device.p12.pass`,
   `ca.p12`, `ca.p12.pass`. The hub authorizes the device by the certificate's CN/fingerprint, not
   by anything in the payload.

Unchanged and non-negotiable: the CA signing key stays offline (never in the runtime tree, never in
an image), the hub keeps no audio/camera privileges, and USB is used for installation and debugging
only - the data path under test is the phone's own Wi-Fi interface to the ingress port. Live video
is NOT part of this transport: it carries compact observations.

## Ingress container health check (H-03 follow-up)

### The defect

The runtime image carries ONE image-level health check, and it is the HUB's:

    Dockerfile:  HEALTHCHECK ... CMD ["/usr/local/bin/hg-healthcheck"]
    healthcheck.sh:  HTTP GET /api/health on 127.0.0.1:$HG_PORT (default 8080)

The `ingress` service runs a DIFFERENT binary (`hg_device_ingress`), on a DIFFERENT port (8443), and
serves no HTTP at all - but because it is built from the same `runtime` stage it INHERITED that
probe. The result was a container that reported

    Up About an hour (unhealthy)

while it was in fact serving mutual TLS correctly, with its own readiness line in the log:

    hg_device_ingress ready: bind=0.0.0.0 port=8443 registry_devices=1 paired=1 tls=mutual(min 1.2)

A health check that can never pass is worse than none: it is noise that trains an operator to ignore
the status. A plain TCP-connect check would have been no better - it passes for ANY process holding
the port, TLS or not, ours or not, which is exactly the fault class a health check exists to catch.

### The fix

| piece | what it does |
|---|---|
| `src/net/IngressHealthProbe.{h,cpp}` + `src/main_ingress_probe.cpp` | a real TLS client probe: connects, completes a handshake, and verifies the SERVER certificate against the configured private CA **and** the expected name/address |
| `deploy/docker/ingress-healthcheck.sh` | the container entry point for it: discovers the CA from the ingress config's `ca_path` (fallbacks: `${CERTS}/ca.crt`, `${CERTS}/ca/ca.crt`) and runs the probe |
| `docker-compose.yml` -> `services.ingress.healthcheck` | a **service-specific** check, which overrides the image-level one for this service only |
| image `HEALTHCHECK` | **left exactly as it was** - it is the hub's probe, and the hub service keeps it (verified in the built image) |

What the probe proves, and what it deliberately does not:

* **Proves**: the endpoint accepts a connection; a TLS handshake reaches the point where the server's
  certificate is presented; that certificate chains to the configured CA and covers the expected
  name/address (OpenSSL's own verification: TLS 1.2 floor, `load_verify_locations`,
  `X509_VERIFY_PARAM_set1_host`/`set1_ip`); and whether the server demands a client certificate.
* **Does not prove**: device authentication. A hub-side container holds no device private key by
  design, so the probe presents no client certificate, sends no observation and touches no replay
  state. Its output says so explicitly: `device_auth=NOT_PERFORMED`.
* **No disable switch**: there is no option to skip chain/name verification, and the CA file must
  load or the probe fails (D-03.1 forbids a verification-disable option).
* **No new listener**: the probe is a CLIENT. Nothing new is exposed.

Example output (live, from the healthy container's own health log):

    hg_ingress_probe: OK endpoint=127.0.0.1:8443 tls=TLSv1.3 server=CN=homeguardian-hub
      san=DNS:ingress,DNS:localhost,IP:192.168.1.6 server_cert_verified=yes
      mutual_tls_demanded=yes device_auth=NOT_PERFORMED
      detail=server identity verified; the server demanded a client certificate (tlsv13 alert certificate required)

If a deployment's certificate SAN does not carry `localhost`, set `HG_INGRESS_HEALTH_HOST` to a name
it does carry (or `HG_INGRESS_HEALTH_IP`). There is no option to skip the check.

### Verification

    # acceptance (in the container test stage, with a real CA, real certs and real sockets)
    bash scripts/verify_ingress_healthcheck.sh

    # live deployment
    docker compose -f deploy/docker/docker-compose.yml up -d ingress
    docker inspect <container> --format '{{.State.Health.Status}}'      # -> healthy
    docker inspect <container> --format '{{range .State.Health.Log}}{{.Output}}{{end}}'
    docker exec <container> /usr/local/bin/hg-ingress-healthcheck       # -> rc 0

### Two deployment prerequisites this surfaced (both are real, neither is a health-check bug)

1. **The mounted credential tree must be readable by the container's uid.** The runtime tree is mode
   0640 owned by the operator, and the container runs as uid **10001** (`USER 10001:10001`), so the
   entrypoint exits **78 (EX_CONFIG)** with `ingress config not readable`. This is the already
   documented provisioning step: `sudo chown 10001:10001 <runtime-dir>/*` (or stage a
   container-readable copy: directories 0755, files 0644). Verified directly:
   `docker run -u 10001:10001 ... [ -r /etc/homeguardian/certs/ingress.json ]` -> not readable.
2. **The data volumes must be writable by the container's uid.** The named volumes are initialized
   from the image's `/data`, which the image chowns to 10001; running the container as any OTHER uid
   yields `ingress data directory not writable: /data/ingress`. Use the shipped uid, or a bind mount
   owned by that uid.

## Provisioning is a hard prerequisite (the deployment refuses to start unprovisioned)

`HG_CERTS_DIR` is **required and has no default**:

    export HG_CERTS_DIR=/absolute/path/to/provisioned-certs     # or put it in deploy/docker/.env
    bash tools/provision_ca.sh init  --ca-dir <offline> --runtime-dir "$HG_CERTS_DIR" --device-dir <devkeys>
    bash tools/provision_ca.sh issue <device-id> --ca-dir <offline> --runtime-dir "$HG_CERTS_DIR" --device-dir <devkeys>
    cp deploy/docker/config/ingress.json.example "$HG_CERTS_DIR/ingress.json"   # then edit the paths

The directory must contain `ingress.json`, `server.crt`, `server.key`, `ca/ca.crt` and `devices.json`,
must contain **no CA signing key** (the entrypoint refuses that on purpose), and its files must be
**readable by the container's uid (10001)** - `sudo chown 10001:10001 "$HG_CERTS_DIR"/*`, or a staged
copy with directories 0755 and files 0644.

Three deliberate consequences:

1. **Compose refuses before starting anything** if the variable is unset, naming what to set. The
   old fallback (`:-./certs`) mounted an empty, root-owned `deploy/docker/certs/` and moved the
   failure *into* the container, where it read as a certificate problem rather than an unprovisioned
   deployment.
2. **Every compose command needs it**, including `down` and `ps`, because they all render the config.
   Put it in `deploy/docker/.env` (copy `deploy/docker/.env.example`) and compose reads it
   automatically from the compose file's directory - from any working directory.
3. **The entrypoint reports everything, not the first thing.** With an incomplete mount it prints the
   full list of missing/unreadable files, the uid it runs as, and the exact provisioning and
   permission commands, then exits 78 (EX_CONFIG). It still refuses a mounted CA signing key outright.

Regression guard (host, needs docker):

    bash scripts/verify_deploy_guards.sh
    # compose refuses unprovisioned; a provisioned dir is mounted verbatim; no './certs' default;
    # the image bakes no token and no credential material; the entrypoint's refusal is complete and
    # actionable; a mounted CA key is still refused; the hub announces its demo-token fallback.

### On the image token

The hub's synthetic demo token is **not baked into the image**. It is a literal the demo config also
carries, but baking it handed every consumer of the image a credential-shaped value and made the build
emit a secrets-in-ENV warning that would hide a real one later. `docker-compose.yml` declares it for
the demo deployment; the hub entrypoint falls back to it **and says so**, so a bare `docker run` is
not silently credentialed. No warning-suppression directive was added.
