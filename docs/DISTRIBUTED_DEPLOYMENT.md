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

The handwritten HTTP server in this project is deliberately loopback-only and
unauthenticated by design; it is not a hardened network service. Publishing it
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
# drive the real pipeline (synthetic simulator in D-01) and persist alerts:
docker compose exec hub bash -lc 'exec 3<>/dev/tcp/127.0.0.1/8080;
  printf "GET /api/run-scenario?scenario=synthetic-critical-impact HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" >&3; cat <&3 | tail -1'
```

`SIMULATION MODE — synthetic data` is the honest banner for D-01: the container
has no camera or microphone. Real capture arrives with the Android nodes (D-02)
and is labelled `REAL LOCAL CAPTURE ACTIVE` only when real frames are flowing.

---

## 2. D-02 — Android acquisition nodes (planned)

Reuses the existing abstractions — `IMediaDevice`, `ConsentGuardedDevice`,
Camera1/Camera2 adapters, the AAudio backend, the NDK build and the
permission/lifecycle harness — so one application can target both the documented
Android 8.1 / API 27 / armeabi-v7a device and newer phones (Camera1 where Camera2
is inadequate).

Planned controls: camera selection (front/rear), microphone selection where
supported, device identifier, configurable resolution/FPS, audio sample
rate/channels, status, explicit start/stop, runtime permissions, a persistent
recording indicator, battery/thermal reporting, safe reconnect, low-resource
operation. Acquisition never starts because the app started — it requires an
explicit user action plus granted permission.

## 3. D-03 — Secure device transport (planned)

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
