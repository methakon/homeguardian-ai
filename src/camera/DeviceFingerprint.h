#pragma once

// HardwareFingerprint — a read-only hardware identity for a device on the LAN
// (HG-CAM-10 / Issue #19, step 1).
//
// THE PROBLEM IT SOLVES
//   "Is this a new camera, or a camera whose DHCP lease changed?" An IP address cannot
//   answer that: it is assigned, reused and recycled by the router, so treating it as
//   identity either invents phantom cameras or hides real ones. The NIC's MAC address is
//   the smallest piece of hardware identity a read-only LAN scan can obtain, so it is what
//   this type normalizes - and nothing else.
//
// THE RULES
//   1. AN ADDRESS IS NEVER AN IDENTITY. An IPv4 literal is refused outright, as is any
//      input containing '.'. That is not defensive coding for its own sake: the whole
//      failure mode this type exists to prevent is an address being mistaken for a device.
//   2. AN SSID, HOSTNAME OR TIMESTAMP IS NOT AN IDENTITY either, so only a MAC shape is
//      accepted.
//   3. A RANDOMIZED MAC IS A WEAK IDENTITY, AND SAYS SO. Android/iOS rotate
//      locally-administered (random) MACs per network, so two observations of the same
//      phone can legitimately carry different fingerprints. `durable()` is false for those,
//      and an inventory must not bind a durable device identity to one.
//   4. A PLACEHOLDER IS NOT A DEVICE. The all-zero and broadcast addresses are refused:
//      they are what a scan reports when it learned nothing.
//
// This type carries NO secret. A MAC is public on the wire by definition; it is treated as
// private in the repository only because it identifies the owner's specific hardware.

#include <cstdint>
#include <string>

namespace homeguardian::camera {

class HardwareFingerprint {
public:
    // Accepts `aa:bb:cc:dd:ee:ff`, `aa-bb-cc-dd-ee-ff` and `aabbccddeeff`, case-insensitively.
    // Returns false (and a reason) for anything else, including every IPv4 literal.
    static bool normalize(const std::string& raw, HardwareFingerprint* out, std::string* reason);

    bool valid() const { return octets_ == 6; }

    uint64_t value() const { return value_; }

    // Lowercase, colon-separated. The single form used in comparisons and in the log.
    std::string canonical() const;

    // The universally/locally administered bit of the first octet. Set means randomized.
    bool locally_administered() const;

    // True only when this fingerprint can ground a DURABLE device identity. A randomized
    // MAC cannot, because the same device may present a different one next time.
    bool durable() const { return valid() && !locally_administered(); }

    // The OUI (first three octets): the silicon vendor class, NOT the model. Two cameras
    // from one vendor share it, so it can corroborate a family but never identify a unit.
    std::string oui() const;

    bool same_vendor(const HardwareFingerprint& other) const {
        return valid() && other.valid() && oui() == other.oui();
    }

    bool operator==(const HardwareFingerprint& other) const { return value_ == other.value_; }

private:
    uint64_t value_ = 0;
    int octets_ = 0;
};

}  // namespace homeguardian::camera
