#pragma once

// The bounded device registry (milestone D-03).
//
// WHY: a network-facing ingestion boundary must know WHICH device is talking,
// whether that device is still trusted, what it is allowed to use, and where its
// message stream got to. The registry is that state, and it is deliberately
// separate from the acquisition path so that a second invariant holds:
//
//   NETWORK AUTHENTICATION IS NOT PERMISSION TO COLLECT.
//
// A device can be authenticated by certificate and still be refused, because
// paired + consented + capture-enabled are separate, independently revocable
// facts. Nothing in this class derives one from another.
//
// BOUNDS: the registry is capped (device count, capabilities per device, name
// lengths) and every mutator refuses rather than growing without limit. Stored as
// JSON so an operator can inspect and edit it; parsed strictly so a hand-edited
// file cannot smuggle in an unbounded or malformed record.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian::net {

class DeviceRegistry {
public:
    struct Config {
        size_t max_devices = 64;
        size_t max_capabilities = 16;
        int64_t online_timeout_ms = 90000;  // no heartbeat for this long -> offline
        Config() = default;
    };

    struct Device {
        std::string device_id;
        std::string display_name;
        std::string device_type;      // "android" | "hub-fixture" | ...
        std::string credential_id;    // certificate fingerprint/CN the device must present
        bool paired = false;
        bool revoked = false;         // registry-file revocation
        // Revocation can also come from the operator's revocation list, which is
        // re-read per connection. Tracked separately so that REMOVING an id from
        // that list actually lifts the revocation: with a single flag, a device
        // that was ever revoked stayed revoked for the process lifetime.
        bool revoked_by_file = false;
        std::string revocation_note;  // bounded, non-sensitive
        std::vector<std::string> capabilities;  // e.g. camera, microphone, motion
        bool has_camera = false;
        bool has_microphone = false;

        int64_t last_heartbeat_ms = 0;
        uint64_t last_sequence = 0;          // highest ACCEPTED sequence
        int64_t last_observation_at_ms = 0;  // highest accepted acquisition timestamp
        bool online = false;

        // Device-scoped consent. Separate from OS permission (which the phone
        // enforces) and from network authentication (which TLS enforces).
        bool consent_granted = false;
        std::string consent_subject;
        std::string consent_purpose;
        int64_t consent_updated_ms = 0;

        // The operator's acquisition switch for this device. Capture being
        // disabled blocks processing even when consent is recorded. The registry
        // file may SET this explicitly (paired + consent granted required), but an
        // absent key is always false and the runtime value is never inferred - a
        // device that was never authorized in writing stays unauthorized.
        bool capture_enabled = false;
        bool is_synthetic = false;
    };

    // Result of binding an authenticated credential to a device record.
    enum class AuthResult {
        ok,
        unknown_device,
        revoked,
        unpaired,
        credential_mismatch,
        no_credential
    };

    static const char* auth_result_to_string(AuthResult result);

    static Config defaults() { return Config{}; }
    explicit DeviceRegistry(Config cfg = defaults()) : cfg_(cfg) {}

    // ---- persistence (strict, bounded) ----
    bool load(const std::string& path, std::string* reason);
    bool save(const std::string& path, std::string* reason) const;

    // ---- lifecycle ----
    // Pair (or re-pair) a device. Refuses when the registry is full and the device
    // is new, or when any field violates a bound.
    bool pair(const Device& device, std::string* reason);
    bool unpair(const std::string& device_id);
    bool revoke(const std::string& device_id, const std::string& note);

    // Apply a revocation list: a bounded JSON array of device ids (or objects with
    // a "device_id"). Re-read per connection by the ingress so a revocation takes
    // effect IMMEDIATELY rather than at the next restart - a revocation that needs
    // a restart is a revocation that is still trusting the device.
    bool apply_revocation_file(const std::string& path, std::string* reason);

    // ---- lookup ----
    Device* find(const std::string& device_id);
    const Device* find(const std::string& device_id) const;

    // Bind an authenticated credential to a device. This is the identity check
    // that stops one device from acting as another.
    AuthResult authenticate(const std::string& device_id, const std::string& credential_id) const;

    // ---- consent / capture ----
    bool set_consent(const std::string& device_id, bool granted, const std::string& subject,
                     const std::string& purpose, int64_t now_ms);
    bool set_capture_enabled(const std::string& device_id, bool enabled);

    // Re-read ONLY the operator-controlled authorization fields (paired, revoked,
    // credential binding, consent, capture) from the registry file.
    //
    // Runtime observations are deliberately NOT touched: the sequence watermark, the
    // acquisition-timestamp watermark and liveness describe the WORLD, and reading
    // them back from an operator file would assert something the hub cannot know.
    // This is what makes a revocation or a consent withdrawal take effect on the NEXT
    // observation without restarting the service.
    //
    // The outcome is explicit so the caller can fail closed:
    //   refreshed - the file parsed; authorization now reflects its contents
    //   absent    - the file is gone (NOT "nothing changed")
    //   malformed - unreadable or invalid; the caller must refuse, not continue
    //   unbounded - more entries than the registry can hold
    enum class RefreshOutcome { refreshed, absent, malformed, unbounded };
    RefreshOutcome refresh_authorization(const std::string& path,
                                         std::vector<std::string>* transitions,
                                         std::string* reason);

    // Seed the replay watermark from durable state at startup. Never lowers it.
    bool seed_sequence_watermark(const std::string& device_id, uint64_t last_sequence);

    // ---- stream state ----
    // True when `sequence` is newer than the device's watermark. Does NOT advance
    // it: the accept path must check first and commit only after every other
    // check passes, so a refused message cannot consume a sequence number.
    bool sequence_is_new(const std::string& device_id, uint64_t sequence) const;
    bool commit_sequence(const std::string& device_id, uint64_t sequence);
    bool note_observation_timestamp(const std::string& device_id, int64_t acquired_at_ms);

    void note_heartbeat(const std::string& device_id, int64_t now_ms,
                        const std::vector<std::string>& capabilities);
    size_t sweep_offline(int64_t now_ms);

    // ---- introspection ----
    size_t device_count() const { return devices_.size(); }
    size_t online_count() const;
    size_t revoked_count() const;
    size_t paired_count() const;
    const Config& config() const { return cfg_; }
    std::vector<Device> snapshot() const { return devices_; }
    void clear() { devices_.clear(); }

private:
    Device* find_mutable(const std::string& device_id);

    Config cfg_;
    std::vector<Device> devices_{};
};

}  // namespace homeguardian::net
