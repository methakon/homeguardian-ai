# Edge video event detection — design review and device implementation assessment

**Workstream:** `HG-EDGE-VIDEO` (analysis only; no implementation yet)
**Date:** 2026-10-24
**Status:** DESIGN REVIEWED AND DECIDED — **NOT IMPLEMENTED**. No vision runtime, model or
capture-ingestion code exists on any target.

> This document records the design discussion and the decisions taken. It is not an
> acceptance report: nothing here is deployed or live.

## 1. The problem

Continuous semantic inference on a camera feed cannot be scheduled at any single interval
without failing in one of three ways:

| cadence | failure |
|---|---|
| every ~2 minutes | blind to almost every event; a person, dog or cat crosses in seconds |
| every 1-2 seconds | sustained CPU load; the device thermally throttles; analysis cannot keep up |
| every ~5 seconds | thermally survivable, but a subject crossing in 3-4 s is still missed |

A fast subject is missed twice over: the sample may not land during the crossing, and by the
time analysis runs the subject has left the frame. Raising the sample rate does not fix this —
cost grows linearly with the rate, and the device throttles.

The required property is therefore not a better interval. It is **decoupling a cheap always-on
observation from expensive event-gated analysis**, and analysing frames captured *before* the
analysis was requested.

## 2. Design references reviewed

Three design references were reviewed and cross-checked against each other and against
measurements taken on the real hardware:

1. *Edge-Optimized Video Analytics: Two-Stage Anomaly Detection Architectures in C++* (9 pp) —
   establishes the temporal-resolution paradox, the Tier-0/Tier-1 cascade, the asymmetric
   trigger with a latch, the pre-event ring buffer, zero-copy memory discipline, and the
   inference-runtime thread settings.
2. *Efficient, Low-CPU Webcam Event Detection in C++: A Two-Tier Edge-Video Architecture* —
   reviews (1) and explicitly changes several of its parts (see §4).
3. *Efficient C++ Video Event Detection for Sub-500 ms Events Under Tight CPU, Memory and
   Thermal Budgets* (31 pp) — the most rigorous. Adds the sampling-probability and CPU-budget
   arithmetic, an explicit trigger state machine, thermal operating states, a failure-mode
   table and a production stack.

They are not independent: (1) is the origin, (2) reviews (1), (3) reviews both. Where a later
reference contradicts an earlier one it states so, which is what makes the lineage usable —
see ADR-024.

## 3. The agreed architecture

```
Camera / RTSP
   |
   v
Capture / Demux thread ──> Compressed packet ring (2-4 s)
   |                                  |
   |  (latest reduced frame)          |  (on trigger: seek to preceding keyframe,
   v                                  |   decode, analyse 3-8 frames)
Tier-0 (80x60 or 160x90)              v
   |                            Bounded event queue (1-2 jobs)
   v                                  |
Asymmetric gate ─────────────────────> Tier-1 inference thread
   ^  strong  -> fire on one frame        |
   |  weak    -> 2 of last 3 agree        v
   |  latched -> 0.8-1.5 s past last hit  Recorder / alert
   |
   +── camera-side motion event / PIR  (OR input, never AND)
   +── thermal monitor (shrinks Tier-1, never Tier-0)
```

Two independent properties make a fast subject catchable:

1. Tier-0 examines every frame, so nothing crosses unwatched.
2. The ring retains 2-4 s of footage, so analysis looks back to where the event began.

The governing sentence for the whole design: **Tier-1 does not have to finish before the
subject leaves the frame. It only has to receive a timestamp before the ring overwrites the
relevant packets.** Look-back is not an optimisation; a perfect trigger with no pre-event buffer
still produces an empty crop.

## 4. Decisions taken, and the conflicts resolved

Five decisions are recorded as ADRs (see `docs/DECISIONS.md`, ADR-020 … ADR-024). Two of them
resolve direct contradictions between the references rather than adopting the newest text:

