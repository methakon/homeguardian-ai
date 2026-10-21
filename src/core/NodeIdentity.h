#pragma once

// NodeIdentity - who this acquisition node is.
//
// A node needs a stable identifier so the hub (milestone D-03) can register,
// authorize, revoke and attribute observations to a specific phone, and so the
// dashboard can show which camera saw something. The identity is deliberately
// NON-SENSITIVE:
//
//   * node_id is a hash of a device-provided seed (Android ID). It is an opaque
//     identifier, not a secret and not reversible to the raw seed. Authentication
//     for the network boundary is D-03's job and uses real key material, not this.
//   * display_name / model are operator-visible labels, sanitised and bounded so
//     a hostile or odd device string cannot inject control characters or bloat a
//     log line.
//
// Nothing here reads telemetry, contacts, accounts or any personal data.

#include <string>

namespace homeguardian {

struct NodeIdentity {
    std::string node_id;       // "hg-" + 16 hex chars, derived from the seed
    std::string display_name;  // <= 32 visible chars, sanitised
    std::string platform;      // e.g. "android"
    int api_level = 0;         // e.g. 27
    std::string abi;           // e.g. "armeabi-v7a"
    std::string model;         // bounded device model string, may be empty
};

// Deterministic, opaque id from a device seed. Empty seed -> a well-known
// "unknown node" id rather than a random value, so behaviour is reproducible.
std::string make_node_id(const std::string& device_seed);

// Strip control characters, collapse whitespace, bound to `max_chars`.
std::string sanitize_label(const std::string& raw, size_t max_chars = 32);

// Assemble an identity, sanitising every free-form field.
NodeIdentity make_node_identity(const std::string& device_seed,
                                const std::string& raw_display_name,
                                int api_level,
                                const std::string& abi,
                                const std::string& raw_model);

// Compact, bounded, non-sensitive summary for a log line or the on-screen panel
// ("node_id=... name=... api=... abi=... model=...").
std::string node_identity_summary(const NodeIdentity& id);

// True when the identity is usable for registration (id present and sane).
bool node_identity_valid(const NodeIdentity& id);

}  // namespace homeguardian
