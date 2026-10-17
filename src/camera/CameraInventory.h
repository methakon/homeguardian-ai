#pragma once

// CameraInventory — the durable hardware identity layer for cameras
// (HG-CAM-10 / Issue #19, steps 1, 3 and 8).
//
// WHAT IT ANSWERS, AND WHY THAT IS THE HARD PART
//   A read-only LAN pass yields addresses and open ports. The question the owner actually
//   asked is "is this a THIRD camera, or the second camera whose DHCP lease moved?" - and
//   answering it wrongly in either direction is a real defect: guess "new" and the registry
//   fills with phantom duplicates; guess "moved" and a genuinely new camera is silently
//   presented as an existing one, so its consent and authorization are inherited from a
//   device that is not there.
//
//   So identity here is the NIC fingerprint (`HardwareFingerprint`) plus, for a device that
//   is to be REGISTERED, a stable owner-provided device identifier from the vendor app.
//   An address is never identity: it is recorded as an attachment in a bounded history,
//   exactly as `ObservedEndpoint` does for a site connector.
//
// WHAT IT REFUSES TO DO
//   - It does not register anything on an address alone (`onboard` needs an owner-provided
//     identity AND an explicit authorization).
//   - It does not bind a durable identity to a randomized MAC: a phone that rotates its MAC
//     would otherwise be onboarded as a new camera every time.
//   - It does not resolve an INCONSISTENT pass. If one NIC appears at two addresses, or one
//     address is claimed by two NICs, the pass is reported as conflicting and NOTHING is
//     applied - because in that case we cannot say which observation is current.
//   - It cannot express "working media". `MediaStatus` has no such member: the best it can
//     say is that a vendor-protocol-only interface was observed.
//
// It is deliberately separate from `net::DeviceRegistry`, which holds AUTHORIZATION
// (paired / consent / capture / revocation) and from `SiteConnector`, which enforces it per
// frame. This layer only decides WHO a device is; it grants nothing.

#include "CameraTypes.h"
#include "DeviceFingerprint.h"

#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian::camera {

// What a read-only pass can honestly say about a device's media interface.
enum class InterfaceClass {
    unknown,
    vendor_protocol_only,             // only the vendor's own listeners answered
    standard_media_answering,         // a standard media port answered (access NOT evaluated here)
    no_responders                     // reachable, but nothing on any documented port
};

inline const char* interface_class_to_string(InterfaceClass c) {
    switch (c) {
        case InterfaceClass::unknown:            return "unknown";
        case InterfaceClass::vendor_protocol_only: return "vendor_protocol_only";
        case InterfaceClass::standard_media_answering: return "standard_media_answering";
        case InterfaceClass::no_responders:      return "no_responders";
    }
    return "unknown";
}

// The media readiness ladder. There is deliberately NO "working" member: only a real,
// authenticated session on the physical device could earn one, and this layer cannot
// observe that.
enum class MediaStatus {
    unknown,
    no_interface_observed,            // nothing on the documented ports
    vendor_protocol_only,             // vendor listeners only; no supported media path
    standard_media_unauthenticated,   // a standard interface answered WITHOUT proving access control
    standard_media_requires_auth      // a standard interface answered and access control is enforced
};

inline const char* media_status_to_string(MediaStatus s) {
    switch (s) {
        case MediaStatus::unknown:                       return "unknown";
        case MediaStatus::no_interface_observed:         return "no_interface_observed";
        case MediaStatus::vendor_protocol_only:          return "vendor_protocol_only";
        case MediaStatus::standard_media_unauthenticated: return "standard_media_unauthenticated";
        case MediaStatus::standard_media_requires_auth:  return "standard_media_requires_auth";
    }
    return "unknown";
}

struct CameraObservation {
    HardwareFingerprint fingerprint{};
    std::string address{};                 // OBSERVED. Never identity.
    InterfaceClass interface_class = InterfaceClass::unknown;
    bool rtsp_port_answering = false;      // documented standard media port
    bool onvif_port_answering = false;     // documented ONVIF port
    bool vendor_port_answering = false;    // the vendor's own listener(s)
    int64_t observed_at_ms = 0;
    std::string model_hint{};              // ONLY when a source actually reported it
};

enum class ObservationOutcome {
    known_at_same_address,
    known_moved,                 // DHCP churn: same NIC, new address, identity unchanged
    new_distinct_device,         // an unseen DURABLE fingerprint: a candidate, not a camera
    unusable_fingerprint,        // invalid or randomized: cannot ground a durable identity
    conflicting_fingerprint,     // one NIC reported at two addresses in one pass
    conflicting_address          // one address claimed by two different NICs
};

inline const char* observation_outcome_to_string(ObservationOutcome o) {
    switch (o) {
        case ObservationOutcome::known_at_same_address: return "known_at_same_address";
        case ObservationOutcome::known_moved:            return "known_moved";
        case ObservationOutcome::new_distinct_device:    return "new_distinct_device";
        case ObservationOutcome::unusable_fingerprint:   return "unusable_fingerprint";
        case ObservationOutcome::conflicting_fingerprint:return "conflicting_fingerprint";
        case ObservationOutcome::conflicting_address:    return "conflicting_address";
    }
    return "unknown";
}

