#include "V380CameraAdapter.h"

#include <chrono>

namespace homeguardian::camera {

V380CameraAdapter::V380CameraAdapter(Options options)
    : options_(std::move(options)),
      source_adapter_(options_.source_config) {
    if (options_.source_config.synthetic_fixture) {
        options_.adapter_class = AdapterClass::mock;
    }
}

CapabilitySet V380CameraAdapter::declared_capabilities() const {
    CapabilitySet caps;
    caps.add(Capability::video);
    caps.add(Capability::motion_events);
    return caps;
}

ConnectOutcome V380CameraAdapter::probe() {
    if (!options_.id.valid()) {
        last_outcome_ = ConnectOutcome::credentials_rejected;
        return last_outcome_;
    }
    if (options_.source_config.endpoint_host.empty()) {
        last_outcome_ = ConnectOutcome::unreachable;
        return last_outcome_;
    }
    last_outcome_ = ConnectOutcome::ok;
    return last_outcome_;
}

ConnectOutcome V380CameraAdapter::start_stream(const StreamRequest& request) {
    (void)request;
    const auto refusal = source_adapter_.open(&open_error_);
    switch (refusal) {
        case V380Refusal::ok:
            streaming_ = true;
            last_outcome_ = ConnectOutcome::ok;
            return last_outcome_;
        case V380Refusal::no_verified_local_interface:
        case V380Refusal::consent_withheld:
            streaming_ = false;
            last_outcome_ = ConnectOutcome::refused_unsafe_interface;
            return last_outcome_;
        case V380Refusal::identity_unverified:
            streaming_ = false;
            last_outcome_ = ConnectOutcome::credentials_rejected;
            return last_outcome_;
        default:
            streaming_ = false;
            last_outcome_ = ConnectOutcome::stream_absent;
            return last_outcome_;
    }
}

void V380CameraAdapter::stop_stream() noexcept {
    streaming_ = false;
    source_adapter_.close();
    have_previous_frame_ = false;
    last_outcome_ = ConnectOutcome::ok;
}

size_t V380CameraAdapter::read_frames(FrameMeta* out, size_t max) {
    if (!streaming_ || max == 0) {
        return 0;
    }

    size_t accounted = 0;
    for (size_t i = 0; i < max; ++i) {
        MediaFrame frame;
        V380Refusal refusal = V380Refusal::ok;
        std::string reason;
        if (!source_adapter_.read_frame(&frame, &refusal, &reason)) {
            if (refusal == V380Refusal::consent_withheld) {
                last_outcome_ = ConnectOutcome::refused_unsafe_interface;
                stop_stream();
            }
            break;
        }

        if (out != nullptr) {
            out[accounted].bytes = static_cast<uint32_t>(frame.bytes.size());
            out[accounted].keyframe = true;
            out[accounted].pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        frame.captured_at.time_since_epoch())
                                        .count();
            out[accounted].codec = "raw_luma";
        }
        ++frames_accounted_;
        ++accounted;

        // Pass decoded frames into the motion analysis pipeline
        if (have_previous_frame_) {
            auto motion_obs = derive_motion_observation(
                previous_frame_, frame, options_.frame_width, options_.frame_height,
                options_.delta_threshold);
            if (motion_obs.valid) {
                if (motion_sink_) {
                    motion_sink_(motion_obs);
                }
                if (recent_motion_observations_.size() >= 32) {
                    recent_motion_observations_.erase(recent_motion_observations_.begin());
                }
                recent_motion_observations_.push_back(motion_obs);
            }
        }
        previous_frame_ = std::move(frame);
        have_previous_frame_ = true;
    }

    return accounted;
}

ConnectOutcome V380CameraAdapter::control(ControlCommand command) {
    (void)command;
    last_outcome_ = ConnectOutcome::capability_absent;
    return last_outcome_;
}

bool V380CameraAdapter::register_into_inventory(CameraInventory& inventory,
                                                const RegistrationOptions& options,
                                                std::string* reason) {
    if (!options.id.valid()) {
        if (reason != nullptr) *reason = "invalid logical device id";
        return false;
    }
    HardwareFingerprint fp;
    std::string norm_err;
    if (!HardwareFingerprint::normalize(options.hardware_mac, &fp, &norm_err)) {
        if (reason != nullptr) *reason = norm_err;
        return false;
    }
    if (!fp.durable()) {
        if (reason != nullptr) *reason = "fingerprint is randomized; cannot ground durable device identity";
        return false;
    }
    CameraObservation obs;
    obs.fingerprint = fp;
    obs.address = options.endpoint_host;
    obs.interface_class = InterfaceClass::vendor_protocol_only;
    obs.vendor_port_answering = true;
    obs.observed_at_ms = options.now_ms;

    if (!inventory.onboard(obs, options.id.site_id, options.owner_identity_label,
                           options.owner_authorized, options.now_ms, reason)) {
        return false;
    }
    const auto* rec = inventory.find_by_fingerprint(fp);
    if (rec != nullptr) {
        inventory.note_firmware(rec->id, "V380 EC39", true, nullptr);
        inventory.set_media_status(rec->id, MediaStatus::vendor_protocol_only, nullptr);
    }
    return true;
}

bool V380CameraAdapter::register_into_device_registry(net::DeviceRegistry& registry,
                                                      const LogicalDeviceId& id,
                                                      const std::string& credential_id,
                                                      bool consent_granted,
                                                      bool capture_enabled,
                                                      int64_t now_ms) {
    if (!id.valid()) return false;
    net::DeviceRegistry::Device d;
    d.device_id = id.canonical();
    d.display_name = "V380 EC39";
    d.device_type = "camera";
    d.credential_id = credential_id;
    d.paired = true;
    d.has_camera = true;
    d.capabilities = {"camera"};
    std::string why;
    if (!registry.pair(d, &why)) return false;
    if (consent_granted) {
        if (!registry.set_consent(d.device_id, true, "household", "presence", now_ms)) return false;
    }
    if (capture_enabled) {
        if (!registry.set_capture_enabled(d.device_id, true)) return false;
    }
    return true;
}

}  // namespace homeguardian::camera
