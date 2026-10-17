# HomeGuardian AI — Mi A3 Edge Device Report

**Session:** HERMES SESSION 2 — `HG-EDGE-01` (edge hardware / ARM64 implementation)
**Target device:** Xiaomi Mi A3 (`laurel_sprout`), serial `edd53dbd04de`
**Date:** 2026-10-17
**Branch / worktree:** `experiment/mi-a3-edge` @ `~/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge`
**Base commit:** `01a4d3b` (MCP Streamable HTTP candidate, **FAIL** security verdict)

> **Verdict up front:** the genuine HomeGuardian C++ engine and the genuine MCP
> Streamable HTTP server **build for, start on, and serve from** the Mi A3, with
> real SQLite persistence and a real detected alert surfaced through the MCP tool —
> and they do so identically on both the FAIL-verdict base `01a4d3b` **and** the
> security-corrected candidate `187727a` (section 4.6).
> **Conditional GO** for a *supervised* 48–72 h soak, subject to the conditions in
> section 9. **NO-GO** as an unattended, network-exposed node.

---

## 0. Scope, isolation, and deliberate non-actions

This work is isolated from every other workstream:

| Item | Value |
|---|---|
| Worktree | `~/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge` |
| Branch | `experiment/mi-a3-edge` (new; based on `01a4d3b`) |
| Build directory | `build_mi_a3_edge_arm64/` (separate from `build/`, `build_final/`, `hg_*`) |
| Device staging dir | `/data/local/tmp/hg_edge` (device-local scratch) |
| Ports | `8790` MCP, `8791` engine — **both loopback**, reached only via `adb forward` |

**Not done, deliberately:**

* No factory reset, no data wipe, no bootloader unlock, no root, no ROM flash, no
  package installed or removed, nothing written outside `/data/local/tmp/hg_edge`.
* Nothing merged into `main`; no release candidate touched; the frozen baselines
  and the security-audit worktrees (`qa-sec-audit`, `hermes-sec-01`) were read-only.
* No LAN or public listener. `HG_MCP_ALLOW_REMOTE` was never set; the server itself
  refuses a non-loopback bind without it (verified in section 4).
* No camera and no microphone were opened; every observation in this report is
  synthetic (the build links the **simulator** backend only).

---

## 1. Device discovery

### 1.1 Two devices, and how identity was established

The session brief named a Xiaomi Mi A3. Two different Android devices appeared on
USB port `2-3` during this session, so identity was established from the **USB bus
and the kernel**, never from the Settings UI or the model string:

| | Device A (first) | Device B (target) |
|---|---|---|
| USB ID (host truth) | `0e8d:201c` MediaTek Inc. `S25_Ultra` | `2717:ff08` Xiaomi Inc. `Mi A3` |
| serial | `4895180706479` | `edd53dbd04de` |
| `ro.product.manufacturer` / `board` | `alps` / `mt6580` | `Xiaomi` / `trinket` |
| ABI | `armeabi-v7a,armeabi` (**no ARM64**) | `arm64-v8a`, `armeabi-v7a`, `armeabi` |
| Android | 8.1.0 / SDK 27 (`persist.sys.fakeandroidversion=15.0`) | 11 / SDK 30 |
| RAM (kernel) | 984,792 kB (~962 MB) | 5,738,336 kB (~5.47 GiB) |
| SELinux | Permissive | Enforcing |

Device A is the fake-spec MT6580 clone already documented at
`docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md:193-215` and `docs/PRIVACY.md:275-280`.
It was **not** a second concurrent phone: at `02:39:57` it disconnected and at
`02:40:01` Device B enumerated **on the same port**, i.e. a physical swap of the
cable/device, not a simultaneous pair. At no point were two Android devices present
at once — verified against `lsusb`, the sysfs USB tree, `/dev/ttyACM*`/`/dev/ttyUSB*`
(none), adb-over-TCP (no listener), and a 600 s `udevadm monitor` window.

### 1.2 Mi A3 — hardware and software (all values read from the device)

| Property | Value | Source |
|---|---|---|
| Model / device / board | `Mi A3` / `laurel_sprout` / `trinket` (SM6125, Snapdragon 665 class) | `getprop` |
| Android / API | **11** / **30** | `getprop` |
| Security patch | **2022-08-01** (~4 years stale) | `getprop ro.build.version.security_patch` |
| Build | `V12.0.26.0.RFQMIXM`, built `2022-08-08` | `getprop` |
| Fingerprint | `Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys` | `getprop` |
| ABI | `arm64-v8a` (64-bit) + `armeabi-v7a` 32-bit; `zygote64_32` | `getprop` |
| CPU | 8 cores, Qualcomm `0x51`/`0x801`; 4 @ 1.8048 GHz + 4 @ 2.016 GHz | `/proc/cpuinfo`, `cpufreq` |
| RAM | `MemTotal` 5,738,336 kB (~5.47 GiB); `MemAvailable` 2.78 GB; zram swap 3.0 GB (758 MB used) | `/proc/meminfo`, `free` |
| Storage | `/data` 108 GB total, 21 GB used, **86 GB free (20%)** | `df` |
| Verified boot | `verifiedbootstate=green`, `flash.locked=1`, `ro.secure=1`, `ro.debuggable=0` | `getprop` |
| OEM unlock | supported=1, **allowed=0** (developer-options toggle is OFF) | `getprop` |
| SELinux | **Enforcing** | `getenforce` |
| Shell identity | `uid=2000(shell)`, context `u:r:shell:s0` | `id`, `/proc/<pid>/attr/current` |
| Wi-Fi | `wlan0` 192.168.1.14/24, SSID `Airtel_Dhar`, RSSI **-50 dBm**, 2437 MHz, 65 Mbps | `ip`, `dumpsys wifi` |
| Uptime at discovery | 1 day 8:23, load average 0.09 | `uptime` |
| Battery | level 96%, **health GOOD**, `status=charging`, temp **33.0 °C**, 4,296 mV, charge counter 3,764,640 µAh, Li-poly, USB max 500 mA @ 5 V | `dumpsys battery` |
| Thermal (idle) | `skin-therm-adc` 36.4 °C, `quiet-therm-adc` 37.4 °C, `cpuss-0-usr` 44.6 °C | `/sys/class/thermal` |
| Thermal trip points | `quiet-therm-adc` passive **56 °C** (tightest), `pmi632-tz`/`pm6125-tz` 95/115 °C, `skin-therm-adc` 125 °C | `thermal_zone*/trip_point_*` |
| Doze | enabled; light-idle after 3 min, deep-idle after 30 min; app standby on | `dumpsys deviceidle` |