* **Ring representation.** Reference (1) proposes retaining decoded high-resolution frames so
  Tier-1 can look back. That is the wrong representation to retain: a 2-second payload is
  141 KiB at 80x60 grayscale versus 178-356 MiB at 1080p, while the same window as compressed
  H.264 at 4 Mbit/s is ~1 MB. Compression is a ~350x reduction and the packets remain
  decodable. (ADR-023)
* **Trigger strictness.** Reference (1) recommends requiring several consecutive motion frames
  before triggering. At a 10 Hz Tier-0 rate, three consecutive observations span 200-300 ms, so
  a short crossing may never accumulate that many hits — the strict rule misses exactly the
  events the system exists to catch. Replaced with an asymmetric trigger: one strong hit fires
  immediately, weak motion requires 2 of the last 3, and a latch holds the gate open. (ADR-020)
* **"CPU burn-out."** At least one reference frames sustained inference as risking physical
  damage. It does not: the device thermally throttles. The real failure is a feedback cycle —
  sustained power → temperature rise → clock throttling → lower throughput → backlog → more
  sustained utilisation — which is why a bounded queue matters more than raw speed.

Agreed parameter ranges (bounded by the spread across the three references, not one author's
preference): Tier-0 at 80x60 or 160x90 luma, 10-15 checks/s; pixel threshold ~18; weak
0.4-0.6%; strong 2-2.5%; latch 0.8-1.5 s; pre-roll 1-3 s; 3-8 analysis frames per event.

Two operational rules that are not tuning values:

* The event timestamp must be recorded on the **first weak hit**, not when the event is
  confirmed. That timestamp is where retrospective analysis begins; anchoring at confirmation
  silently truncates the start of the crossing.
* Thermal pressure is relieved by shrinking semantic work, **never** by reducing the Tier-0
  rate. Tier-0 is the one stage that must not be degraded.

## 5. Device implementation assessment — Mi A3

The production edge node is the Xiaomi Mi A3 (`laurel_sprout`), Android 11 / API 30,
`arm64-v8a`. The following was measured directly on the connected device. Identity was taken
from the device's own properties, not from the USB descriptor string (see FRICTION_LOG
Incident 80).

### 5.1 Idle load

| quantity | value |
|---|---|
| load average | 0.00 |
| CPU | 713% of 800% idle (50% user, 30% sys) |
| cores | 8 — 4 @ 2.016 GHz + 4 @ 1.8048 GHz, governor `schedutil` |
| RAM | 5.47 GiB total, ~3.4 GiB available |
| storage | 93 GB free |
| battery | 95%, 32.0 °C, charging, health GOOD |

### 5.2 Thermal headroom — the binding constraint

The tightest passive trip point on this device is **56 °C** on the skin/quiet zone, not the
80-85 °C figure commonly assumed for small single-board computers. That is a materially tighter
envelope for any Tier-1 workload.

| zone | now | trip | headroom |
|---|---|---|---|
| quiet-therm-adc | 34 °C | **56 °C** | **21 °C** ← binding |
| skin-therm-adc | 33 °C | 125 °C | 91 °C |
| cpuss-0-usr | 38 °C | 125 °C | 86 °C |

The device has previously been observed reaching ~56 °C on the hottest zone, i.e. it *can*
reach its throttling point. Any Tier-1 budget must be derived from this trip point.

### 5.3 Compatibility

| capability | state |
|---|---|
| ABI / SIMD | `arm64-v8a`, NEON/ASIMD, AES, SHA1/2, CRC32 — the vectorisation the Tier-0 path needs |
| CPU↔camera reachability | verified: the device reaches the LAN camera on the same subnet over HTTP |
| frame acquisition | verified: HTTP 200, a valid JPEG payload, ~28 ms round trip |
| toolchain | NDK r26d + `cmake` present on the build host; `ninja` absent by design (see §6) |
| existing on-device runtime | `org.homeguardian.edgeservice` v1.1.1, installed and running |

### 5.4 What is actually running on the device

