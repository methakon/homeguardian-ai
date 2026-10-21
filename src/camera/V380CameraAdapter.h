#pragma once

// V380CameraAdapter — adapts V380SourceAdapter to the ICameraAdapter seam,
// connects to the main HomeGuardian camera registry (CameraInventory & DeviceRegistry),
// and actively streams decoded frames into the motion analysis pipeline.

#include "CameraInventory.h"
#include "CameraTypes.h"
#include "ICameraAdapter.h"
#include "backend/linux/V380SourceAdapter.h"
#include "core/LocalMediaObservation.h"
#include "net/DeviceRegistry.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace homeguardian::camera {

class V380CameraAdapter final : public ICameraAdapter {
public:
    using MotionCallback = std::function<void(const LocalMediaObservation&)>;

    struct Options {
        LogicalDeviceId id;
        std::string attachment_summary;
        std::string observed_ip;
        V380SourceAdapter::Config source_config;
        uint32_t frame_width = 64;
        uint32_t frame_height = 48;
        uint8_t delta_threshold = FeatureBounds::kDefaultPixelDelta;
        AdapterClass adapter_class = AdapterClass::vendor_protocol_unverified;
    };

    struct RegistrationOptions {
        LogicalDeviceId id;
        std::string hardware_mac;          // for HardwareFingerprint
        std::string endpoint_host;         // current observed IP
        std::string owner_identity_label;  // stable owner/app identity (not IP)
        bool owner_authorized = true;
        int64_t now_ms = 0;
    };

    explicit V380CameraAdapter(Options options);
    ~V380CameraAdapter() override = default;

    // ---- ICameraAdapter implementation ----
    const LogicalDeviceId& id() const override { return options_.id; }
    AdapterClass adapter_class() const override { return options_.adapter_class; }
    std::string attachment_summary() const override { return options_.attachment_summary; }
    std::string observed_ip() const override {
        return options_.observed_ip.empty() ? options_.source_config.endpoint_host : options_.observed_ip;
    }
    CapabilitySet declared_capabilities() const override;

    ConnectOutcome probe() override;
    ConnectOutcome start_stream(const StreamRequest& request) override;
    void stop_stream() noexcept override;
    bool streaming() const override { return streaming_; }
    size_t read_frames(FrameMeta* out, size_t max) override;
    ConnectOutcome control(ControlCommand command) override;
    ConnectOutcome last_outcome() const override { return last_outcome_; }
    uint64_t frames_accounted() const override { return frames_accounted_; }

    // ---- Motion pipeline wiring ----
    void set_motion_sink(MotionCallback sink) { motion_sink_ = std::move(sink); }
    const std::vector<LocalMediaObservation>& recent_motion_observations() const {
        return recent_motion_observations_;
    }

    // Direct access to underlying source adapter
    V380SourceAdapter& source_adapter() { return source_adapter_; }
    const V380SourceAdapter& source_adapter() const { return source_adapter_; }

    // ---- Main camera registry wiring helpers ----
    // Registers and onboards the camera into CameraInventory (durable identity layer).
    static bool register_into_inventory(CameraInventory& inventory,
                                        const RegistrationOptions& options,
                                        std::string* reason);

    // Registers and pairs the camera in DeviceRegistry (authorization layer).
    static bool register_into_device_registry(net::DeviceRegistry& registry,
                                              const LogicalDeviceId& id,
                                              const std::string& credential_id,
                                              bool consent_granted = true,
                                              bool capture_enabled = true,
                                              int64_t now_ms = 0);

private:
    Options options_;
    V380SourceAdapter source_adapter_;
    bool streaming_ = false;
    uint64_t frames_accounted_ = 0;
    ConnectOutcome last_outcome_ = ConnectOutcome::ok;
    std::string open_error_;

    // Frame history for derive_motion_observation
    bool have_previous_frame_ = false;
    MediaFrame previous_frame_{};
    MotionCallback motion_sink_ = nullptr;
    std::vector<LocalMediaObservation> recent_motion_observations_{};
};

}  // namespace homeguardian::camera
