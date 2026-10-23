#include "SiteProfile.h"

#include <cstdlib>

namespace homeguardian::camera {

bool SiteProfile::parse_ipv4(const std::string& in, uint32_t* out_host_order) {
    if (out_host_order == nullptr) return false;
    uint32_t value = 0;
    int octets = 0;
    size_t i = 0;
    while (i < in.size()) {
        size_t j = i;
        while (j < in.size() && in[j] >= '0' && in[j] <= '9') ++j;
        if (j == i || j - i > 3) return false;              // empty or over-long octet
        const int part = std::atoi(in.substr(i, j - i).c_str());
        if (part > 255) return false;
        value = (value << 8) | static_cast<uint32_t>(part);
        ++octets;
        i = j;
        if (i < in.size()) {
            if (in[i] != '.') return false;
            ++i;
            if (i == in.size()) return false;               // trailing dot
        }
    }
    if (octets != 4) return false;
    *out_host_order = value;
    return true;
}

bool Ipv4Cidr::parse(const std::string& in, Ipv4Cidr* out) {
    if (out == nullptr) return false;
    const size_t slash = in.find('/');
    if (slash == std::string::npos) return false;           // a bare address is not a reach rule
    const std::string addr = in.substr(0, slash);
    const std::string prefix_text = in.substr(slash + 1);
    if (prefix_text.empty() || prefix_text.size() > 2) return false;
    for (char c : prefix_text) if (c < '0' || c > '9') return false;
    const int prefix = std::atoi(prefix_text.c_str());
    if (prefix < 0 || prefix > 32) return false;

    uint32_t network = 0;
    if (!SiteProfile::parse_ipv4(addr, &network)) return false;

    out->network = network;
    out->prefix = static_cast<uint32_t>(prefix);
    out->text = in;
    return true;
}

}  // namespace homeguardian::camera