**Not obtainable without root:** `/sys/class/power_supply/battery/` is
`Permission denied` to the shell user on this device, so design capacity and cycle
count could not be read. Health is therefore reported from the platform
(`health=GOOD`), not derived from a design-capacity ratio. The vendor-spec Mi A3
capacity (4030 mAh) is **not** claimed as a measurement here.

### 1.3 Deviations from the session brief

The brief's premises were correct for the Mi A3 but describe only half the session:

* **"ARM64" — correct**, uniquely so among the two devices: the Mi A3 has
  `arm64-v8a`, the clone has none. Device identity had to be established before any
  build target could be chosen; had the clone been the target, *no* ARM64 artifact
  could have run.
* **"6 GB RAM" — approximately correct** (5.47 GiB usable).
* **Android 11, not a modern release**, and the **security patch is 2022-08-01**.
  This is the single most important software finding: the device is ~4 years behind
  on platform security fixes.
* The brief says the Mi A3 is *"intended to run the genuine local HomeGuardian MCP
  server"*. Nothing in the repository previously described this device — see
  section 8.

---

## 2. Recommended ARM64 Linux runtime

**Recommendation: run the real binaries natively under Android's Bionic libc,
cross-compiled with the NDK for `arm64-v8a` / API 30, launched from
`/data/local/tmp` over adb. Do not introduce Termux or a PRoot/glibc userland.**

| | **(A) Native NDK / Bionic** ← chosen | (B) Termux native ARM64 | (C) Debian/Ubuntu ARM64 under PRoot |
|---|---|---|---|
| Extra app install | **none** | Termux APK (F-Droid/GitHub) | Termux or UserLAnd **plus** a rootfs |
| libc | Bionic (same libc Android uses) | Bionic | **glibc** |
| Syscall overhead | **none** | none | **every syscall intercepted via `ptrace`** |
| Disk cost | **3.9 MB** (server+bridge+libc++) | ~200 MB+ | 300 MB–1 GB+ |
| Runs the real C++ binaries | **yes — demonstrated** | yes (different prefix) | yes, but through interception |
| Verified in this session | **yes, end-to-end** | no | no |

**Justification, from this repository's own recorded findings — not preference:**

1. **The dependency surface makes the glibc userland unnecessary.** `hg_mcp_server`
   needs only POSIX sockets, `spdlog`, `nlohmann/json` and the *vendored* SQLite
   amalgamation. Its ELF `NEEDED` list is `liblog, libm, libc++_shared, libdl, libc`
   — all Bionic. There is no glibc requirement to satisfy, so PRoot's entire value
   proposition (a glibc userland) buys nothing here.
2. **The repository already learned the glibc lesson the hard way.**
   `docs/DECISIONS.md:477-486` records that a binary linked against a newer glibc
   than the runtime provides simply does not start (`GLIBC_2.3x not found`), forcing
   builder and runtime to be the same release. Choosing Bionic removes that class of
   failure instead of managing it.
3. **PRoot's `ptrace` interception would land exactly on this project's worst
   measured cost.** `docs/PERF_SCENARIO_LATENCY_V15.md` root-causes scenario latency
   to SQLite's durable commit paying one `fsync` per commit, at ~50–410 ms per
   `fsync` on the reference host. Intercepting and re-issuing every `fsync`/`open`/
   `stat` through a `ptrace` tracer is the single worst place to add overhead, on a
   device whose storage is already several times slower than that host.
4. **Bionic is not a compatibility compromise here — it is the native ABI.** The
   server code is plain POSIX; nothing in it needs GNU extensions. The one Bionic
   artefact observed is a benign linker warning about an unused `DT_*` entry in the
   NDK's prebuilt `libc++_shared.so` (section 6).

**What was NOT verified empirically, and the exact action required.** Termux and
PRoot were evaluated from their documented mechanics and this project's recorded
measurements; neither was executed, because both require **installing an Android
application**, which is a device state change and needs your approval. If you want
the comparison to be empirical rather than argued:

> **Action required (operator):** install Termux on the Mi A3 (F-Droid build
> `com.termux`, or `adb install` of the official F-Droid APK). Then
> `pkg install clang cmake git nlohmann-json spdlog` inside Termux and rebuild
> `hg_mcp_server`; for PRoot, additionally `pkg install proot-distro` and
> `proot-distro install debian`. Tell me when it is installed and I will run the
> same benchmark suite against all three and replace this table's last row with
> measurements.

---

## 3. Reproducible build and install guide

Four scripts, all in this branch under `scripts/`. Nothing is destructive, nothing
installs a package, and no secret is stored on the host.

```bash
WT=~/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge

# 1. Cross-compile for arm64-v8a (NDK r26d, offline FetchContent reuse)
bash $WT/scripts/mi_a3_edge_build.sh "$WT"

# 2. Push + provision credentials on-device + start loopback-only, timed
bash $WT/scripts/mi_a3_edge_deploy.sh edd53dbd04de

# 3. Functional MCP validation (18 checks) over adb forward
bash $WT/scripts/mi_a3_edge_verify.sh edd53dbd04de 18790 8790

# 4. CPU / RAM / latency / thermal measurement
python3 $WT/scripts/mi_a3_edge_measure.py edd53dbd04de 18790 8790 200

# 5. Synthetic scenario through the engine + joined MCP read-back
bash $WT/scripts/mi_a3_edge_scenario.sh edd53dbd04de synthetic-critical-impact
```

Build configuration (the non-obvious parts, each load-bearing):

* `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake`
  `-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared`
  — API 30 matches the device; `c++_shared` because the NDK's static libc++ is not
  used by the existing project.
* `-DHOMEGUARDIAN_BUILD_TESTS=OFF` — the host test target is not wanted on-device.
* `-DFETCHCONTENT_SOURCE_DIR_SPDLOG=` / `_NLOHMANN_JSON=` — reuse an existing
  `build/_deps` checkout so the cross build needs **no network**.
* **`hg_mcp_server` is not inside the `if(OpenSSL_FOUND)` guard** (CMakeLists
  lines 187–282 vs 380), and its sources reference no TLS symbol — verified by
  `grep`. That is *why* the MCP server cross-compiles cleanly with no OpenSSL for
  Android, while the three D-03 TLS binaries are skipped. This is a real property of
  the tree, not an assumption.
* Credentials: the bearer token is generated **on the device** with
  `/dev/urandom`, written at mode `0600` owned by the running uid, and never copied
  to the host or the repository.

---

## 4. Functional validation — real C++ MCP startup and tool calls

### 4.1 Build result

