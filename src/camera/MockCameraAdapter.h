#pragma once

// MockCameraAdapter — a deterministic, in-process stand-in for a camera
// (milestone HG-V380-09, task 3).
//
// WHAT IT IS FOR
//   The registry, consent, rejection, reconnection and revocation rules can all be
//   exercised TODAY, against a real `DeviceRegistry` and the real event pipeline,
//   without touching a physical camera — which matters because neither V380 unit may
//   be used: camera 1's standard interface is unauthenticated and disabled, and
//   camera 2 has no standard interface and unverified vendor authentication.
//
// WHAT IT PROVES, AND WHAT IT DOES NOT
//   A passing test here proves the BOUNDARY works: that authorization is consulted
//   before every frame, that denial categories are distinct, that a revocation stops
//   a running stream, and that a connector cannot emit an observation for a device it
//   may not use. It proves NOTHING about V380 media: `adapter_class()` returns
//   `mock`, the connector derives `Provenance::synthetic` from it, and `probe()`
//   reports `unreachable` forever unless a script says otherwise. No socket is
//   opened and no bytes leave the process.
//
// DETERMINISM
//   Every behaviour is a scripted field. Frames are accounted for by METADATA ONLY
//   (size, keyframe flag, monotonically increasing PTS) and no payload is ever
//   produced, buffered or handed out, so a mock cannot become a place where "media"
//   quietly accumulates.

#include "ICameraAdapter.h"

#include <cstdint>
#include <optional>
#include <string>

namespace homeguardian::camera {

// The scripted behaviour of one mock device. Copyable and inspectable so a test can
// state the device it is standing in for.
struct MockScript {
    // What the (pretend) device claims it can do. A declaration is not a grant.
    CapabilitySet capabilities{};

    ConnectOutcome probe_outcome = ConnectOutcome::ok;
    ConnectOutcome start_outcome = ConnectOutcome::ok;
    ConnectOutcome control_outcome = ConnectOutcome::ok;

    uint32_t frames_per_read = 1;      // frames accounted for per read_frames() call
    uint32_t frame_bytes = 1024;       // size reported per frame (never allocated)
    std::string codec = "h265";
    bool keyframe_every_10th = true;

    // 0 = never. After N successful reads the adapter behaves like this family does
    // when it drops a client: it closes with NO payload and reports
    // `transport_closed` (measured on a real V380: idle close ~5.0 s, close on an
    // unrecognized payload ~0.21 s, zero bytes returned).
    uint32_t close_after_reads = 0;

    // When true, a start/control attempt fails `unreachable` — the device is gone.
    bool unreachable = false;
};

// Convenience: a mock that looks like a camera that CANNOT do video, i.e. the shape
// camera 2 is actually in (only vendor ports listen; no documented media interface).
MockScript no_video_device_script();

// Convenience: a mock for a device whose interface exists but is not safe to use —
// the shape camera 1 is actually in (anonymous live video and state disclosure).
MockScript unsafe_interface_device_script();

class MockCameraAdapter final : public ICameraAdapter {
public:
    MockCameraAdapter(LogicalDeviceId id, std::string attachment_summary, MockScript script);

    const LogicalDeviceId& id() const override { return id_; }
    AdapterClass adapter_class() const override { return AdapterClass::mock; }
    std::string attachment_summary() const override { return attachment_; }
    CapabilitySet declared_capabilities() const override { return script_.capabilities; }

    ConnectOutcome probe() override;
    ConnectOutcome start_stream(const StreamRequest& request) override;
    void stop_stream() noexcept override;
    bool streaming() const override { return streaming_; }
    size_t read_frames(FrameMeta* out, size_t max) override;
    ConnectOutcome control(ControlCommand command) override;
    ConnectOutcome last_outcome() const override { return last_outcome_; }
    uint64_t frames_accounted() const override { return frames_accounted_; }

    // Test affordances (not part of ICameraAdapter).
    void set_script(const MockScript& script) { script_ = script; }
    const MockScript& script() const { return script_; }
    uint32_t reads_performed() const { return reads_performed_; }
    uint32_t control_calls() const { return control_calls_; }
    ControlCommand last_control() const { return last_control_; }
    void set_forced_outcome(ConnectOutcome outcome) { forced_outcome_opt_ = outcome; }
    void clear_forced_outcome() { forced_outcome_opt_.reset(); }

private:
    LogicalDeviceId id_;
    std::string attachment_;
    MockScript script_;
    bool streaming_ = false;
    std::optional<ConnectOutcome> forced_outcome_opt_{};   // when set, overrides script_
    ConnectOutcome last_outcome_ = ConnectOutcome::ok;
    uint64_t frames_accounted_ = 0;
    uint32_t reads_performed_ = 0;
    uint32_t control_calls_ = 0;
    ControlCommand last_control_ = ControlCommand::ptz_home;
    int64_t next_pts_ms_ = 0;
};

}  // namespace homeguardian::camera
