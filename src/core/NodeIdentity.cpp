#include "NodeIdentity.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace homeguardian {

namespace {

// FNV-1a (64-bit) over the seed bytes. A hash is sufficient here: node_id is an
// opaque label used for registration/attribution, not a credential. D-03 adds
// real key material for authentication.
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;   // offset basis
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;             // FNV prime
    }
    return h;
}

}  // namespace

std::string make_node_id(const std::string& device_seed) {
    if (device_seed.empty()) {
        return "hg-0000000000000000";      // reproducible "unknown node"
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "hg-%016llx",
                  static_cast<unsigned long long>(fnv1a64(device_seed)));
    return std::string(buf);
}

std::string sanitize_label(const std::string& raw, size_t max_chars) {
    if (max_chars == 0) return std::string();
    std::string out;
    out.reserve(max_chars);
    bool last_space = false;
    for (unsigned char c : raw) {
        if (out.size() >= max_chars) break;
        // Drop control characters and anything non-printable-ASCII; keep UTF-8
        // bytes (>= 0x80) so a device name is not mangled into mojibake.
        if (c < 0x20 || c == 0x7f) continue;
        if (c == ' ') {
            if (last_space || out.empty()) continue;   // collapse/trim spaces
            last_space = true;
            out.push_back(' ');
            continue;
        }
        last_space = false;
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

NodeIdentity make_node_identity(const std::string& device_seed,
                                const std::string& raw_display_name,
                                int api_level,
                                const std::string& abi,
                                const std::string& raw_model) {
    NodeIdentity id;
    id.node_id = make_node_id(device_seed);
    id.display_name = sanitize_label(raw_display_name, 32);
    if (id.display_name.empty()) id.display_name = "unnamed node";
    id.platform = "android";
    id.api_level = api_level;
    id.abi = sanitize_label(abi, 24);
    id.model = sanitize_label(raw_model, 40);
    return id;
}

std::string node_identity_summary(const NodeIdentity& id) {
    std::string s = "node_id=" + id.node_id + " name=\"" + id.display_name + "\"";
    if (!id.platform.empty()) s += " platform=" + id.platform;
    if (id.api_level > 0) s += " api=" + std::to_string(id.api_level);
    if (!id.abi.empty()) s += " abi=" + id.abi;
    if (!id.model.empty()) s += " model=\"" + id.model + "\"";
    return s;
}

bool node_identity_valid(const NodeIdentity& id) {
    // "hg-" + 16 lowercase hex characters, and a non-empty display name.
    if (id.node_id.size() != 19) return false;
    if (id.node_id.compare(0, 3, "hg-") != 0) return false;
    for (size_t i = 3; i < id.node_id.size(); ++i) {
        const char c = id.node_id[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return !id.display_name.empty();
}

}  // namespace homeguardian
