#pragma once

#include <string>

namespace homeguardian::net {

// Is durable replay protection configured for this boundary?
//
// DELIBERATELY TLS-FREE so it can be exercised by the host test suite: the rule it encodes
// is a policy decision, not a socket operation, and a policy that can only be tested inside
// a container is a policy that regresses unnoticed.
//
// THE RULE (D-03.2): durable replay protection is MANDATORY for the ingress. Without it,
// restarting the hub makes an already-accepted observation acceptable again, which is the
// exact hole the feature exists to close. There is one way to run without it, and it must
// be stated explicitly AND be unreachable from a network:
//
//   * replay_state_path non-empty           -> allowed (the normal case),
//   * empty + allow_ephemeral_replay_protection false -> REFUSED, because silence must not
//     be able to disable protection,
//   * empty + allow_ephemeral true + loopback bind -> allowed as an ISOLATED TEST,
//   * empty + allow_ephemeral true + any other bind -> REFUSED (that is a deployment).
struct ReplayPolicyInput {
    std::string replay_state_path;
    bool allow_ephemeral_replay_protection = false;
    std::string bind_address;
};

// Returns true when the configuration may run. On failure, writes the reason to `error`.
bool validate_replay_configuration(const ReplayPolicyInput& input, std::string* error);

}  // namespace homeguardian::net
