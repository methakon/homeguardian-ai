#pragma once

// V380SourceAdapter: the authorized V380 EC39 camera, modelled from MEASUREMENTS rather than assumptions.
//
// WHY IT REFUSES BY DEFAULT
//   The device was measured by a separate workstream (docs/HG_V380_01..08, branch
//   experiment/hg-v380-ec39-integration): it exposes **no RTSP, ONVIF, HTTP or UPnP/mDNS interface in its
//   default state**, its port 9800 resets on any payload, 554/8554 are refused and the documented ONVIF port
//   8899 is refused. RTSP and ONVIF are vendor-app switches which that workstream documented and began
//   verifying. So an adapter that "connects" today would be inventing a protocol. This one carries the
//   measured facts, refuses with a code and a named owner action while no verified local interface exists,
//   and accepts real frames only once such an interface has been verified - its own contract does not change
//   when that happens.
//
// WHAT IT DOES DO
//   Buffers frames through the bounded media buffer (frame ceiling, byte ceiling, drop accounting, purge on
//   consent withdrawal), keeps media strictly local, and treats identity as the ENROLLED identifier, never
//   as an address: a camera that changes IP after a DHCP lease must remain the same logical source, which is
//   exactly the re-discovery problem docs/HG_V380_07_NETWORK_CHANGE_REDISCOVERY.md records.
//
// WHAT IT IS NOT: not a claim that this worker verified the device, not an RTSP or ONVIF implementation, and
// not a path for media to reach the MCP control plane. Frames never become observations here; only bounded
// scalars derived from them would.

#include "core/BoundedMediaBuffer.h"

#include <cstdint>
#include <string>

namespace homeguardian {

// Each field states whether it was MEASURED, DOCUMENTED or NOT VERIFIED. A field that was not verified says
// so, and nothing downstream may read an unverified field as a capability.
struct V380Capabilities {
    std::string model = "V380 EC39";
    std::string evidence = "docs/HG_V380_02_MEDIA_PROTOCOLS.md, docs/HG_V380_04_ONVIF_ACTIVATION_VERIFICATION.md";
    bool verified_by_this_worker = false;        // built on another workstream's measurements
    bool rtsp_listener_present = false;          // MEASURED: absent in the default state
    bool onvif_listener_present = false;         // MEASURED: absent; vendor switch on 8899/UDP
    bool rtsp_documentedly_enableable = true;    // DOCUMENTED by the vendor/app; not verified by this worker
    std::string documented_main_stream_path = "/live/ch00_1";
    std::string documented_sub_stream_path = "/live/ch00_0";
    std::string documented_video_codec = "H.264";
    bool microphone_verified = false;            // NOT VERIFIED
    bool talkback_verified = false;              // NOT VERIFIED: two-way audio is never silently enabled
    // True only when a verified, authenticated local interface exists for this deployment.
    bool local_interface_available = false;
};

enum class V380Refusal {
    ok,
    no_verified_local_interface,   // the honest default: nothing verified to connect to yet
    consent_withheld,              // capture without consent is refused, not queued
    identity_unverified,           // no enrolled identity: an address is not an identity
    not_opened,
    buffer_rejected,               // the bounded buffer refused the frame
};

const char* v380_refusal_to_string(V380Refusal refusal);

class V380SourceAdapter {
public:
    struct Config {
        // The ENROLLED logical identity (from DeviceRegistry). Required: an IP address is not an identity.
        std::string enrolled_identity;
        // Where the device currently answers. Explicitly NOT used as identity, and expected to change.
        std::string endpoint_host;
        bool consent_granted = false;
        // Rehearsal only: a synthetic frame producer, so the downstream path is testable without the device.
        bool synthetic_fixture = false;
        size_t fixture_frames = 4;
        MediaBufferConfig buffer{};
    };

    explicit V380SourceAdapter(Config cfg);

    const V380Capabilities& capabilities() const { return caps_; }
    const Config& config() const { return cfg_; }

    // Refuses unless a verified local interface exists (or the synthetic fixture was explicitly requested)
    // AND consent is granted AND the identity is enrolled. `error` names the owner action when it refuses.
    V380Refusal open(std::string* error);
    void close() { opened_ = false; }

    // One frame, bounded. Returns false with a refusal when nothing was produced - never a blank frame.
    bool read_frame(MediaFrame* out, V380Refusal* refusal, std::string* reason);

    // Consent withdrawn: stop intake and discard everything buffered.
    size_t withdraw_consent(const std::string& why);

    // The device reappears at a different address. Identity is unchanged and nothing else about the adapter
    // changes: this is the DHCP case, and the point is that it is not an identity event.
    void update_endpoint(const std::string& host) { cfg_.endpoint_host = host; }
    const std::string& identity() const { return cfg_.enrolled_identity; }

    const MediaBufferStats& stats() const { return buffer_.stats(); }
    size_t buffered_frames() const { return buffer_.size(); }

private:
    Config cfg_{};
    V380Capabilities caps_{};
    BoundedMediaBuffer buffer_;
    bool opened_ = false;
    bool consent_ = false;   // granted at construction, and never silently turned back on
    bool revoked_ = false;   // consent withdrawn after construction
    uint64_t next_frame_id_ = 1;
};

}  // namespace homeguardian
