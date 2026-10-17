#include "net/DeviceRegistry.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace homeguardian::net {

namespace {

constexpr size_t kMaxIdChars = 96;
constexpr size_t kMaxNameChars = 64;
constexpr size_t kMaxTypeChars = 32;
constexpr size_t kMaxNoteChars = 160;
constexpr size_t kMaxCapabilityChars = 32;

bool bounded(const std::string& s, size_t max_chars) { return s.size() <= max_chars; }

bool credential_id_shape_ok(const std::string& credential_id) {
    // A certificate fingerprint (hex, possibly colon-separated) or a short CN.
    if (credential_id.empty() || credential_id.size() > 128) return false;
    for (char c : credential_id) {
        const bool hexish = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                            c == ':' || c == '-' || c == '_' || c == '.' ||
                            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!hexish) return false;
    }
    return true;
}

bool device_shape_ok(const DeviceRegistry::Device& device, size_t max_capabilities,
                     std::string* reason) {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (device.device_id.empty()) return fail("device_id is empty");
    if (!bounded(device.device_id, kMaxIdChars)) return fail("device_id too long");
    if (!bounded(device.display_name, kMaxNameChars)) return fail("display_name too long");
    if (!bounded(device.device_type, kMaxTypeChars)) return fail("device_type too long");
    if (!bounded(device.revocation_note, kMaxNoteChars)) return fail("revocation_note too long");
    if (!bounded(device.consent_subject, kMaxIdChars)) return fail("consent_subject too long");
    if (!bounded(device.consent_purpose, kMaxTypeChars)) return fail("consent_purpose too long");
    if (device.capabilities.size() > max_capabilities) return fail("too many capabilities");
    for (const std::string& cap : device.capabilities) {
        if (cap.empty() || !bounded(cap, kMaxCapabilityChars)) return fail("capability invalid");
    }
    if (!device.credential_id.empty() && !credential_id_shape_ok(device.credential_id)) {
        return fail("credential_id has an unexpected shape");
    }
    return true;
}

nlohmann::json to_json(const DeviceRegistry::Device& d) {
    nlohmann::json j;
    j["device_id"] = d.device_id;
    j["display_name"] = d.display_name;
    j["device_type"] = d.device_type;
    j["credential_id"] = d.credential_id;
    j["paired"] = d.paired;
    j["revoked"] = d.revoked;
    if (!d.revocation_note.empty()) j["revocation_note"] = d.revocation_note;
    j["capabilities"] = d.capabilities;
    j["has_camera"] = d.has_camera;
    j["has_microphone"] = d.has_microphone;
    j["last_heartbeat_ms"] = d.last_heartbeat_ms;
    j["last_sequence"] = d.last_sequence;
    j["last_observation_at_ms"] = d.last_observation_at_ms;
    j["online"] = d.online;
    j["consent_granted"] = d.consent_granted;
    j["consent_subject"] = d.consent_subject;
    j["consent_purpose"] = d.consent_purpose;
    j["consent_updated_ms"] = d.consent_updated_ms;
    j["capture_enabled"] = d.capture_enabled;
    j["is_synthetic"] = d.is_synthetic;
    return j;
}

}  // namespace

