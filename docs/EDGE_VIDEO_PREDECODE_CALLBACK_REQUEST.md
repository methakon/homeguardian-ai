# Pre-decode callback — interface request to the integration workstream

**Date:** 2026-10-25
**Workstream:** `HG-EDGE-VIDEO` (observer / pre-filter)
**Status:** REQUEST. Nothing here is implemented on the integration side. No shared file has been
edited by this workstream.
**Basis:** ruling 1, *Raw-JPEG ring*, in `EDGE_VIDEO_INTEGRATION_ACKNOWLEDGMENT.md`; interface item 2,
*Acquisition adapter point*, in `EDGE_VIDEO_FILE_OWNERSHIP_PROPOSAL.md` §6.

---

## 1. What is being asked for

One hook and one signature change in `src/bridge/CameraAnalysis.h`, implemented by the integration
workstream. The observer supplies the narrow consumer interface and a test double that exercises it.

The observer's pre-event ring is built and host-tested, and **has no caller** outside its own tests.
It therefore retains nothing in the running system, so a fired event would decode an empty window.
This request closes that gap. It is not a request to rewrite the camera pipeline, change detection,
or take over any part of the analysis path.

## 2. Where the seam is

`homeguardian::portal::CameraAnalysis::process(const std::vector<uint8_t>& jpeg)`:

| line | code | significance |
|---|---|---|
| `:131` | `void process(const std::vector<uint8_t>& jpeg)` | the fetched JPEG arrives here |
| `:136` | `const int64_t ms = ...system_clock::now()...` | **time is taken here, not received** |
| `:137` | `if(!authorize()) return;` | authorization HAS been checked |
| `:138` | `auto luma = decode_jpeg_luma(jpeg);` | **the guard runs inside this call** |
| `:139` | `if(luma.empty()) { ++decode_failures_; ... }` | refusal path |
| `:140` | `if(!authorize()) return;` | re-checked after decode |
| `:145` | `auto f = session_->ingest(luma...);` | the existing detector |

The callback must fire **between `:137` and `:138`**. That is the only point at which the fetched
bytes exist, authorization has already been checked, and nothing has consumed them yet.

## 3. Four hazards at that seam

**3.1 The bytes there are unvalidated.** The size/header/dimension guard is inside
`decode_jpeg_luma` (`:38`):

```cpp
if(jpeg.size()>512*1024 || !detail::looks_like_jpeg(jpeg) ||
   !detail::jpeg_dimensions(jpeg,&w,&h) || w>4096 || h>4096) return {};
```

A hook firing before that call receives a payload that may exceed 512 KiB, may not be a JPEG at
all, or may declare dimensions beyond the ceiling. If the ring retains it, the service has acquired
a way to retain arbitrary caller-supplied bytes. The ruling states the obligation: *"Integration
must apply equivalent checks before a new pre-decode callback can retain input."*

**3.2 There is no capture timestamp to preserve.** `:136` manufactures the time from the service's
own clock. The ruling requires *"Preserve actual sampling timestamps."* Capture time exists at
acquisition and is not currently an input to `process()`, so this is a signature change rather than
an insertion. Without it the ring's `captured_at` would be process-entry time and every clip anchor
would be wrong by the queueing delay.

**3.3 The borrow contract is undefined.** The parameter is `const std::vector<uint8_t>&` — borrowable
for the duration of the call. The ruling requires this be stated rather than assumed: *"The callback
borrows immutable bytes only for its duration; the ring owns any retained bounded copy."*

**3.4 Authority must be re-checkable at retention.** The ring already refuses to retain when
unauthorized and clears retained data on withdrawal. It needs to be told, from the integration side,
when authorization is withdrawn, the source or enrollment changes, or the lifecycle stops.

## 4. The proposed interface

Supplied by this workstream as a new file, `src/edge/FrameConsumer.h`:

```cpp
// Borrowed, immutable, valid ONLY for the duration of the call. The consumer owns any copy.
struct BorrowedFrame {
    const std::uint8_t* data = nullptr;      // the fetched JPEG bytes, validated by the caller
    std::size_t size = 0;
    int width = 0;                            // DERIVED FROM CHECKED HEADERS, not assumed
    int height = 0;
    std::chrono::system_clock::time_point captured_at;   // actual capture time
    std::uint64_t sequence = 0;               // acquisition sequence, for gap detection
};

class FrameConsumer {
public:
    virtual ~FrameConsumer() = default;
    // Called once per acquired frame, on the acquisition path.
    virtual void on_frame(const BorrowedFrame& frame) = 0;
    // Called on authorization withdrawal, source/enrollment change, lifecycle stop.
    virtual void on_authority_changed(bool authorized) = 0;
};
```

## 5. What integration must do at the seam

1. **Add the hook** between `:137` and `:138`, invoking `on_frame` with the borrowed bytes.
2. **Apply the equivalent guard BEFORE the hook** — size ceiling, magic bytes, dimensions, dimension
   ceiling. Unvalidated bytes must never reach the hook. This is the ruling's explicit requirement.
3. **Pass the actual capture timestamp** from acquisition rather than the `process()` entry clock.
   This is a signature change to `process()` or to its caller.
4. **Document the borrow as call-scoped**, with the consumer owning any retained copy.
5. **Notify on authority change** — withdrawal, source/enrollment change, lifecycle stop — so
   retained data is cleared and queued work invalidated.
6. **Derive dimensions from the checked headers** and pass them, rather than letting the consumer
   guess.

## 6. What the observer supplies

| item | state |
|---|---|
| `src/edge/EventRing.{h,cpp}` | built, in the repository, host-tested (43 checks) |
| `src/edge/FrameConsumer.h` | the interface above — to be supplied with this request |
| a test double satisfying `FrameConsumer` | proves the ring works end-to-end from borrowed bytes to a retained window |
| bounds | total bytes, frame count, age, per-frame size — already enforced and tested |
| lifecycle | `clear()` on withdrawal, source change and shutdown; already implemented and tested |

## 7. Scope limit — what this does NOT solve

This integration supplies the **missing pre-roll input**. It does not solve fast-event detection in
general. A 100–200 ms event is still captured only if **all** of the following hold:

1. the camera actually sampled the event, at whatever rate the acquisition provides;
2. those samples reached this processing path — which they do not if a cadence gate drops frames
   before detection (`main_camera_bridge.cpp:260`, which `continue`s before `session.ingest`);
3. the trigger detected the change from the samples it received.

The ring can only retain what it was given. If the samples were never taken or never reached the
callback, a perfect trigger and a perfect ring still produce nothing. **Connecting the ring does not
by itself establish that any particular event was captured.**

## 8. How this would be accepted

* the hook exists at the stated point, with validation preceding it;
* bytes handed over are within the documented limits, with dimensions from checked headers;
* the timestamp is the capture time, demonstrably not process-entry time;
* retained data is cleared and queued work invalidated on withdrawal, source change and shutdown;
* **one genuine triggered clip, produced jointly, not a synthetic stand-in** — the owner's item 2;
* measured CPU, latency, recall, queue bounds, memory and temperature on the phone before any
  power-saving claim is accepted.

## 9. Explicitly not requested

No change to detection, thresholds, the detector registry or the evidence allowlist. No second
acquisition path, poller, credential store or HTTP client. No change to authorization, provenance,
consent, alert persistence or the MCP boundaries. No new decoder at the seam. No network or disk
encoding inside the callback.