enum class HealthState { unknown, online, offline };

inline const char* health_state_to_string(HealthState h) {
    switch (h) {
        case HealthState::unknown: return "unknown";
        case HealthState::online:  return "online";
        case HealthState::offline: return "offline";
    }
    return "unknown";
}

struct CameraRecord {
    LogicalDeviceId id{};                  // durable logical identity, independent of address
    std::string owner_identity_label{};    // the stable owner/app-provided identifier
    HardwareFingerprint fingerprint{};
    std::string site_id{};
    bool authorized = false;               // set only by an explicit owner authorization

    int64_t first_seen_ms = 0;
    int64_t last_seen_ms = 0;
    std::string last_address{};            // attachment, not identity
    uint32_t address_changes = 0;
    std::vector<std::string> address_history{};   // bounded

    InterfaceClass interface_class = InterfaceClass::unknown;
    MediaStatus media = MediaStatus::unknown;
    std::string firmware_model{};          // EMPTY unless actually read from the device
    bool firmware_verified = false;
    HealthState health = HealthState::unknown;
};

// A distinct device that has been SEEN but not registered. Kept so an owner decision can be
// made about a real observation instead of a re-scan.
struct CandidateRecord {
    HardwareFingerprint fingerprint{};
    std::string last_address{};
    int64_t first_seen_ms = 0;
    int64_t last_seen_ms = 0;
    InterfaceClass interface_class = InterfaceClass::unknown;
    uint32_t sightings = 0;
};

struct PassReport {
    bool consistent = false;                       // no conflict: the pass may be applied
    std::vector<ObservationOutcome> outcomes{};    // parallel to the observation vector
    size_t known_same_address = 0;
    size_t known_moved = 0;
    size_t new_devices = 0;
    size_t unusable = 0;
    std::vector<std::string> conflicts{};          // bounded, non-sensitive
    std::string summary{};
};

class CameraInventory {
public:
    struct Config {
        size_t max_cameras = 32;
        size_t max_candidates = 32;
        size_t max_address_history = 8;
        int64_t offline_after_ms = 300000;   // no sighting for this long -> offline
    };

    // Two constructors rather than one defaulted argument: a nested aggregate with default
    // member initializers cannot be spelled `= {}` in this position.
    CameraInventory() = default;
    explicit CameraInventory(Config cfg) : cfg_(cfg) {}

    const Config& config() const { return cfg_; }

    // Resolve a whole read-only pass. Detects the two conflicts a per-device call cannot
    // see, because they are properties of the PASS. When the pass is inconsistent, no
    // record is touched - a conflicting observation is not evidence.
    PassReport reconcile_pass(const std::vector<CameraObservation>& pass, int64_t now_ms);

    // Resolve and apply a single observation of a KNOWN device (endpoint + health).
    // A previously unseen durable fingerprint is recorded as a candidate and reported as
    // `new_distinct_device`; it is NOT registered.
    ObservationOutcome observe(const CameraObservation& obs, int64_t now_ms, std::string* reason);

    // Register a distinct device. Requires ALL of:
    //   - a valid, durable fingerprint (a randomized MAC cannot ground an identity),
    //   - a stable owner-provided device identifier that is not an IP address (this is the
    //     identity a duplicate cannot fake, and its absence is what makes an onboarding
    //     BLOCKED rather than merely pending),
    //   - an explicit owner authorization for THIS device.
    bool onboard(const CameraObservation& obs, const std::string& site_id,
                 const std::string& owner_provided_identity, bool owner_authorized,
                 int64_t now_ms, std::string* reason);

    // Evidence, recorded as evidence. A firmware string is NEVER inferred from an OUI.
    bool note_firmware(const LogicalDeviceId& id, const std::string& model, bool verified,
                       std::string* reason);
    bool set_media_status(const LogicalDeviceId& id, MediaStatus status, std::string* reason);

    size_t sweep_offline(int64_t now_ms);

    const CameraRecord* find(const LogicalDeviceId& id) const;
    const CameraRecord* find_by_fingerprint(const HardwareFingerprint& fp) const;
    std::vector<CameraRecord> snapshot() const { return records_; }
    std::vector<CandidateRecord> candidates() const { return candidates_; }
    size_t size() const { return records_.size(); }

    // ---- persistence (strict, bounded) ----
    bool save(const std::string& path, std::string* reason) const;
    bool load(const std::string& path, std::string* reason);

private:
    CameraRecord* find_mutable(const LogicalDeviceId& id);
    CameraRecord* find_mutable_by_fingerprint(const HardwareFingerprint& fp);
    CandidateRecord* find_candidate(const HardwareFingerprint& fp);
    CandidateRecord& note_candidate(const CameraObservation& obs, int64_t now_ms);
    bool apply_known(CameraRecord& record, const CameraObservation& obs, int64_t now_ms);

    Config cfg_;
    std::vector<CameraRecord> records_{};
    std::vector<CandidateRecord> candidates_{};
};

}  // namespace homeguardian::camera