const char* DeviceRegistry::auth_result_to_string(AuthResult result) {
    switch (result) {
        case AuthResult::ok: return "ok";
        case AuthResult::unknown_device: return "device_unknown";
        case AuthResult::revoked: return "device_revoked";
        case AuthResult::unpaired: return "device_unpaired";
        case AuthResult::credential_mismatch: return "identity_mismatch";
        case AuthResult::no_credential: return "no_credential";
    }
    return "unauthorized";
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

bool DeviceRegistry::load(const std::string& path, std::string* reason) {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("registry file is not readable");

    nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded()) return fail("registry file is not valid JSON");

    // Accept either {"devices":[...]} or a bare array.
    const nlohmann::json* list = nullptr;
    if (parsed.is_array()) {
        list = &parsed;
    } else if (parsed.is_object() && parsed.contains("devices") && parsed["devices"].is_array()) {
        list = &parsed["devices"];
    } else {
        return fail("registry must be an array or an object with a 'devices' array");
    }
    if (list->size() > cfg_.max_devices) return fail("registry file exceeds the device bound");

    std::vector<Device> loaded;
    std::unordered_set<std::string> seen;
    for (const auto& entry : *list) {
        if (!entry.is_object()) return fail("device entry is not an object");
        Device d;
        d.device_id = entry.value("device_id", std::string());
        d.display_name = entry.value("display_name", std::string());
        d.device_type = entry.value("device_type", std::string());
        d.credential_id = entry.value("credential_id", std::string());
        d.paired = entry.value("paired", false);
        d.revoked = entry.value("revoked", false);
        d.revocation_note = entry.value("revocation_note", std::string());
        d.has_camera = entry.value("has_camera", false);
        d.has_microphone = entry.value("has_microphone", false);
        d.consent_granted = entry.value("consent_granted", false);
        d.consent_subject = entry.value("consent_subject", std::string());
        d.consent_purpose = entry.value("consent_purpose", std::string());
        // Restore persisted runtime metrics and replay sequence watermark
        d.last_heartbeat_ms = entry.value("last_heartbeat_ms", 0LL);
        d.last_sequence = entry.value("last_sequence", 0ULL);
        d.last_observation_at_ms = entry.value("last_observation_at_ms", 0LL);
        d.consent_updated_ms = entry.value("consent_updated_ms", 0LL);
        d.is_synthetic = (d.device_type == "simulator");
        // A device is online ONLY if persisted online=true, paired, unrevoked, and authenticated
        d.online = entry.value("online", false) && d.paired && !d.revoked && !d.credential_id.empty();
        // Capture is OFF unless the registry explicitly authorizes it AND the
        // device is paired AND consent is recorded. An absent key, an unpaired
        // device or a consentless device is always false: the fail-closed default
        // holds, and there is no path that infers authorization from anything but
        // the operator's own written entry.
        d.capture_enabled = entry.value("capture_enabled", false) && d.paired &&
                            d.consent_granted && !d.revoked;

        if (entry.contains("capabilities")) {
            if (!entry["capabilities"].is_array()) return fail("capabilities is not an array");
            for (const auto& cap : entry["capabilities"]) {
                if (!cap.is_string()) return fail("capability is not a string");
                d.capabilities.push_back(cap.get<std::string>());
            }
        }

        std::string why;
        if (!device_shape_ok(d, cfg_.max_capabilities, &why)) {
            return fail("invalid device '" + d.device_id + "': " + why);
        }
        if (!seen.insert(d.device_id).second) return fail("duplicate device_id in registry");
        loaded.push_back(std::move(d));
    }
    devices_ = std::move(loaded);
    if (reason != nullptr) reason->clear();
    return true;
}

bool DeviceRegistry::save(const std::string& path, std::string* reason) const {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    nlohmann::json out = nlohmann::json::object();
    nlohmann::json list = nlohmann::json::array();
    for (const Device& d : devices_) list.push_back(to_json(d));
    out["devices"] = list;
    out["updated_at_ms"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Write-then-rename so a crash cannot leave a truncated registry.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return fail("registry temp file is not writable");
        file << out.dump(2) << "\n";
        if (!file.good()) return fail("registry write failed");
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) return fail("registry rename failed");
    if (reason != nullptr) reason->clear();
    return true;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

DeviceRegistry::Device* DeviceRegistry::find_mutable(const std::string& device_id) {
    for (Device& d : devices_) {
        if (d.device_id == device_id) return &d;
    }
    return nullptr;
}

DeviceRegistry::Device* DeviceRegistry::find(const std::string& device_id) {
    return find_mutable(device_id);
}

const DeviceRegistry::Device* DeviceRegistry::find(const std::string& device_id) const {
    for (const Device& d : devices_) {
        if (d.device_id == device_id) return &d;
    }
    return nullptr;
}

bool DeviceRegistry::pair(const Device& device, std::string* reason) {
    std::string why;
    if (!device_shape_ok(device, cfg_.max_capabilities, &why)) {
        if (reason != nullptr) *reason = why;
        return false;
    }
    Device* existing = find_mutable(device.device_id);
    if (existing != nullptr) {
        // Re-pairing keeps the trust facts the operator set explicitly, and
        // always clears runtime state.
        const bool consent_granted = existing->consent_granted;
        const std::string subject = existing->consent_subject;
        const std::string purpose = existing->consent_purpose;
        *existing = device;
        existing->is_synthetic = (existing->device_type == "simulator");
        existing->consent_granted = consent_granted;
        existing->consent_subject = subject;
        existing->consent_purpose = purpose;
        existing->last_sequence = 0;
        existing->last_observation_at_ms = 0;
        existing->last_heartbeat_ms = 0;
        existing->online = false;
        if (reason != nullptr) reason->clear();
        return true;
    }
    if (devices_.size() >= cfg_.max_devices) {
        if (reason != nullptr) *reason = "registry is full";
        return false;
    }
    Device to_insert = device;
    to_insert.is_synthetic = (to_insert.device_type == "simulator");
    devices_.push_back(to_insert);
    if (reason != nullptr) reason->clear();
    return true;
}

bool DeviceRegistry::unpair(const std::string& device_id) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return false;
    d->paired = false;
    d->consent_granted = false;
    d->capture_enabled = false;
    d->online = false;
    d->last_sequence = 0;
    d->last_observation_at_ms = 0;
    return true;
}