```
source SHA   : 01a4d3b
artifact     : hg_mcp_server  arm64-v8a PIE, ELF64 AArch64, interpreter /system/bin/linker64
               imports: liblog, libm, libc++_shared, libdl, libc          (Bionic only)
sizes        : 34,913,840 B unstripped -> 1,977,688 B stripped (1.89 MB)
warnings     : 0 for hg_mcp_server; 3 pre-existing -Wunused warnings in
               DemoScenarioRunner.cpp / hg_alexa_bridge.cpp (not introduced here)
engine       : homeguardian + hg_alexa_bridge also build for arm64-v8a
```

The binary resolves **only** to Android system libraries at runtime — independently
confirmed with `linker64 --list` on the device:

```
liblog.so        => /system/lib64/liblog.so
libm.so          => /apex/com.android.runtime/lib64/bionic/libm.so
libdl.so         => /apex/com.android.runtime/lib64/bionic/libdl.so
libc.so          => /apex/com.android.runtime/lib64/bionic/libc.so
libc++_shared.so => /data/local/tmp/hg_edge/libc++_shared.so        (staged beside it)
```

### 4.2 Fail-closed startup

```
$ ./hg_mcp_server hg_config.json /nonexistent.json
mcp: credential store failed to load securely: credential file not accessible
EXIT=3                                    <- exactly the designed fail-closed code
```

With a valid `0600` credential:

```
[hg_mcp] hg_mcp_server listening on 127.0.0.1:8790 (loopback=true, auth=required, tools=2)
/proc/net/tcp -> 0100007F:2256          <- 127.0.0.1 only; never 00000000 (all interfaces)
```

### 4.3 MCP functional suite — **18/18 pass**

`scripts/mi_a3_edge_verify.sh` against the on-device binary
(`logs/mcp_functional_verify.txt`):

| Area | Check | Result |
|---|---|---|
| auth | `tools/list` with no `Authorization` | **401** + `WWW-Authenticate: Bearer` |
| auth | `tools/list` with a wrong bearer | **401** |
| origin | foreign `Origin` | **403** |
| handshake | `initialize` | **200**, `Mcp-Session-Id` minted |
| handshake | protocol version | **2025-11-25**, server `homeguardian-mcp 1.0.0` |
| session | `tools/list` with no session | **400** |
| tools | `tools/list` | **200** — `get_homeguardian_status`, `get_alert_summary` |
| tools | `tools/call get_homeguardian_status` | **200** `{"capture_enabled":false,"read_only":true,"state":"Running"}` |
| tools | `tools/call get_alert_summary` | **200** |
| contract | mutating tool name (`enable_capture`) | **-32602**, no such tool |
| transport | `GET /mcp` | **405** |
| transport | `DELETE /mcp` / without session | **200** / **400** |
| bounds | 70 KiB body | **413** |
| bounds | chunked `Transfer-Encoding` | **501**, explicitly refused |
| bounds | body with no `Content-Length` (raw socket) | **411** |

### 4.4 SQLite persistence on the device

The server created a real store in `/data/local/tmp/hg_edge`:
`homeguardian_mcp_audit.db` (73,728 B) with `-wal`/`-shm` sidecars, journal mode
`wal`. Pulled to the host and read independently: tables
`events, alerts, profiles, routines, consent_records`; `PRAGMA integrity_check` → **ok**.

### 4.5 Synthetic scenario + alerts, and the joined MCP read

`scripts/mi_a3_edge_scenario.sh` drives the **real engine** (`hg_alexa_bridge`,
arm64) on the device. This found a genuine behaviour, not a bug:

* With `media_capture_enabled=false` the scenario is **blocked by the fail-closed
  consent/capture gate**: `events_processed: 0`, `alerts_generated: 0`,
  `"capture disabled by configuration; no observations were delivered"`. The gate
  works.
* With the simulator-enabled config, scenario `synthetic-critical-impact` ran in
  **58 ms** and produced **2 alerts**, including one **`critical`**
  (`rule=synthetic-impact`, `confidence=0.75`,
  `evidence: transient 0.95 -> stillness 0.1 within 1 sample(s)`) and one `info`
  (`rule=detection-fusion`, confidence 0.70).
* Those alerts were persisted on-device (`homeguardian_bridge_audit.db`, WAL) and
  read back **independently on the host**: `alerts: 2 rows` (one `critical`, one
  `info`), `integrity_check: ok`.
* **Joined proof** — the MCP read-only tool, pointed at the engine's database,
  reports the engine's alert:

```
tools/call get_homeguardian_status -> {"capture_enabled":false,"read_only":true,"state":"Running"}
tools/call get_alert_summary      -> {"alert_count":2,
                                      "alert_ids":["alert-f942ee36-...","alert-157eb066-..."],
                                      "highest_severity":"critical","read_only":true}
```

Also verified by negative controls: session POST with a wrong secret → **401**;
`run-scenario` with a foreign `Origin` → **403**.

Evidence files: `logs/mcp_functional_verify.txt`, `logs/mi_a3_edge_scenario.txt`,
`logs/device-artifacts/`.

### 4.6 Re-run against the security-corrected candidate (`187727a`)

While this work was in progress, `fix/mcp-session-principal-binding` advanced from
the base `01a4d3b` to **`187727a`** — *"fix(security): resolve HG-SEC-02 blockers
B-1 through B-6 and documentation hygiene"*. It is a **direct descendant** of
`01a4d3b` and it changes exactly the files this deployment uses:
`src/main_mcp_server.cpp`, `src/mcp/McpHttpServer.cpp`,
`src/core/AlexaCredentialStore.cpp` and `CMakeLists.txt`. That is the
"corrected SHA" the session brief anticipated, so the whole suite was re-run
against it rather than left as a footnote.

Method: a **second isolated worktree/branch** (`experiment/mi-a3-edge-sec02`,
`~/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-sec02`) based on `187727a`,
with this session's tooling cherry-picked on top — so the first worktree's committed
SHA stayed intact and the two candidates stay comparable. Evidence:
`logs/mcp_functional_verify_sec02.txt`, `logs/mi_a3_edge_scenario.txt`,
`logs/mi_a3_edge_measurements.txt` in that worktree.

| Check | `01a4d3b` (base) | `187727a` (corrected) |
|---|---|---|
| arm64-v8a build, warnings | 0 | 0 |
| stripped size | 1,977,688 B | **1,984,800 B** |
| fail-closed exit 3 on bad credential store | yes | yes |
| binds `127.0.0.1:8790` only | yes | yes |
| MCP functional suite | **18/18** | **18/18** |
| cold start to listening | 275–287 ms | **275 ms** |
| RSS steady state | 5,512 kB | **5,424 kB** |
| 150× tool call | p50 7.98 ms, 0 errors | **p50 8.32 ms, 0 errors** |
| server CPU per call | 1.55 ms | **1.87 ms** |
| RSS growth over 450 requests | +0 kB | **+0 kB** |
| synthetic scenario → `critical` alert | yes (58 ms) | **yes** |
| joined MCP read of that alert | `highest_severity=critical` | **`highest_severity=critical`** |
| battery temp under load | 33.0 → 33.0 °C | **33.2 → 33.2 °C** |

