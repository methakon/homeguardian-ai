#include "net/IngressConfigPolicy.h"

namespace homeguardian::net {

bool validate_replay_configuration(const ReplayPolicyInput& input, std::string* error) {
    auto refuse = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!input.replay_state_path.empty()) return true;
    if (!input.allow_ephemeral_replay_protection) {
        // MANDATORY: an unset path must not be able to disable protection by omission.
        return refuse(
            "replay_state_path is required: durable replay protection is mandatory for the "
            "ingress. Set replay_state_path (beside audit_db_path), and set "
            "allow_ephemeral_replay_protection ONLY for an isolated test.");
    }
    const std::string& bind = input.bind_address;
    const bool loopback = bind == "127.0.0.1" || bind == "::1" || bind == "localhost";
    if (!loopback) {
        // The opt-out exists for isolated tests. Any other bind is a deployment, and a
        // deployment may not run without replay protection "because it was configured".
        return refuse("refusing to run WITHOUT durable replay protection on a non-loopback bind "
                      "address (" + bind + "): that is a live deployment, not an isolated test");
    }
    return true;
}

}  // namespace homeguardian::net