bool DeviceRegistry::revoke(const std::string& device_id, const std::string& note) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return false;
    if (d->revoked) return false;  // already revoked
    d->revoked = true;
    d->revocation_note = note.size() > kMaxNoteChars ? note.substr(0, kMaxNoteChars) : note;
    // Revocation is total: it also drops consent, capture and liveness, so no
    // residual flag can keep a revoked device partially authorized.
    d->consent_granted = false;
    d->capture_enabled = false;
    d->online = false;
    d->paired = false;
    return true;
}

bool DeviceRegistry::apply_revocation_file(const std::string& path, std::string* reason) {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Absent is the normal case (nothing revoked) and is not an error.
        if (reason != nullptr) reason->clear();
        return true;
    }
    const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded()) return fail("revocation file is not valid JSON");
    if (!parsed.is_array() || parsed.size() > cfg_.max_devices * 4) {
        return fail("revocation file must be a bounded array");
    }
    for (const auto& entry : parsed) {
        std::string id;
        std::string note = "revoked via revocation file";
        if (entry.is_string()) {
            id = entry.get<std::string>();
        } else if (entry.is_object()) {
            id = entry.value("device_id", std::string());
            note = entry.value("reason", note);
        } else {
            return fail("revocation entries must be a string or an object");
        }
        if (id.empty() || id.size() > kMaxIdChars) return fail("revocation entry has no usable id");
        Device* d = find_mutable(id);
        if (d == nullptr) continue;  // revoking an unknown device is a no-op, not an error
        d->revoked_by_file = true;
        d->revoked = true;
        d->revocation_note = note.size() > kMaxNoteChars ? note.substr(0, kMaxNoteChars) : note;
        d->consent_granted = false;
        d->capture_enabled = false;
        d->online = false;
        d->paired = false;
    }
    // An id REMOVED from the list is no longer revoked by the list. Without this,
    // un-revoking was impossible for the life of the process: the flag was only
    // ever set, never cleared, so a device the operator had reinstated stayed
    // refused until the service restarted.
    for (Device& d : devices_) {
        if (!d.revoked_by_file) continue;
        bool still_listed = false;
        for (const auto& item : parsed) {
            const std::string listed = item.is_string() ? item.get<std::string>()
                                        : item.is_object() ? item.value("device_id", std::string())
                                                           : std::string();
            if (listed == d.device_id) { still_listed = true; break; }
        }
        if (still_listed) continue;
        d.revoked_by_file = false;
        d.revoked = false;
        d.revocation_note.clear();
        // Pairing, consent and capture are NOT restored automatically: the
        // operator reinstates those explicitly. Un-revoking lifts the ban, it does
        // not re-grant permissions.
    }
    if (reason != nullptr) reason->clear();
    return true;
}