| component | state |
|---|---|
| `org.homeguardian.edgeservice` v1.1.1 | **running** as a foreground service; MCP endpoint on loopback only, answering `401` to anonymous callers |
| `org.homeguardian.harness` v0.2.0 | installed, **not running**; requests CAMERA and RECORD_AUDIO, neither granted |
| staged adb-launched binaries | present on disk, **not executing** |
| vision runtime / model / capture ingestion | **absent** — no runtime, no model file, no camera-ingesting process |

The running edge service holds only `INTERNET`, `FOREGROUND_SERVICE`, `RECEIVE_BOOT_COMPLETED`
and `ACCESS_NETWORK_STATE`. It requests no camera or microphone permission, which is a
deliberate property of the design, not an omission (ADR-022).

## 6. Implementation route — decided

1. **Native NDK / Bionic, `arm64-v8a`, API 30.** Not Termux, not a PRoot/glibc userland. The
   comparison is decisive: a few MB on disk versus hundreds of MB, and PRoot intercepts every
   syscall through `ptrace`. The cross-compile is already scripted and documented in
   `docs/EDGE_DEVICE_MI_A3.md`; the APK build is scripted under `apk_service/`. New vision code
   belongs **inside the existing native service**, not in a second application.
2. **Ingest the LAN camera over the network, not the device camera.** The device↔camera path is
   verified reachable and the service is already a network client with no camera permission.
3. **Add Tier-0 as a stage in the existing foreground service**, shaped as §3: reduced-luma
   change detection on the fetched frames, asymmetric gate, compressed pre-roll, bounded 1-2 job
   queue, then an INT8 classifier on the motion region.

**Tier-0 is a composition of existing components, not new image processing.** The change metric
and the false-positive discriminators this design proposes already exist, are allocation-free and
host-tested: `MotionFrameFeatures::changed_fraction` is the "changed pixels / pixels" score, and
`RegionChangeFeatures` already provides illumination/auto-gain rejection (`gain_correlation`), the
compact-versus-global distinction (`peak_cell_share`, `spread_radius`) and the changed-mass
location that serves as the motion ROI (`centroid_*`) — all in `src/core/BoundedFeatures.*` under
the W-02 workstream, with the separation validated on real frames in
`docs/CHANGE_CHARACTER_SEPARATION.md`. The workstream's new work is the gate, the ring, the queue,
the thermal controller and the fetch path — **not** the measurement. Scope, ownership boundaries
and the anti-duplicate list are in `docs/EDGE_VIDEO_IMPLEMENTATION_PLAN.md`, which must be read
before any code is written here.

Sequencing: implement and measure Tier-0 first, on the real device, against the 56 °C trip
point, before any Tier-1 inference is written — because Tier-0 is the stage that must never be
degraded.

## 7. Open items and blockers

* **Not implemented.** No vision code, runtime or model exists on the device or in the tree.
  Everything in §3 and §6 is design.
* **TLS-bound ingestion is unavailable on this node.** The NDK toolchain ships no OpenSSL, so
  the mutual-TLS ingestion path is not available until OpenSSL is cross-compiled for Android
  (recorded previously in `docs/EDGE_DEVICE_MI_A3.md` §9 condition 6). This does not affect
  plain-HTTP camera acquisition, which needs no TLS.
* **Restart resilience is unproven.** A previously recorded defect: an Android-managed service
  restart did not complete on its own. This affects any always-on monitoring role.
* **On-battery behaviour is unproven for continuous operation.** Earlier measurement showed the
  probe cadence degrading on battery while the server continued to serve. Continuous monitoring
  assumes the device stays responsive; that assumption is untested.
* **Platform currency.** Android 11 with a 2022-08-01 security patch; SELinux enforcing;
  verified boot green; no root. Platform limits cannot be worked around on this device.
* **Expected first measurement.** Tier-0 at 160x90 / 10-15 Hz should land in the low single-digit
  percentage of one core on this class of SoC; the existing service already demonstrates ~0.15%
  of one core for its MCP workload. This is an expectation to be measured, not a result.

## 8. Non-actions taken deliberately

No camera or microphone on the device was activated. No device state was modified; every check
was read-only. No new listener, permission, package or configuration was introduced.