Verified present in the built sources: the B-1 credential-identity binding
(`AlexaCredentialStore.cpp`) and the B-6 `::shutdown(SHUT_WR)` + drain before
`::close()` (`McpHttpServer.cpp:85-87, 820`).

**Interpretation.** The device and the runtime are insensitive to the security
fixes: `187727a` behaves identically on the Mi A3, at the same resource cost. So
the runtime recommendation here **transfers unchanged** to the corrected SHA, and
**`187727a` is the candidate to pilot on**, not `01a4d3b`.

**Two honest caveats.** (1) This is a *compatibility* result, not a security
approval — this session did not audit HG-SEC-01/02, and whether `187727a` is the
*independently approved* SHA is not something this session can certify; confirm with
the security-audit workstream (`qa-sec-audit`, `hermes-sec-01`) before relying on
it. (2) The device is currently running the `187727a` build, since it was deployed
last.

---

## 5. Measurements

`logs/mi_a3_edge_measurements.txt` / `.json`. Latency is **end-to-end over the
adb-forwarded USB link**, so it includes the transport, not just the server.

| Metric | Value |
|---|---|
| Cold start to listening | **275–287 ms** |
| RSS (resident) | **5,512 kB (~5.4 MB)**, constant |
| 200× `tools/call` | 200/200 → HTTP 200, **0 errors** |
| Latency p50 / p95 / p99 / max | **7.98 / 9.25 / 11.29 / 14.70 ms** |
| Throughput | **121.8 req/s** |
| **Server CPU per call** | **1.55 ms = 18.9 % of one core** (measured from `/proc/<pid>/stat` utime+stime) |
| Mixed workload (600 req: list + 2 calls) | 121.6 req/s, p50 7.87 ms, p95 11.07 ms, max 15.93 ms |
| Server CPU per mixed request | 1.367 ms = 16.6 % of one core |
| RSS growth over 600 requests | **+0 kB** (no leak) |
| Battery temp before → after | 33.0 → **33.0 °C** |
| `skin-therm` before → after | 36.4 → 36.6 °C |
| Hottest real thermal zone before → after | 44.6 → **45.6 °C** (throttle point 56 °C) |
| Reliability | process survived an adb-server restart (same PID); clean SIGTERM shutdown with counters logged |

The `ps` VSZ figure (~12.4 GB) on this platform is a 64-bit address-space
reservation and is not a memory measurement; RSS is.

---

## 6. Remaining compatibility issues

1. **The D-03 mutual-TLS ingestion boundary cannot be built for Android as-is.**
   NDK r26d ships no OpenSSL (`libssl`/`libcrypto` absent from the sysroot), so
   `hg_device_ingress`, `hg_device_client` and `hg_ingress_probe` are skipped in the
   cross build exactly as they are skipped on a host without `libssl-dev`. The Mi A3
   node therefore currently has **the loopback MCP server and the engine only** —
   not the mTLS device-ingestion path. Fixing this requires vendoring and
   cross-compiling OpenSSL for Android (a real, bounded task — not attempted here).
2. **`hg_mcp_server` depends on `libc++_shared.so`** staged beside it. It can be
   removed by linking the NDK's static libc++, but that was not done (it would change
   an artifact the MCP branch already verifies).
3. **Benign linker warning** on every start: `libc++_shared.so: unused DT entry:
   unknown processor-specific (type 0x70000001 ...) (ignoring)`. It comes from the
   NDK's prebuilt `libc++_shared.so` and has no runtime effect — the binary loads and
   runs. It is noise in the logs, nothing more.
4. **Android API-30 vs the documented API-34 target.** `docs/ARCHITECTURE.md:305`
   and `docs/STATUS.md:153` say the Android backends cross-compile for `arm64-v8a /
   API 34`; the actual device is API 30 and the H-03 harness targets API 27. Three
   different Android targets are now documented for one project (section 8).