// Re-read the operator's AUTHORIZATION decisions without touching anything the hub
// observed for itself. See the header for why those are different kinds of fact.
DeviceRegistry::RefreshOutcome DeviceRegistry::refresh_authorization(
    const std::string& path, std::vector<std::string>* transitions, std::string* reason) {
    auto fail = [reason](RefreshOutcome outcome, const std::string& why) {
        if (reason != nullptr) *reason = why;
        return outcome;
    };
    auto note_change = [transitions](const std::string& text) {
        // Bounded, non-sensitive: a device id and a field transition. No keys, no
        // credentials, no consent subject text.
        if (transitions != nullptr && transitions->size() < 64) transitions->push_back(text);
    };

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(RefreshOutcome::absent, "authorization file is no longer readable");
    nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded()) {
        return fail(RefreshOutcome::malformed, "authorization file is not valid JSON");
    }
    if (!parsed.is_object() || !parsed.contains("devices") || !parsed["devices"].is_array()) {
        return fail(RefreshOutcome::malformed, "authorization file has no devices array");
    }
    if (parsed["devices"].size() > cfg_.max_devices) {
        return fail(RefreshOutcome::unbounded, "authorization file exceeds the registry bound");
    }

    for (const auto& entry : parsed["devices"]) {
        if (!entry.is_object()) return fail(RefreshOutcome::malformed, "device entry is not an object");
        const std::string id = entry.value("device_id", std::string());
        if (id.empty() || id.size() > kMaxIdChars) {
            return fail(RefreshOutcome::malformed, "device entry has no usable device_id");
        }
        Device* device = find_mutable(id);
        if (device == nullptr) {
            // A device that APPEARED while the service was running: pairing a new phone
            // must not require restarting the whole stack.
            if (devices_.size() >= cfg_.max_devices) {
                return fail(RefreshOutcome::unbounded, "the registry is at capacity");
            }
            Device fresh;
            fresh.device_id = id;
            devices_.push_back(std::move(fresh));
            device = find_mutable(id);
            if (device == nullptr) return fail(RefreshOutcome::malformed, "could not register the device");
            note_change(id + ": registered");
        }

        const std::string credential = entry.value("credential_id", std::string());
        if (credential.empty() || credential.size() > kMaxIdChars) {
            return fail(RefreshOutcome::malformed, std::string("credential_id invalid for ") + id);
        }
        if (credential != device->credential_id) {
            // A changed fingerprint is a ROTATION: the old certificate stops matching
            // this record and is refused from this moment.
            device->credential_id = credential;
            note_change(id + ": credential rebound (rotation)");
        }

        const bool paired = entry.value("paired", false);
        const bool revoked = entry.value("revoked", false);
        const bool consent = entry.value("consent_granted", false) && !revoked;
        const bool capture = entry.value("capture_enabled", false) && paired && consent && !revoked;

        if (revoked != device->revoked) {
            device->revoked = revoked;
            note_change(id + (revoked ? ": revoked" : ": revocation lifted"));
        }
        if (paired != device->paired) {
            device->paired = paired;
            note_change(id + (paired ? ": paired" : ": unpaired"));
        }
        if (consent != device->consent_granted) {
            device->consent_granted = consent;
            note_change(id + (consent ? ": consent granted" : ": consent withdrawn"));
        }
        if (consent) {
            const std::string subject = entry.value("consent_subject", std::string());
            const std::string purpose = entry.value("consent_purpose", std::string());
            if (subject.size() <= kMaxIdChars) device->consent_subject = subject;
            if (purpose.size() <= kMaxIdChars) device->consent_purpose = purpose;
        }
        if (capture != device->capture_enabled) {
            device->capture_enabled = capture;
            note_change(id + (capture ? ": capture enabled" : ": capture disabled"));
        }
        if (!consent || revoked) {
            // Belt and braces: withdrawal and revocation both stop capture here, even
            // if the file still claims capture_enabled.
            if (device->capture_enabled) {
                device->capture_enabled = false;
                note_change(id + ": capture disabled (consent/revocation)");
            }
            if (revoked) device->online = false;
        }
    }
    if (reason != nullptr) reason->clear();
    return RefreshOutcome::refreshed;
}

bool DeviceRegistry::seed_sequence_watermark(const std::string& device_id, uint64_t last_sequence) {
    Device* device = find_mutable(device_id);
    if (device == nullptr) return false;
    // Never lower a watermark: durable state may only raise it.
    if (last_sequence > device->last_sequence) device->last_sequence = last_sequence;
    return true;
}

