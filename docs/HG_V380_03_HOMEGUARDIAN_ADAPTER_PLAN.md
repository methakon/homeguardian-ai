# HG-V380-03 — HomeGuardian Adapter Plan for the V380 Camera

**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Depends on:** `docs/HG_V380_01_DISCOVERY.md` (identity), `docs/HG_V380_02_MEDIA_PROTOCOLS.md` (surface)

**This is a PLAN. No adapter code is written or committed in this milestone**, and nothing in
it is claimed as implemented. The brief's acceptance for this stage is discovery + capability
verification + an integration plan; the plan below is written against the interfaces that
already exist in this repository, with file paths, so that the implementing milestone cannot
drift from the project's architecture.

---

## 1. Hard constraint from the measured state

The camera currently exposes **no** RTSP, ONVIF or HTTP interface
(`HG_V380_02` §1). Therefore:

- **The adapter cannot be implemented against RTSP/ONVIF today** — there is nothing to
  connect to. Writing `V380RtspDevice` now would be unverifiable code, which this project's
  own rules forbid ("never claim a detector is implemented until inference runs on real
  input").
- The **gate is an owner action**: enable *ONVIF setting* in the app, then re-test for
  unauthenticated access before anything is built.
- The adapter therefore ships as a **plan + a seam**, with the transport chosen at the moment
  an authenticated interface exists.

## 2. Where the adapter plugs in (existing interfaces)

| Existing component | Path | Role for the adapter |
|---|---|---|
| `IMediaDevice` | `src/core/IMediaDevice.h` | The lifecycle contract the camera backend must satisfy: `Idle → Initialized → Capturing → Stopped`, `fail()` on error, deterministic `stop()`+`close()` in the destructor, `kind() == DeviceKind::camera` |
| `ConsentGuardedDevice` | `src/core/ConsentGuardedDevice.h` | The ONLY way the adapter may be started: `initialize()`, `start()`, `recheck_and_enforce()` and `authorize_delivery()` before every frame |
| `ConsentGate` | `src/core/ConsentGate.h` | Fail-closed decision point; supplies `DenyReason` in a closed vocabulary (`capture_disabled`, `no_consent_record`, `expired`, `ambiguous`, …) |
| `AcquisitionRequest` | `src/core/ConsentGate.h` | `{subject_scope, purpose, data_category}` — a camera stream is `data_category = "camera"` under an explicit purpose |
| `Observation` | `src/core/Observation.h` | The only allowed output shape: provenance + detector + status. **Never** a raw frame |
| `Pipeline` / `Application::process_event()` | `src/core/Pipeline.cpp` | Every observation flows through the EXISTING pipeline — no second event engine |
| `Config::media_capture_enabled` | `src/core/Config.{h,cpp}` | Kill switch; `false` must make the gate deny with `capture_disabled` |
| `FakeMediaDevice` | `src/core/FakeMediaDevice.h` | The host-test double the adapter's tests reuse (no camera on the test host) |
| `SimulatorRuntime` | `src/core/SimulatorRuntime.{h,cpp}` | Where a real device would be registered in place of the simulator backend |

There is **no** `src/devices/` directory today; the natural home for a real camera backend is a
new `src/backend/v380/` sibling to `src/backend/android/`, `src/backend/simulator/`,
`src/backend/linux/`.

## 3. Proposed shape

```
src/backend/v380/
  V380CameraDevice.h/.cpp        // IMediaDevice implementation; owns the connection lifecycle
  V380StreamDescriptor.h         // parsed stream info: codec, resolution, fps, channels — data only
  V380Credentials.h/.cpp         // loads host+credentials from a file; refuses world/group-readable
  V380Transport.h                // the SEAM: abstract open/read/close so the protocol is swappable
```

`V380Transport` is the important one. Given the measured reality (§HG_V380_02 §1) the
transport implementations that can ever exist are:

- `RtspOnvifTransport` — **the only one in scope**: RTSP over TCP with ONVIF for control.
  Requires the owner-enabled interface; testable end-to-end with `ffprobe`/`ffmpeg` and a
  real socket.
- `ProprietaryHdTransport` — speaking the reverse-engineered 8800 protocol. **NOT in scope.**
  It is a "proprietary-protocol bypass" under the brief's own wording and requires a separate,
  explicit owner decision after a security review (see `HG_V380_02` §3.4 and the family's
  documented password-packet key issue).

Keeping the transport behind a seam is what lets the device, consent, observation and pipeline
layers be written and host-tested **once**, with the same rules that produced the working
`ITransport` seam on the D-02x/Home Assistant adapter.

## 4. Rules the adapter must obey (each one already a project invariant)

1. **Consent before capability.** The adapter must not open a socket before
   `ConsentGuardedDevice::initialize()` authorizes. The connect itself is protected
   acquisition: point an unauthenticated probe at the camera and it is still the camera.
2. **Per-payload authorization.** Every delivered frame/sample is preceded by
   `authorize_delivery()`; a `false` means drop the payload and the device has been forced out
   of capture. No batching around the check.
3. **Credentials never leave the credential layer.** Read from a file that is refused if
   group/other-readable (the pattern already used for the Alexa credential store and the D-03
   ingress token); never logged; never placed in an `Observation`, a status payload, a
   `describe()`, or an error string. The camera's UID/device ID is treated as a credential.
4. **No cloud, no relay, no exposure.** No VPS media path, no transcoding on the Oracle box, no
   port forwarding, no public listener. The stream is pulled from the LAN and processed on the
   node (or the hub's own loopback surface) — the VPS coordinates connections and does nothing
   else.
5. **Closed observation vocabulary.** The adapter may emit *presence/activity/energy* class
   observations with explicit provenance. It may NOT emit identity, species, emotion, or
   "what a person was doing" claims, and no scalar (frame count, motion energy) may be
   promoted into a semantic claim anywhere in the adapter, its logs or its docs.
6. **Honest failure taxonomy.** Because the camera returns nothing on an unauthenticated or
   malformed exchange (measured: 0 bytes in ~0.2 s), the adapter must classify its own errors
   (`credentials_rejected`, `transport_closed`, `stream_absent`, `codec_unsupported`,
   `idle_timeout`) and never relay device text as if it were a device diagnosis.
7. **Reconnect is a first-class state, not an exception.** The camera closes idle connections
   at ~5.0 s and closes on unrecognized input; the adapter needs a bounded keepalive and a
   bounded reconnect with backoff, and must count reconnects rather than hiding them.
8. **One clock.** Timestamps share the wall clock used elsewhere in the project
   (`system_clock`), never a boot-relative counter.

## 5. Integration steps (for the implementing milestone)

1. Owner enables ONVIF in the app; `HG_V380_02` §4.1 re-check for unauthenticated access is
   performed and recorded BEFORE any code is written. If unauthenticated access appears → stop.
2. Re-run the discovery probe set; capture the new port state (`554`, `8899`) as evidence.
3. Reach the stream with the most basic authenticated client available (`ffprobe` on the
   documented candidate paths) and record the ACTUAL working URL, codec, resolution, FPS. This
   is the measurement the brief's section 4 asks for; the candidate paths are candidates.
4. Implement `V380CameraDevice` + `V380Transport` (RTSP/ONVIF only) against the existing
   interfaces; register it as a backend alongside the simulator.
5. Host tests (no camera needed): lifecycle contract, consent denial → no connection opened,
   mid-stream withdrawal → forced stop, credential-file permission refusal, error
   classification, connector counts. Reuse `FakeMediaDevice` for the contract cases.
6. Real-camera acceptance: authenticated stream, N frames delivered, consent withdrawal
   mid-session, `capture_disabled` kill switch, reconnect after a forced drop — each recorded
   as measured output, not narration.
7. Docs: the milestone's own report (`docs/HG_V380_04_*`), plus STATUS/ROADMAP rows.

**Every step above that depends on a working stream stays BLOCKED until step 1 is done by the
owner.** Steps 1–3 are owner-gated; steps 4–6 are agent-gated on step 3's result.

## 6. Capabilities: verified / not measured

| Capability (from the brief) | Status |
|---|---|
| Video: resolution, FPS, codec, timestamps, duration, dropped frames, reconnect | **NOT MEASURED** — no interface |
| Fixed vs PTZ lens: separate streams or combined frame | **NOT MEASURED** — no interface |
| Audio: present? codec? | **NOT MEASURED** — no interface |
| Two-way talkback on a documented authenticated interface | **NOT MEASURED** — no interface; and it must be a *documented* interface before it is even considered |
| PTZ control | **NOT MEASURED** — no interface |
| Illumination (night vision) | **NOT MEASURED** — no interface |
| Alerts (motion / person / ONVIF event) | **NOT MEASURED** — no interface |

Nothing in this table may be filled from the owner's historical specification list; it is
listed in `HG_V380_01` §4 as *reported*, not verified.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Enabling ONVIF exposes the stream without authentication | Mandatory re-check of `HG_V380_02` §4.1 before proceeding; stop and document the security impact if it appears |
| Firmware does not support ONVIF (vendor says the switch is hidden when unsupported) | Report the negative result; do not fall back to the SD-card route or the proprietary protocol without a new explicit decision |
| Attaching a real camera to the node widens the privacy surface | Consent gate before connect, per-payload authorization, closed observation vocabulary, no raw frame persistence, no cloud path |
| Adapter written before a stream exists becomes unverifiable code | The plan's step order: measurement (3) strictly before implementation (4) |
| DHCP address changes and the adapter silently points at the wrong device | Verify the device identity from the authenticated stream/app, not from the IP alone; a DHCP reservation may be proposed but requires owner approval |

## 8. Owner decisions required

1. Open the V380 app → *Settings → Advanced settings → ONVIF setting*: is the switch present,
   and is it to be enabled? (If absent, the workstream stops at a documented negative.)
2. Confirm the identity read-out from `HG_V380_01` §4 (model, firmware, IP, MAC) so the
   LAN-verified device can be closed against the owner's unit.
3. Approve a DHCP reservation on the ZTE F670L for the camera (optional, separate approval —
   it is a router change).
4. Confirm that the proprietary TCP/8800 path is to remain **out of scope** (current position:
   yes — it is forbidden this milestone and needs its own decision).

**Status: plan complete and grounded in existing interfaces; implementation BLOCKED on owner
decision 1. No code, no camera configuration change, no router change, and no credential was
touched by this milestone.**