5. **The camera path is known-broken on this device class and was not retried.**
   `docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md` records that camera2 NDK
   enumerates 0 devices on the MT6580 unit. On the Mi A3 the camera path was **not
   exercised** in this session (it needs a permission grant and an explicit operator
   action, and this session's mandate was the MCP server and engine).
6. **Security patch level 2022-08-01.** Not fixable without a vendor ROM update,
   which is out of scope and forbidden without approval.
7. **The base SHA `01a4d3b` carries a FAIL security verdict** — superseded during
   this session by `187727a`, which was built and validated on the device (section
   4.6). Everything above is **localhost-only, non-deployed compatibility testing**
   against `01a4d3b`; nothing here should be read as a security approval of either
   SHA.

---

## 7. Preparation for 24/7 operation (assessment, not yet executed)

**What works in our favour:**

* **Doze / App Standby do not apply to this process.** It runs in the **shell
  domain** (`u:r:shell:s0`), not as a `untrusted_app`. The device's Doze timers
  (light-idle 3 min, deep-idle 30 min) and app-standby buckets govern apps; a
  `setsid`-detached shell-domain process is not frozen by them. Verified in practice:
  the process survived an adb-server restart with the same PID, and the device never
  rebooted (uptime monotonically 1 day 8:23 → 8:38).
* Idle headroom is large: load average 0.09, 2.78 GB available RAM, 86 GB free.

**What does not work yet, and must be solved before any unattended run:**

1. **No automatic restart.** Nothing starts on boot: `bootreason=powerkey`, there is
   no `init.d`, `/data/local/tmp` is not a service location, and the HomeGuardian
   harness APK is **not installed on the Mi A3** (it exists only on the MT6580
   clone). After a reboot, or after a crash, **the server stays down until an
   operator re-runs the deploy script.** Options, in order of preference:
   * (a) extend the existing harness APK with a `BOOT_COMPLETED` receiver that starts
     the server and supervises it — no root, uses machinery the project already has;
   * (b) an operator-driven re-arm after a reboot (acceptable only for a supervised
     test);
   * (c) Magisk/boot script — **rejected**: requires root, which is forbidden.
2. **No supervisor.** Nothing restarts the process after an abnormal exit, and
   nothing bounds restarts. A crash-looping supervisor is worse than none.
3. **Health checks.** The server has no `/health` endpoint of its own; liveness must
   be probed with a real MCP `initialize` over the forward (this session's scripts do
   exactly that, and `hg_ingress_probe` exists in-tree as the pattern for a
   service-specific probe).
4. **Wi-Fi reconnection.** Wi-Fi is `COMPLETED` on `Airtel_Dhar` (RSSI -50 dBm, 2.4
   GHz, 65 Mbps) with the watchdog at its default. Auto-reconnect was **not** tested
   by toggling the radio or rebooting the device — that is a deliberate
   non-action, and the useful local path (MCP over `adb forward`) does not depend on
   Wi-Fi at all.
5. **USB/ADB transport instability — observed once, real.** During this session the
   device went `adb ... offline` while remaining electrically present on the bus
   (still `2717:ff08` on port `2-3`). `adb reconnect offline` restored it, and the
   device had not rebooted (uptime unchanged). So the link can drop without the phone
   being at fault; any long run needs `adb reconnect` in its recovery loop, and the
   server's uptime must be judged from the device, not from adb.

**Battery and thermal safety — before any long-duration test:**

* Health is **GOOD**, temperature 33.0 °C under load, and the tightest thermal trip
  (`quiet-therm-adc`, 56 °C) stayed **~10 °C** above the hottest measured zone
  (45.6 °C). Thermally there is headroom.
* **The open risk is charging, not heat.** The device currently sits at 100 %/96 %
  on a 500 mA USB charger, i.e. continuously held at full charge. This session did
  **not** run a long unattended charge, and the brief explicitly forbids leaving an
  ageing or abnormal battery charging unattended — the device's own capacity/cycle
  nodes are unreadable without root, so its true age is unknown.
* **Required before a soak:** either place the phone on a switchable/charge-limiting
  supply, or accept continuous full charge as a deliberate, recorded decision —
  *your* call, not mine.

**Proposed soak (not started):** a **supervised 48–72 h** run with (1) the
BOOT_COMPLETED restart path or an operator re-arm, (2) a supervisor with a bounded
restart policy, (3) a periodic `initialize`-based liveness probe, (4) `adb reconnect`
in the recovery loop, (5) 5-minute sampling of RSS, `/proc/<pid>/stat` CPU, battery
temperature and the thermal zones, and (6) a hard stop on any thermal trip or
charge anomaly. **Awaiting your approval.**

---

## 8. Documentation audit

**Added by this session** (all inside `experiment/mi-a3-edge`):
`docs/EDGE_DEVICE_MI_A3.md` (this file), `config/mi_a3_edge.json`,
`config/mi_a3_edge_engine.json`, four scripts, `scripts/mi_a3_edge_measure.py`,
and `logs/`.

**Findings:**

1. **No document described the Mi A3 at all.** The only Android device in the
   documentation is the MT6580 clone (`docs/PRIVACY.md:275-280`,
   `docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md`). This report is the first
   record of the real `arm64-v8a` device and of the engine + MCP server running on it.
2. **Three inconsistent Android targets in the docs.** `docs/ARCHITECTURE.md:305`
   and `docs/STATUS.md:153` state `arm64-v8a / API 34`; `docs/ANDROID_WIFI_*` and
   `docs/STATUS.md` describe the harness as `armeabi-v7a / API 27`; and the actual
   device reached in this session is **API 30**. The API-34 claim in particular
   describes a target no device here runs. Recommend reconciling these into one
   statement of "device reached" vs "target compiled for".
3. `docs/PRIVACY.md:280` ("32-bit only (no arm64 on this device)") is **correct for
   the clone** but reads as a project-wide claim; now that an arm64 device is in use
   it should be scoped explicitly to that device.
4. **Roadmap coverage missing.** The HomeGuardian roadmap (`docs/ROADMAP.md`) has no
   row for an edge/ARM64 device server, and the `my-job-agent` control plane has no
   matching row either (checked read-only with `npm run gate:status --find ...` for
   "HomeGuardian", "edge device", "Mi A3", "ARM64" — no match). Per the standing rule
   I did **not** invent a row. **This workstream needs a roadmap row, and that is an
   operator decision.** Flagged, not silently absorbed.
5. Nothing frozen was modified: the security-audit worktrees and the release
   candidate were read-only throughout.

**Committed experimental SHA:** the head of `experiment/mi-a3-edge` — the single
commit that contains this report, the four scripts plus the measurement driver, the
two configs, and the raw evidence logs under `logs/`. It is derived from `01a4d3b`
and is **not** merged anywhere. Resolve it with:

```bash
git log --oneline -1 experiment/mi-a3-edge
```

---

## 9. GO / NO-GO recommendation

**GO — conditional — for a supervised 48–72 h pilot on the Mi A3.**

The genuine engine and the genuine MCP Streamable HTTP server demonstrably build
for, start on, and serve from the device: 18/18 functional checks, a real detected
`critical` alert read back through the MCP tool, real SQLite WAL persistence,
5.4 MB RSS, 1.55 ms of CPU per tool call, and no thermal movement under load.

The conditions are not optional:

1. **Loopback only.** Keep the server bound to `127.0.0.1` behind `adb forward`.
   No LAN listener, no public exposure, no remote camera ingestion — and
   `01a4d3b` carries a FAIL security verdict regardless.
2. **Fix the restart path first** (BOOT_COMPLETED receiver in the harness APK, or a
   recorded decision to accept operator re-arm). No unattended run on a process that
   cannot come back by itself.
3. **Add a supervisor with a bounded restart policy** and a real `initialize`-based
   liveness probe.
4. **Resolve the charging question** — switchable/charge-limited supply, or an
   explicit recorded decision to accept continuous full charge.
5. **Put `adb reconnect` in the recovery loop**; judge uptime from the device.
6. **Do not treat the mTLS ingestion path as available** on this node until OpenSSL
   is cross-compiled for Android (section 6.1).
7. **Pilot on `187727a`, not `01a4d3b`.** The corrected candidate was built and
   validated on the device (section 4.6) with identical results, so no re-porting is
   needed. Get its HG-SEC-01/02 sign-off confirmed by the security-audit
   workstream first — this session validated the *device and runtime*, not any SHA's
   security posture.

**NO-GO** for anything unattended, LAN-reachable, or camera-ingesting at this stage.

---

## 10. HG-EDGE-02 — the managed Android service

**Status: APK is built, signed and statically verified. NOT INSTALLED — awaiting
operator approval (see section 10.6).**

### 10.1 What was built

The same genuine C++ engine and MCP server, packaged as a managed Android
application: `org.homeguardian.edgeservice`, source in `apk_service/`, built by
`apk_service/build_service_apk.sh`.

A **new package, deliberately separate from `org.homeguardian.harness`**:

| | harness (existing) | edge service (this) |
|---|---|---|
| Purpose | camera/mic acquisition node | run the local MCP server |
| Permissions | CAMERA, RECORD_AUDIO, ACCESS_NETWORK_STATE | INTERNET, FOREGROUND_SERVICE, RECEIVE_BOOT_COMPLETED |
| Lifecycle | stops when the activity stops | outlives the activity and the screen |
| Capture | yes, gated | **none, by construction** |

Keeping them apart means the service cannot acquire media even by accident, and the
permission prompt the operator approves is the minimum set.

### 10.2 Architecture

* **Native code is a JNI shared library**, `libhg_edge_service.so`, not an
  executable in a writable directory. Android 10+ blocks executing files from
  app-writable paths, so `System.loadLibrary` is the supported way to run native code
  in an app — and it needs no ADB privileges and nothing in `/data/local/tmp`.
* **The source list is derived from `CMakeLists.txt`** (the `add_executable(hg_mcp_server)`
  block minus `src/main_mcp_server.cpp`, which owns `main()`): 41 translation units.
  It cannot silently drift from the binary HG-EDGE-01 validated, and the build
  refuses to run if the count looks partial or if test sources leak in.
* `hg_edge_service_jni.cpp` constructs the real `Application`, `PersistenceAudit`,
  `AlexaCredentialStore`, `McpTools` and `McpHttpServer`, and runs `serve()` on a
  `std::thread` because Android owns the process entry point.
* **spdlog is routed to logcat** through a liblog sink. An app's stdout/stderr go to
  `/dev/null` on Android, so the binary's usual stderr logging would be silent.
* **`EdgeService`** is a foreground service with a visible, ongoing notification
  carrying Stop and Restart actions. It returns `START_STICKY` so Android recreates it
  after a process kill.
* **`BootReceiver`** handles `BOOT_COMPLETED` and `MY_PACKAGE_REPLACED`. It is *not*
  direct-boot aware: the credential file and the SQLite store live in
  credential-encrypted storage, so Android delivers `BOOT_COMPLETED` only after the
  first unlock — that is the honest behaviour, not a limitation to work around.
* **Bounded crash recovery**: a watchdog probes MCP every 20 s; if the engine is gone
  or 3 consecutive probes fail, the engine restarts — at most 5 times inside a 10-minute
  window, with exponential backoff. The budget refills only after 3 minutes of verified
  health, so a fast crash-loop cannot reset itself. When exhausted the service enters
  DEGRADED, says so in the notification, and stops instead of spinning.
* **The health probe is a real MCP client** (`McpProbe`): `initialize` (capturing
  `Mcp-Session-Id`), `notifications/initialized`, `tools/list`, `tools/call`. It runs
  in-process, which is what makes "MCP available with no ADB attached" provable, and
  every attempt is timestamped into `probe.log` (private copy at `0600`, plus a
  secret-free copy in the app's external files dir so the host can pull evidence
  without the app being debuggable and without root).

### 10.3 Permissions requested

Install-time (normal) permissions only — **no runtime prompt, no user data**:

| Permission | Why |
|---|---|
| `INTERNET` | Required to open *any* socket, including one bound to the loopback address |
| `FOREGROUND_SERVICE` | Required to run a foreground service on API 28+ |
| `RECEIVE_BOOT_COMPLETED` | Required to receive `ACTION_BOOT_COMPLETED` |

Deliberately **not** requested: CAMERA, RECORD_AUDIO, ACCESS_FINE_LOCATION,
READ/WRITE_EXTERNAL_STORAGE, POST_NOTIFICATIONS (API 33+ only; the device is API 30
and a foreground-service notification needs no permission there).
`android:debuggable` is **not** set — it would expose the app's private storage and
the bearer token inside it to anything holding ADB.

### 10.4 SDK field reconciliation (requested)

Five values that this project's docs had been conflating. They are kept as five
separate fields in `apk_service/build_service_apk.sh`:

| Field | Value here | What it actually governs |
|---|---|---|
| **compileSdk / android.jar** | **34** | which platform API the code compiles against — a *build input*. Newest available locally (`android-34`). It does **not** have to match the device. |
| **minSdkVersion** | **30** | the oldest device the APK will install on. **This is what makes the app refuse to install on the MT6580 clone (API 27).** |
| **targetSdkVersion** | **30** | the behaviour contract Android applies to the app. Chosen to match the only device we target, so no API-31+ foreground-service background-start restriction or API-34 `foregroundServiceType` requirement is in play. Raising it later is a real task, not a formality. |
| **NDK API level** | **30** | the platform level native code links against (`aarch64-linux-android30-clang++`), i.e. the minimum API for the sysroot and libc symbols. |
| **device API level** | **30** | what the phone actually runs (`adb shell getprop ro.build.version.sdk`). Read from the device; **never** a build input. |
| **packaged ABI** | **arm64-v8a** | the device's `abilist64`. The clone is `armeabi-v7a`-only and cannot run this APK. |

For comparison, the pre-existing harness is minSdk **27** / targetSdk **27** /
compileSdk **27** / NDK API **27** / `armeabi-v7a` (the clone), while
`docs/ARCHITECTURE.md:305` and `docs/STATUS.md:153` describe the camera/audio
backends as `arm64-v8a / API 34` and the device reached here is **API 30**. That is
**five distinct values across the project**; the earlier "three inconsistent Android
targets" finding is a subset of it.

### 10.5 Built artefact and static verification

| Check | Result |
|---|---|
| Package / version | `org.homeguardian.edgeservice`, versionCode 1, versionName 1.0.0 |
| Signed | **yes** — debug keystore outside the repo, `apksigner verify` exit 0, v3 scheme |
| Certificate SHA-256 | `869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b` |
| APK SHA-256 | `d58c7e294711ee01617639f5140dd0f349b928a43a09eb0ceb3fb841f697543c` |
| Size / contents | 1.56 MB; `classes.dex`, `lib/arm64-v8a/{libhg_edge_service.so, libc++_shared.so}` |
| zipalign | 4-byte aligned, OK |
| Native library | ELF aarch64; `NEEDED` = `liblog, libc++_shared, libm, libdl, libc` (Bionic only) |
| JNI exports | **7 declared / 7 exact unmangled** `Java_org_homeguardian_edgeservice_NativeEngine_*`; **0** mangled `_Z*Java_*` |
| Components in dex | `EdgeService`, `BootReceiver`, `MainActivity` all present |
| Manifest in APK | service `exported=false`, receiver `exported=true` (BOOT_COMPLETED is a protected broadcast), activity launcher |
| Loopback-only | structural: `cfg.allow_remote = false` hardcoded, bind literal `127.0.0.1` hardcoded in the app, and **the JNI layer contains no `getenv` at all** — the `HG_MCP_ALLOW_REMOTE` escape hatch the command-line binary reads does not exist on this code path |

The JNI-export checker (`scripts/verify_jni_exports.sh`) was generalised in this
branch: it hardcoded `org_homeguardian_harness`, so for the new package it would have
checked the wrong symbol names and passed while checking nothing. It now derives the
package from the Java sources.

### 10.6 Test results — PASS / FAIL / UNVERIFIED

Approved scope: tests 1, 2, 3 and 6, then extended by the operator to include 4 and
5. The 48–72 h soak remains **not started**.

| # | Test | Status | Evidence |
|---|---|---|---|
| 1 | Installation and user-approved permissions | **PASS** | `adb -s edd53dbd04de install -r` → Success; `primaryCpuAbi=arm64-v8a`; versionCode 1 / 1.0.0; `INTERNET`, `FOREGROUND_SERVICE`, `RECEIVE_BOOT_COMPLETED` all `granted=true`; **CAMERA and RECORD_AUDIO are not requested at all**; the device's `base.apk` sha256 equals the local build byte-for-byte |
| 2 | Native C++ MCP startup inside the app sandbox | **PASS** | `onStartCommand action=…START` → `nativeStart` → `hg_mcp_server listening on 127.0.0.1:8790 (loopback=true, auth=required, tools=2)` → `ENGINE_UP … creds_mode=0600`; socket `0100007F:2256` (loopback, never `00000000`); app's own MCP client returns `proto=2025-11-25 tools=get_homeguardian_status,get_alert_summary`; host-side via `adb forward`: no token → **401**, wrong bearer → **401** with `WWW-Authenticate: Bearer` |
| 3a | Manual **Stop** | **PASS** | `input tap` on the real button → `serve() returned; server stopped`, `ENGINE_STOP code=0 reason=operator stop`, `ServiceRecord` count 0, listener closed (host curl → empty reply), UI flipped START enabled / STOP disabled |
| 3b | Manual **Restart** | **PASS** | `onStartCommand action=…RESTART` → `restartEngine: operator restart` → `listening on 127.0.0.1:8790`; `LISTEN 0100007F:2256` |
| 3c | Automatic crash recovery | **PASS** (after fixing D2 below) | `am crash` → AMS `Process … has died` → `Scheduling restart of crashed service … in 1000ms` → new process → `onStartCommand … startId=2` → `listening on 127.0.0.1:8790` → `ENGINE_UP`; `restartCount=1 crashCount=1` |
| 4 | Power-off/reboot then service recovery | **PASS** | `adb reboot`; uptime reset to 32 s; at t+45 s: `BootReceiver action=android.intent.action.BOOT_COMPLETED` → `BOOT_RECEIVED` → `onStartCommand …START` → `listening on 127.0.0.1:8790` → `ENGINE_UP`; **0** "credentials provisioned" lines in that boot, i.e. the existing 0600 credential file survived and was reused |
| 5 | Screen-off MCP availability with no ADB | **PASS**, with the qualification in 10.10 | Screen off (`mWakefulness=Dozing`, `mScreenOn=false`), `adb kill-server`, unattended: **66 `PROBE_OK` / 0 `PROBE_FAIL`**, inter-probe cadence **median 20.1 s**, and then repeated under **forced deep Doze** (`dumpsys deviceidle force-idle` → `mState=IDLE mLightState=OVERRIDE`) with ADB again disconnected: probes continued, gaps unchanged. **Caveat: this ran on USB power, where the SoC keeps running; it is not a real suspend test. The overnight on-battery run in 10.10 is, and it shows the probe cadence degrading while the server keeps serving.** |
| 6 | CPU, RAM, battery and thermal | **MEASURED** (see 10.8/10.9) | — |

**Not a PASS:** the 48–72 h supervised soak, and any unattended-charging behaviour —
neither was started.

Cadence caveat, stated plainly: the probe log contains two gaps larger than 25 s
(365 s, 128 s). Both are **mine, not the server's** — they span a `force-stop` +
rebuild + reinstall, and the reboot. Inside the actual unattended windows the cadence
never broke.

### 10.7 Defects found and fixed during this test round

Both were found by running the real code, and both are in **my** integration layer,
not in the MCP server.

**D1 — use-after-free of the `ToolRegistry` (SIGSEGV).** `McpHttpServer::Impl` stores
`const ToolRegistry& tools` (`src/mcp/McpHttpServer.cpp:338`), not a copy. The
command-line binary satisfies that by accident: its registry is a local of `main()`
and lives as long as the process. My `start()` had it as a local too, so it died when
`start()` returned while the accept loop kept running on a thread. First `tools/list`
then took a hard fault:

```
Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x6f9c0ab000000038
  #00 nlohmann::json::parse<std::string const&>
  #01 homeguardian::mcp::(anonymous namespace)::tools_list_json(ToolRegistry const&)+248
  #02 homeguardian::mcp::Dispatcher::handle_one(...)
  #04 homeguardian::mcp::McpHttpServer::Impl::handle_connection(int)
  #05 homeguardian::mcp::McpHttpServer::Impl::worker_loop()
```

Fixed by owning the registry as a member (`tools_`) and releasing it after the server
in `teardown_locked()`. Post-fix: 66 probes, zero crashes.

**D2 — `START_STICKY` alone recovered nothing.** After the process was killed, AMS
restarted the service but destroyed it before `onStartCommand` ran, because it was
started-but-not-yet-foreground:

```
AMS: Start proc 27682:… for service {…EdgeService}
AMS: Stopping service due to app idle: u0a226 -8s275ms …EdgeService     <-- 42 ms later
AMS: Service done with onDestroy, but executeNesting=2
```

No engine came back. Fixed by calling `startForeground()` in `onCreate()` — a
foreground service is skipped by the idle sweep, so the notification must exist from
the first moment of the lifecycle. This is also the correct requirement regardless of
the sweep, since a foreground service must be foreground promptly.

**D3 — the UI could not be inspected or driven.** `uiautomator dump` returned
`ERROR: could not get idle state` because the status view was rewritten every second.
Rendering only when the content actually changes fixed it (and removes a pointless
1 Hz redraw on a battery-sensitive device). This is why the tests above could use the
real buttons via `input tap` rather than an internal shortcut.

### 10.8 Measurements (test 6)

The app process, which is what now owns the engine. Latency is the app's own MCP
probe over loopback, measured on-device end to end.

| Metric | Value |
|---|---|
| App CPU (95 s window, pre-reboot) | 150 ms = **0.16 % of one core** |
| App CPU per MCP probe | **28–37 ms** (includes waking the JVM for each probe) |
| Probe latency | min 19 ms, **p50 29 ms**, max 46 ms (n=66) |
| App RSS | **91,160 kB steady, flat for 2 min** (no leak) |
| App peak RSS (VmHWM) | 89,960–96,084 kB |
| Bare native server (HG-EDGE-01) | 5,412 kB — so the managed form costs ~86 MB of JVM + framework |
| Cold start to listening | **~140 ms** from `nativeStart` to `listening` |
| Probe reliability | **66 PROBE_OK / 0 PROBE_FAIL** |
| Crashes since the D1 fix | **0** |

### 10.9 Battery and thermal (the operator asked for these before proceeding)

| Metric | Value |
|---|---|
| Level | 96 % (95 → 96 % across the tests) |
| Health | **GOOD** (`health=2`) |
| Temperature | **33.0 – 34.3 °C**, unchanged under load |
| Voltage | 4,296 – 4,331 mV |
| Status | charging, USB, **500 mA @ 5 V** (`Max charging current 500000 µA`) |
| Charge counter | 3,845,600 µAh (≈3.85 Ah) |
| Real thermal zones under load | **41 – 47 °C**; hottest is `cpuss-0-usr` at 46.9 °C |
| Tightest throttle trip | `quiet-therm-adc` passive at **56 °C** → ~9 °C of headroom |

**Honest limitation on the thermal figure.** Immediately after the reboot, two samples
reported an aggregate maximum of 54.6 °C and 56.5 °C across the zones my filter
accepts. Steady-state per-zone sampling (60 s, every 5 s) shows all real zones at
41–47 °C and never reproduced those values. So they were boot-time transients, but I
**cannot say which zone produced them**, because the filter discards `lmh-*`/`-step`/
`-lvl*` names (which are limiter thresholds, e.g. `lmh-dcvs-00` reads a flat 75 °C)
and the raw sample was not retained. Attributed per-zone sampling is a soak item.

**The charging question is still open**, and the 500 mA figure is exactly the kind of
number one must not read optimistically: it is the *negotiated* USB current, not a
charge-percentage limit. The phone sits at 95–96 % while plugged in, so "500 mA" does
not imply "safe charge level". Continuous unattended charging remains **not approved**
and no unattended soak was started.

### 10.10 Overnight unattended run — and a correction to the test-5 claim

The device was left running from ~04:30 to ~10:15 (5 h 45 m) with nothing attached
except the cable. That window is the most useful reliability evidence in this report,
and it **qualifies** what section 10.6 says about test 5.

**What held up.** Same process the whole time (PID 9266, no restart), service still
`isForeground=true`, socket still `0100007F:2256` (loopback only), **198 `PROBE_OK` /
0 `PROBE_FAIL`**, probe latency 17–69 ms (median 29 ms), and the engine's own
lifecycle log shows no `ENGINE_STOP` and no recovery event after `BOOT_RECEIVED`.

**What I have to correct.** The overnight probe cadence is **not** 20 s. It contains
**45 gaps longer than 30 s**, the largest 680 s. The cause is not a fault in the
service: `Handler.postDelayed` is based on `SystemClock.uptimeMillis`, which **does
not advance while the device is suspended**. Measured on the device:

```
t0 : awake=20706.96  elapsed=23353.45
t30: awake=20737.07  elapsed=23501.40
over 148.0 s of wall time the device was AWAKE for 30.1 s
=> 20% awake, 80% asleep;  46 min of deep sleep since boot
```

So a "20 s" delay means **20 s of awake time**, which on a mostly-sleeping device is
minutes of wall time. That is the entire gap pattern.

**Therefore the test-5 PASS means less than I first wrote.** Test 5 was run *on USB
power*, where the SoC keeps running and `uptime` keeps advancing — which is why the
cadence held at 20.1 s there. It was a screen-off + device-idle test, **not** a real
suspend test. The overnight window is the real suspend test, and it shows the *probe*
degrades while the *server* does not.

**Corrected statement of the result:**

* **MCP availability:** supported. Across 5 h 45 m including ~46 min of real deep
  sleep, every probe that ran succeeded, the listener stayed bound to loopback, and
  the process never restarted.
* **Continuous 20 s monitoring through sleep:** **not** achieved, and I would not want
  it — forcing a wake every 20 s to run a health probe would wreck the battery, which
  is the opposite of what this node is for. A sleep gap is expected behaviour, not a
  failure, and the probe log does not currently distinguish the two. Logging the
  awake-vs-wall delta alongside each probe is a small change worth making so an
  operator cannot misread a sleep gap as an outage.

**The phone was mostly UNPLUGGED during that window**, which is why the level fell.
From `dumpsys batterystats --history`:

```
(day start) status=discharging health=good plug=none volt=4413 charge=3764 ... fg=u0a226:"org.homeguardian.edgeservice"
+5h16m01s  plug=usb  +plugged
+5h16m44s  status=discharging plug=none  -plugged
+5h16m46s  status=charging  plug=usb     +plugged
```

So the 96 % → 90 % fall was **normal battery discharge**, not a charging failure, and
the phone was only connected about 5 h 16 m into the session (the `-plugged/+plugged`
blip 43 s apart is a USB re-enumeration — the same behaviour that produced the earlier
`adb offline`). Level has since recovered to 92–93 %. `charge_counter` reads a flat
3,683,680 µAh across samples while the level rises, so **it is too coarse on this
device to act as a net-current measurement** — it must not be used as one.

**On-battery cost of the node, measured:**

| Process | CPU since boot (5 h 45 m) |
|---|---|
| `org.homeguardian.edgeservice` (our node) | **7.5 s** |
| `com.radio.pocketfm` (third-party radio app) | **463.7 s** |

The node used ~1.6 % of the CPU one unrelated third-party app on the phone burned, and
did not appear in `top` at all. The persistent load average of 1.5–2.5 on this device
is the phone's own software, not this service.

**Unattended charging stays unapproved.** The overnight run happened to be on battery,
so it does not test charging at all — and the supply is a negotiated 500 mA, which is
not a charge-level limit.

## Appendix — evidence files

| File | Contents |
|---|---|
| `logs/cmake_configure_arm64.log` | NDK cross-configure; the OpenSSL-gated skips |
| `logs/build_arm64.log` | arm64-v8a build of server + engine + bridge |
| `logs/mcp_functional_verify.txt` | the 18 MCP checks, raw |
| `logs/mi_a3_edge_measurements.txt` / `.json` | latency / CPU / RSS / thermal |
| `logs/mi_a3_edge_scenario.txt` | scenario run + alerts + joined MCP read |
| `logs/device-artifacts/` | SQLite databases pulled from the device |