DeviceRegistry::AuthResult DeviceRegistry::authenticate(const std::string& device_id,
                                                        const std::string& credential_id) const {
    if (credential_id.empty()) return AuthResult::no_credential;
    if (device_id.empty()) return AuthResult::unknown_device;
    const Device* d = find(device_id);
    if (d == nullptr) return AuthResult::unknown_device;
    if (d->revoked) return AuthResult::revoked;
    if (!d->paired) return AuthResult::unpaired;
    if (d->credential_id.empty()) return AuthResult::no_credential;
    if (d->credential_id != credential_id) return AuthResult::credential_mismatch;
    return AuthResult::ok;
}

bool DeviceRegistry::set_consent(const std::string& device_id, bool granted,
                                 const std::string& subject, const std::string& purpose,
                                 int64_t now_ms) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return false;
    if (d->revoked) return false;  // a revoked device cannot be re-consented silently
    if (granted) {
        if (subject.empty() || purpose.empty()) return false;
        if (!bounded(subject, kMaxIdChars) || !bounded(purpose, kMaxTypeChars)) return false;
    }
    d->consent_granted = granted;
    if (granted) {
        d->consent_subject = subject;
        d->consent_purpose = purpose;
    }
    // A WITHDRAWAL keeps the subject/purpose it is withdrawing: the difference
    // between "never consented" (empty subject -> consent_missing) and "consent
    // withdrawn by X for purpose Y" (consent_withdrawn) is exactly what an audit
    // needs, and clearing it would erase that distinction.
    d->consent_updated_ms = now_ms;
    if (!granted) d->capture_enabled = false;  // withdrawal also stops capture
    return true;
}

bool DeviceRegistry::set_capture_enabled(const std::string& device_id, bool enabled) {
    Device* d = find_mutable(device_id);
    if (d == nullptr || d->revoked) return false;
    if (enabled && (!d->paired || !d->consent_granted)) return false;  // no capture without consent
    d->capture_enabled = enabled;
    return true;
}

bool DeviceRegistry::sequence_is_new(const std::string& device_id, uint64_t sequence) const {
    const Device* d = find(device_id);
    if (d == nullptr) return false;
    return sequence > d->last_sequence;
}

bool DeviceRegistry::commit_sequence(const std::string& device_id, uint64_t sequence) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return false;
    if (sequence <= d->last_sequence) return false;
    d->last_sequence = sequence;
    return true;
}

bool DeviceRegistry::note_observation_timestamp(const std::string& device_id, int64_t acquired_at_ms) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return false;
    if (acquired_at_ms > d->last_observation_at_ms) d->last_observation_at_ms = acquired_at_ms;
    return true;
}

void DeviceRegistry::note_heartbeat(const std::string& device_id, int64_t now_ms,
                                    const std::vector<std::string>& capabilities) {
    Device* d = find_mutable(device_id);
    if (d == nullptr) return;
    if (!d->paired || d->revoked || d->credential_id.empty()) {
        d->online = false;
        return;
    }
    d->last_heartbeat_ms = now_ms;
    d->online = true;
    if (!capabilities.empty() && capabilities.size() <= cfg_.max_capabilities) {
        d->capabilities = capabilities;  // the device reports what it can do
        d->has_camera = false;
        d->has_microphone = false;
        for (const std::string& cap : capabilities) {
            if (cap == "camera") d->has_camera = true;
            if (cap == "microphone") d->has_microphone = true;
        }
    }
}

size_t DeviceRegistry::sweep_offline(int64_t now_ms) {
    size_t changed = 0;
    for (Device& d : devices_) {
        if (!d.online) continue;
        if (d.last_heartbeat_ms > now_ms + 5000 || (now_ms - d.last_heartbeat_ms >= cfg_.online_timeout_ms)) {
            d.online = false;
            ++changed;
        }
    }
    return changed;
}

size_t DeviceRegistry::online_count() const {
    size_t n = 0;
    for (const Device& d : devices_) {
        if (d.online) ++n;
    }
    return n;
}

size_t DeviceRegistry::revoked_count() const {
    size_t n = 0;
    for (const Device& d : devices_) {
        if (d.revoked) ++n;
    }
    return n;
}

size_t DeviceRegistry::paired_count() const {
    size_t n = 0;
    for (const Device& d : devices_) {
        if (d.paired) ++n;
    }
    return n;
}

}  // namespace homeguardian::net
