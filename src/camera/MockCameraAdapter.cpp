#include "MockCameraAdapter.h"

namespace homeguardian::camera {

MockScript no_video_device_script() {
    // The shape camera 2 is actually in: no documented media interface exists at all
    // (only the vendor's 8800/9800 listen, and its media/authentication behaviour is
    // unverified). No video capability is declared, so a stream request must fail
    // `capability_absent` rather than fall back to something else.
    MockScript s;
    s.capabilities = CapabilitySet{};   // deliberately empty
    s.probe_outcome = ConnectOutcome::ok;   // it IS reachable on the vendor ports
    s.start_outcome = ConnectOutcome::capability_absent;
    s.frames_per_read = 0;
    return s;
}

MockScript unsafe_interface_device_script() {
    // The shape camera 1 is actually in: a standard interface exists but served live
    // video and device state with no authentication. An adapter for such a device must
    // be able to say so and be left stopped.
    MockScript s;
    s.capabilities.add(Capability::video);
    s.capabilities.add(Capability::ptz);
    s.probe_outcome = ConnectOutcome::refused_unsafe_interface;
    s.start_outcome = ConnectOutcome::refused_unsafe_interface;
    s.frames_per_read = 0;
    return s;
}

MockCameraAdapter::MockCameraAdapter(LogicalDeviceId id, std::string attachment_summary,
                                     MockScript script)
    : id_(std::move(id)), attachment_(std::move(attachment_summary)), script_(std::move(script)) {}

ConnectOutcome MockCameraAdapter::probe() {
    if (forced_outcome_opt_) { last_outcome_ = *forced_outcome_opt_; return last_outcome_; }
    if (script_.unreachable) { last_outcome_ = ConnectOutcome::unreachable; return last_outcome_; }
    last_outcome_ = script_.probe_outcome;
    return last_outcome_;
}

ConnectOutcome MockCameraAdapter::start_stream(const StreamRequest& request) {
    // A declared capability is a precondition, not a permission: the connector has
    // already asked the registry before calling, and this only checks that the device
    // can actually do what was requested.
    if (!script_.capabilities.has(Capability::video)) {
        last_outcome_ = ConnectOutcome::capability_absent;
        return last_outcome_;
    }
    if (forced_outcome_opt_) { last_outcome_ = *forced_outcome_opt_; streaming_ = false; return last_outcome_; }
    if (script_.unreachable) { last_outcome_ = ConnectOutcome::unreachable; streaming_ = false; return last_outcome_; }
    last_outcome_ = script_.start_outcome;
    streaming_ = (last_outcome_ == ConnectOutcome::ok);
    reads_performed_ = 0;
    next_pts_ms_ = 0;
    // `request` is honoured by the caller (the connector pulls a bounded number per
    // call); a mock has no queue depth to configure.
    (void)request;
    return last_outcome_;
}

void MockCameraAdapter::stop_stream() noexcept {
    streaming_ = false;
}

size_t MockCameraAdapter::read_frames(FrameMeta* out, size_t max) {
    if (!streaming_ || out == nullptr || max == 0) return 0;

    // A forced non-ok outcome means the transport is gone: behave like this
    // family does and end the stream with NO payload and no diagnosis.
    if (forced_outcome_opt_ && *forced_outcome_opt_ != ConnectOutcome::ok) {
        streaming_ = false;
        last_outcome_ = *forced_outcome_opt_;
        return 0;
    }

    if (script_.close_after_reads != 0 && reads_performed_ >= script_.close_after_reads) {
        // Behave like the real family: close with no payload and no diagnosis.
        streaming_ = false;
        last_outcome_ = ConnectOutcome::transport_closed;
        return 0;
    }

    const size_t n = (script_.frames_per_read < max) ? script_.frames_per_read : max;
    for (size_t i = 0; i < n; ++i) {
        FrameMeta f;
        f.bytes = script_.frame_bytes;
        const uint64_t index = frames_accounted_ + i;
        f.keyframe = script_.keyframe_every_10th ? (index % 10 == 0) : false;
        f.pts_ms = next_pts_ms_;
        f.codec = script_.codec.empty() ? "unknown" : script_.codec;
        out[i] = f;
        next_pts_ms_ += 40;   // 25 fps, deterministically
    }
    frames_accounted_ += n;
    ++reads_performed_;
    last_outcome_ = ConnectOutcome::ok;
    return n;
}

ConnectOutcome MockCameraAdapter::control(ControlCommand command) {
    last_control_ = command;
    ++control_calls_;

    const bool is_ptz = (command != ControlCommand::light_on && command != ControlCommand::light_off);
    const Capability needed = is_ptz ? Capability::ptz : Capability::illumination;
    if (!script_.capabilities.has(needed)) {
        last_outcome_ = ConnectOutcome::capability_absent;
        return last_outcome_;
    }
    if (forced_outcome_opt_) { last_outcome_ = *forced_outcome_opt_; return last_outcome_; }
    if (script_.unreachable) { last_outcome_ = ConnectOutcome::unreachable; return last_outcome_; }
    last_outcome_ = script_.control_outcome;
    return last_outcome_;
}

}  // namespace homeguardian::camera
