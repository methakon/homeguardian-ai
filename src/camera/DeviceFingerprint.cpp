#include "DeviceFingerprint.h"

#include <cstdio>

namespace homeguardian::camera {

bool HardwareFingerprint::normalize(const std::string& raw, HardwareFingerprint* out,
                                    std::string* reason) {
    auto fail = [reason](const char* why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (out == nullptr) return fail("null output");

    if (raw.empty()) return fail("empty fingerprint");
    if (raw.size() > 32) return fail("fingerprint is too long to be a MAC");
    // Rule 1: an address is never an identity.
    if (raw.find('.') != std::string::npos) {
        return fail("an IP address is not a device identity: an address is assigned, "
                    "reused and recycled, so it cannot distinguish a moved camera from a "
                    "new one");
    }

    std::string hex;
    hex.reserve(12);
    for (char c : raw) {
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9') { hex.push_back(c); continue; }
        if (c >= 'a' && c <= 'f') { hex.push_back(c); continue; }
        if (c >= 'A' && c <= 'F') { hex.push_back(static_cast<char>(c - 'A' + 'a')); continue; }
        // Spaces, SSIDs, hostnames, ISO timestamps - all refused.
        return fail("not a MAC address");
    }
    if (hex.size() != 12) return fail("not a MAC address: expected twelve hex digits");

    uint64_t value = 0;
    for (char c : hex) {
        const int digit = (c <= '9') ? (c - '0') : (c - 'a' + 10);
        value = (value << 4) | static_cast<uint64_t>(digit);
    }

    // Rule 4: placeholders are not devices.
    if (value == 0) return fail("all-zero address is a placeholder, not a device");
    if (value == 0xFFFFFFFFFFFFULL) return fail("broadcast address is not a device");

    out->value_ = value;
    out->octets_ = 6;
    if (reason != nullptr) reason->clear();
    return true;
}

std::string HardwareFingerprint::canonical() const {
    if (!valid()) return {};
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
                  static_cast<unsigned long long>((value_ >> 40) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 32) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 24) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 16) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 8) & 0xFF),
                  static_cast<unsigned long long>(value_ & 0xFF));
    return std::string(buf);
}

bool HardwareFingerprint::locally_administered() const {
    if (!valid()) return false;
    return (((value_ >> 40) & 0xFF) & 0x02) != 0;
}

std::string HardwareFingerprint::oui() const {
    if (!valid()) return {};
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%02llx:%02llx:%02llx",
                  static_cast<unsigned long long>((value_ >> 40) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 32) & 0xFF),
                  static_cast<unsigned long long>((value_ >> 24) & 0xFF));
    return std::string(buf);
}

}  // namespace homeguardian::camera
