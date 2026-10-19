#include "backend/linux/V380SourceAdapter.h"

namespace homeguardian {

const char* v380_refusal_to_string(V380Refusal refusal) {
    switch (refusal) {
        case V380Refusal::ok:                        return "ok";
        case V380Refusal::no_verified_local_interface: return "no_verified_local_interface";
        case V380Refusal::consent_withheld:          return "consent_withheld";
        case V380Refusal::identity_unverified:       return "identity_unverified";
        case V380Refusal::not_opened:                return "not_opened";
        case V380Refusal::buffer_rejected:           return "buffer_rejected";
    }
    return "not_opened";
}

V380SourceAdapter::V380SourceAdapter(Config cfg)
    : cfg_(std::move(cfg)), buffer_(cfg_.buffer), consent_(cfg_.consent_granted) {}

V380Refusal V380SourceAdapter::open(std::string* error) {
    const auto refuse = [&](V380Refusal code, const std::string& why) {
        if (error != nullptr) *error = why;
        return code;
    };

    if (cfg_.enrolled_identity.empty()) {
        return refuse(V380Refusal::identity_unverified,
                      "no enrolled identity: the endpoint address (" + cfg_.endpoint_host +
                          ") is where the device answers today, and must never be its identity");
    }
    if (!cfg_.consent_granted) {
        return refuse(V380Refusal::consent_withheld,
                      "consent is not granted for this source: capture is not started and nothing is queued");
    }
    if (!caps_.local_interface_available && !cfg_.synthetic_fixture) {
        return refuse(V380Refusal::no_verified_local_interface,
                      "the V380 EC39 exposes no verified local interface in its default state (evidence: " +
                          caps_.evidence +
                          "). OWNER ACTION: enable RTSP or ONVIF in the V380 Pro app on the same LAN, tell "
                          "me which switch you enabled, and I will re-verify with a bounded probe before this "
                          "adapter is pointed at the device. Until then it refuses rather than guessing a "
                          "protocol.");
    }
    opened_ = true;
    return V380Refusal::ok;
}

bool V380SourceAdapter::read_frame(MediaFrame* out, V380Refusal* refusal, std::string* reason) {
    const auto fail = [&](V380Refusal code, const std::string& why) {
        if (refusal != nullptr) *refusal = code;
        if (reason != nullptr) *reason = why;
        return false;
    };
    // The two refusal causes are kept INDEPENDENT, because a caller must be able to tell them apart:
    //   - consent absent or withdrawn -> an authorization event; retrying will not help.
    //   - never opened (open() refused) -> a lifecycle state; something else must change first.
    // The first version conflated them, and reported "not opened" for a revocation (a caller could read a
    // revocation as a transient state) and then "consent withheld" for an adapter that never opened at all.
    if (!consent_ || revoked_) {
        return fail(V380Refusal::consent_withheld,
                    revoked_ ? "consent was withdrawn: no frame is produced"
                             : "consent is not granted: capture is not started and nothing is queued");
    }
    if (!opened_) return fail(V380Refusal::not_opened, "the adapter is not open");

    // Rehearsal source: a bounded synthetic frame so the downstream path can be exercised without the
    // device. It is explicitly NOT the camera: `verified_by_this_worker` stays false and the class stays
    // synthetic.
    if (cfg_.synthetic_fixture && !caps_.local_interface_available) {
        if (next_frame_id_ > cfg_.fixture_frames) {
            return fail(V380Refusal::ok, "the synthetic fixture produced its bounded run of frames");
        }
        MediaFrame frame;
        frame.frame_id = next_frame_id_++;
        frame.captured_at = std::chrono::system_clock::now();
        frame.bytes.assign(1024, static_cast<uint8_t>(next_frame_id_ & 0xFF));
        const auto outcome = buffer_.enqueue(std::move(frame), reason);
        if (outcome == MediaEnqueueOutcome::rejected_disconnected ||
            outcome == MediaEnqueueOutcome::rejected_oversized) {
            return fail(V380Refusal::buffer_rejected, reason != nullptr ? *reason : "buffer rejected the frame");
        }
        if (out) {
            if (!buffer_.dequeue(out)) return fail(V380Refusal::ok, "no frame available");
        }
        if (refusal != nullptr) *refusal = V380Refusal::ok;
        return true;
    }

    // The real path. It is reachable only once a verified local interface exists, and it is deliberately not
    // implemented against an unverified protocol: writing RTSP or ONVIF code for a capability the device was
    // measured NOT to expose is how invented protocol support gets shipped.
    return fail(V380Refusal::no_verified_local_interface,
                "no verified stream reader: the local interface has not been verified for this deployment");
}

size_t V380SourceAdapter::withdraw_consent(const std::string& why) {
    revoked_ = true;          // sticky: consent is never silently re-granted by a later open()
    consent_ = false;
    opened_ = false;
    std::string reason;
    const size_t discarded = buffer_.purge(&reason);
    (void)why;
    return discarded;
}

}  // namespace homeguardian
