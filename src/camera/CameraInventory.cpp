#include "CameraInventory.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace homeguardian::camera {

namespace {

constexpr size_t kMaxLabelChars = 64;
constexpr size_t kMaxAddressChars = 45;
constexpr size_t kMaxFirmwareChars = 96;
constexpr size_t kMaxSiteChars = 64;
constexpr const char* kHeader = "hg-camera-inventory 1";

// Tokens are space-separated on disk, so a token may not contain whitespace. Validating at
// the edge keeps the file format unambiguous without inventing an escaping scheme.
bool printable_token(const std::string& s, size_t max_chars) {
    if (s.empty() || s.size() > max_chars) return false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F) return false;
    }
    return true;
}

InterfaceClass interface_from_token(const std::string& t, bool* ok) {
    *ok = true;
    if (t == "unknown") return InterfaceClass::unknown;
    if (t == "vendor_protocol_only") return InterfaceClass::vendor_protocol_only;
    if (t == "standard_media_answering") return InterfaceClass::standard_media_answering;
    if (t == "no_responders") return InterfaceClass::no_responders;
    *ok = false;
    return InterfaceClass::unknown;
}

MediaStatus media_from_token(const std::string& t, bool* ok) {
    *ok = true;
    if (t == "unknown") return MediaStatus::unknown;
    if (t == "no_interface_observed") return MediaStatus::no_interface_observed;
    if (t == "vendor_protocol_only") return MediaStatus::vendor_protocol_only;
    if (t == "standard_media_unauthenticated") return MediaStatus::standard_media_unauthenticated;
    if (t == "standard_media_requires_auth") return MediaStatus::standard_media_requires_auth;
    *ok = false;
    return MediaStatus::unknown;
}

HealthState health_from_token(const std::string& t, bool* ok) {
    *ok = true;
    if (t == "unknown") return HealthState::unknown;
    if (t == "online") return HealthState::online;
    if (t == "offline") return HealthState::offline;
    *ok = false;
    return HealthState::unknown;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool parse_u64(const std::string& t, uint64_t* out) {
    if (t.empty() || t.size() > 20) return false;
    uint64_t v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    *out = v;
    return true;
}

}  // namespace

PassReport CameraInventory::reconcile_pass(const std::vector<CameraObservation>& pass,
                                           int64_t now_ms) {
    PassReport report;
    report.outcomes.assign(pass.size(), ObservationOutcome::unusable_fingerprint);

    // --- the two conflicts that are properties of the PASS, not of one observation ---
    std::map<std::string, std::set<std::string>> addresses_by_fingerprint;
    std::map<std::string, std::set<std::string>> fingerprints_by_address;
    for (const auto& obs : pass) {
        if (!obs.fingerprint.durable()) continue;
        const std::string fp = obs.fingerprint.canonical();
        if (!obs.address.empty()) {
            addresses_by_fingerprint[fp].insert(obs.address);
            fingerprints_by_address[obs.address].insert(fp);
        }
    }
    for (const auto& kv : addresses_by_fingerprint) {
        if (kv.second.size() > 1) {
            report.conflicts.push_back("one NIC fingerprint reported at " +
                                       std::to_string(kv.second.size()) +
                                       " addresses in a single pass: it is not possible to say "
                                       "which observation is current");
        }
    }
    for (const auto& kv : fingerprints_by_address) {
        if (kv.second.size() > 1) {
            report.conflicts.push_back("one address is claimed by " +
                                       std::to_string(kv.second.size()) +
                                       " different NIC fingerprints in a single pass");
        }
    }
    report.consistent = report.conflicts.empty();

    if (!report.consistent) {
        // A conflicting pass is not evidence. Nothing is applied, so no record can be
        // moved onto an address we cannot confirm.
        for (size_t i = 0; i < pass.size(); ++i) {
            if (!pass[i].fingerprint.durable()) {
                report.outcomes[i] = ObservationOutcome::unusable_fingerprint;
            } else if (addresses_by_fingerprint[pass[i].fingerprint.canonical()].size() > 1) {
                report.outcomes[i] = ObservationOutcome::conflicting_fingerprint;
            } else {
                report.outcomes[i] = ObservationOutcome::conflicting_address;
            }
        }
        report.summary = "conflicting pass: " + std::to_string(report.conflicts.size()) +
                         " conflict(s); nothing applied";
        return report;
    }

    for (size_t i = 0; i < pass.size(); ++i) {
        const CameraObservation& obs = pass[i];
        if (!obs.fingerprint.durable()) {
            report.outcomes[i] = ObservationOutcome::unusable_fingerprint;
            ++report.unusable;
            continue;
        }
        if (CameraRecord* known = find_mutable_by_fingerprint(obs.fingerprint)) {
            const bool moved = !obs.address.empty() && !known->last_address.empty() &&
                               obs.address != known->last_address;
            apply_known(*known, obs, now_ms);
            report.outcomes[i] =
                moved ? ObservationOutcome::known_moved : ObservationOutcome::known_at_same_address;
            if (moved) ++report.known_moved; else ++report.known_same_address;
            continue;
        }
        note_candidate(obs, now_ms);
        report.outcomes[i] = ObservationOutcome::new_distinct_device;
        ++report.new_devices;
    }

    report.summary = "consistent pass: " + std::to_string(report.known_same_address) +
                     " known at the recorded address, " + std::to_string(report.known_moved) +
                     " known but moved, " + std::to_string(report.new_devices) +
                     " distinct and unregistered, " + std::to_string(report.unusable) +
                     " unusable fingerprint(s)";
    return report;
}

ObservationOutcome CameraInventory::observe(const CameraObservation& obs, int64_t now_ms,
                                            std::string* reason) {
    // A single observation cannot see an intra-pass conflict by construction; use
    // reconcile_pass for a whole pass.
    if (!obs.fingerprint.valid()) {
        if (reason) *reason = "no usable hardware fingerprint";
        return ObservationOutcome::unusable_fingerprint;
    }
    if (!obs.fingerprint.durable()) {
        if (reason) {
            *reason = "fingerprint is locally administered (randomized), so the same device "
                      "may present a different one: it cannot ground a durable identity";
        }
        return ObservationOutcome::unusable_fingerprint;
    }

    if (CameraRecord* known = find_mutable_by_fingerprint(obs.fingerprint)) {
        const bool moved = !obs.address.empty() && !known->last_address.empty() &&
                           obs.address != known->last_address;
        apply_known(*known, obs, now_ms);
        if (reason) reason->clear();
        return moved ? ObservationOutcome::known_moved : ObservationOutcome::known_at_same_address;
    }

    note_candidate(obs, now_ms);
    if (reason) {
        *reason = "a distinct device was observed but is not registered: registration needs a "
                  "stable owner-provided identity and an explicit authorization";
    }
    return ObservationOutcome::new_distinct_device;
}

bool CameraInventory::onboard(const CameraObservation& obs, const std::string& site_id,
                              const std::string& owner_provided_identity, bool owner_authorized,
                              int64_t now_ms, std::string* reason) {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };

    if (!obs.fingerprint.valid()) return fail("no usable hardware fingerprint");
    if (!obs.fingerprint.durable()) {
        return fail("a randomized (locally administered) MAC cannot ground a durable device "
                    "identity: the same device may present a different one next time");
    }
    if (!printable_token(site_id, kMaxSiteChars)) return fail("site id is empty, too long, or contains whitespace");
    if (!printable_token(owner_provided_identity, kMaxLabelChars)) {
        return fail("no stable owner-provided device identity: a device cannot be registered on "
                    "an observed address alone");
    }
    if (LogicalDeviceId::is_ipv4_literal(owner_provided_identity)) {
        return fail("the owner-provided identity is an IP address: an address is assigned and "
                    "recycled, so it cannot identify a camera");
    }
    if (!owner_authorized) {
        return fail("this device has not been explicitly authorized by the owner");
    }
    if (const CameraRecord* existing = find_by_fingerprint(obs.fingerprint)) {
        return fail("already registered as " + existing->id.canonical());
    }
    if (records_.size() >= cfg_.max_cameras) return fail("camera inventory is full");

    // An address that is already attributed to a DIFFERENT fingerprint is not this device's.
    for (const auto& r : records_) {
        if (!r.last_address.empty() && !obs.address.empty() && r.last_address == obs.address &&
            !(r.fingerprint == obs.fingerprint)) {
            return fail("the observed address is already attributed to another registered device");
        }
    }

    // Derive the logical device id from the OWNER's identity, not from the address: this is
    // what keeps it stable across a DHCP change, a NAT boundary or a site move.
    std::string segment = "cam.v380.";
    for (char c : owner_provided_identity) {
        if (c >= 'A' && c <= 'Z') segment.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            segment.push_back(c);
        else segment.push_back('-');
    }
    LogicalDeviceId id{site_id, segment};
    if (!id.valid()) return fail("the derived logical device id is not a valid identity");

    CameraRecord record;
    record.id = id;
    record.owner_identity_label = owner_provided_identity;
    record.fingerprint = obs.fingerprint;
    record.site_id = site_id;
    record.authorized = true;
    record.first_seen_ms = now_ms;
    record.last_seen_ms = now_ms;
    record.last_address = obs.address;
    record.address_changes = 0;
    record.interface_class = obs.interface_class;
    record.health = HealthState::online;
    // `media` and `firmware_model` stay at their unknowns: neither is inferable from a
    // read-only pass, and an inferred value here would become a false claim.
    records_.push_back(record);

    // The candidate is now a registered device.
    candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(),
                                     [&](const CandidateRecord& c) {
                                         return c.fingerprint == obs.fingerprint;
                                     }),
                      candidates_.end());
    if (reason != nullptr) reason->clear();
    return true;
}

bool CameraInventory::note_firmware(const LogicalDeviceId& id, const std::string& model,
                                    bool verified, std::string* reason) {
    CameraRecord* r = find_mutable(id);
    if (r == nullptr) {
        if (reason) *reason = "no such device";
        return false;
    }
    if (verified) {
        if (!printable_token(model, kMaxFirmwareChars)) {
            if (reason) *reason = "unverified-or-unprintable firmware string";
            return false;
        }
        r->firmware_model = model;     // only ever a string that was actually read out
    } else {
        r->firmware_model.clear();     // an unverified report is NOT a firmware version
    }
    r->firmware_verified = verified;
    if (reason) reason->clear();
    return true;
}

bool CameraInventory::set_media_status(const LogicalDeviceId& id, MediaStatus status,
                                       std::string* reason) {
    CameraRecord* r = find_mutable(id);
    if (r == nullptr) {
        if (reason) *reason = "no such device";
        return false;
    }
    r->media = status;
    if (reason) reason->clear();
    return true;
}

size_t CameraInventory::sweep_offline(int64_t now_ms) {
    size_t changed = 0;
    for (auto& r : records_) {
        if (r.health != HealthState::online) continue;
        if (now_ms - r.last_seen_ms > cfg_.offline_after_ms) {
            r.health = HealthState::offline;
            ++changed;
        }
    }
    return changed;
}

const CameraRecord* CameraInventory::find(const LogicalDeviceId& id) const {
    for (const auto& r : records_) if (r.id.canonical() == id.canonical()) return &r;
    return nullptr;
}

const CameraRecord* CameraInventory::find_by_fingerprint(const HardwareFingerprint& fp) const {
    for (const auto& r : records_) if (r.fingerprint == fp) return &r;
    return nullptr;
}

CameraRecord* CameraInventory::find_mutable(const LogicalDeviceId& id) {
    for (auto& r : records_) if (r.id.canonical() == id.canonical()) return &r;
    return nullptr;
}

CameraRecord* CameraInventory::find_mutable_by_fingerprint(const HardwareFingerprint& fp) {
    for (auto& r : records_) if (r.fingerprint == fp) return &r;
    return nullptr;
}

CandidateRecord* CameraInventory::find_candidate(const HardwareFingerprint& fp) {
    for (auto& c : candidates_) if (c.fingerprint == fp) return &c;
    return nullptr;
}

CandidateRecord& CameraInventory::note_candidate(const CameraObservation& obs, int64_t now_ms) {
    if (CandidateRecord* existing = find_candidate(obs.fingerprint)) {
        existing->last_seen_ms = now_ms;
        if (!obs.address.empty()) existing->last_address = obs.address;
        existing->interface_class = obs.interface_class;
        ++existing->sightings;
        return *existing;
    }
    CandidateRecord c;
    c.fingerprint = obs.fingerprint;
    c.last_address = obs.address;
    c.first_seen_ms = now_ms;
    c.last_seen_ms = now_ms;
    c.interface_class = obs.interface_class;
    c.sightings = 1;
    candidates_.push_back(c);
    if (candidates_.size() > cfg_.max_candidates) {
        // Bounded: an unbounded candidate list is a memory leak with a nicer name.
        candidates_.erase(candidates_.begin());
    }
    return candidates_.back();
}

bool CameraInventory::apply_known(CameraRecord& record, const CameraObservation& obs,
                                  int64_t now_ms) {
    record.last_seen_ms = now_ms;
    record.health = HealthState::online;
    if (!obs.address.empty() && obs.address != record.last_address) {
        if (!record.last_address.empty()) {
            record.address_history.push_back(record.last_address);
            if (record.address_history.size() > cfg_.max_address_history) {
                record.address_history.erase(record.address_history.begin());
            }
            ++record.address_changes;
        }
        record.last_address = obs.address;
    }
    // The interface class is an observation and is recorded as one. The media STATUS is not
    // touched here: it is a readiness claim that only `set_media_status` (evidence in hand)
    // may raise, so a routine scan can never upgrade a camera's readiness.
    record.interface_class = obs.interface_class;
    return true;
}

// ---------------------------------------------------------------- persistence

bool CameraInventory::save(const std::string& path, std::string* reason) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (reason) *reason = "cannot open for writing: " + path;
        return false;
    }
    out << kHeader << "\n";
    for (const auto& r : records_) {
        std::string history;
        for (size_t i = 0; i < r.address_history.size(); ++i) {
            if (i != 0) history.push_back(';');
            history += r.address_history[i];
        }
        out << "camera " << r.id.site_id << ' ' << r.id.device_id << ' '
            << r.fingerprint.canonical() << ' '
            << (r.owner_identity_label.empty() ? "-" : r.owner_identity_label) << ' '
            << (r.authorized ? 1 : 0) << ' ' << r.first_seen_ms << ' ' << r.last_seen_ms << ' '
            << (r.last_address.empty() ? "-" : r.last_address) << ' ' << r.address_changes << ' '
            << interface_class_to_string(r.interface_class) << ' '
            << media_status_to_string(r.media) << ' '
            << (r.firmware_model.empty() ? "-" : r.firmware_model) << ' '
            << (r.firmware_verified ? 1 : 0) << ' ' << health_state_to_string(r.health) << ' '
            << (history.empty() ? "-" : history) << "\n";
    }
    for (const auto& c : candidates_) {
        out << "candidate " << c.fingerprint.canonical() << ' '
            << (c.last_address.empty() ? "-" : c.last_address) << ' ' << c.first_seen_ms << ' '
            << c.last_seen_ms << ' ' << interface_class_to_string(c.interface_class) << ' '
            << c.sightings << "\n";
    }
    out.flush();
    if (!out) {
        if (reason) *reason = "write failed";
        return false;
    }
    if (reason) reason->clear();
    return true;
}

bool CameraInventory::load(const std::string& path, std::string* reason) {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    std::ifstream in(path);
    if (!in) return fail("cannot open for reading: " + path);

    std::vector<CameraRecord> records;
    std::vector<CandidateRecord> candidates;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) return fail("missing or mismatched header");

    size_t line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;

        if (kind == "camera") {
            std::string site, device, fp, label, auth, first, last, addr, changes, iface, media,
                fw, fwv, health, history;
            if (!(ss >> site >> device >> fp >> label >> auth >> first >> last >> addr >>
                  changes >> iface >> media >> fw >> fwv >> health >> history)) {
                return fail("malformed camera record at line " + std::to_string(line_no));
            }
            CameraRecord r;
            r.id = LogicalDeviceId{site, device};
            if (!r.id.valid()) return fail("invalid device id at line " + std::to_string(line_no));
            if (!HardwareFingerprint::normalize(fp, &r.fingerprint, nullptr)) {
                return fail("invalid fingerprint at line " + std::to_string(line_no));
            }
            r.owner_identity_label = (label == "-") ? std::string() : label;
            uint64_t v = 0;
            if (!parse_u64(auth, &v) || v > 1) return fail("bad authorization flag");
            r.authorized = (v == 1);
            if (!parse_u64(first, &v)) return fail("bad first_seen");
            r.first_seen_ms = static_cast<int64_t>(v);
            if (!parse_u64(last, &v)) return fail("bad last_seen");
            r.last_seen_ms = static_cast<int64_t>(v);
            r.last_address = (addr == "-") ? std::string() : addr;
            if (!parse_u64(changes, &v)) return fail("bad address_changes");
            r.address_changes = static_cast<uint32_t>(v);
            bool ok = false;
            r.interface_class = interface_from_token(iface, &ok);
            if (!ok) return fail("unknown interface class");
            r.media = media_from_token(media, &ok);
            if (!ok) return fail("unknown media status");
            r.firmware_model = (fw == "-") ? std::string() : fw;
            if (!parse_u64(fwv, &v) || v > 1) return fail("bad firmware_verified");
            r.firmware_verified = (v == 1);
            if (!r.firmware_verified) r.firmware_model.clear();
            r.health = health_from_token(health, &ok);
            if (!ok) return fail("unknown health state");
            if (history != "-") {
                for (const auto& a : split(history, ';')) {
                    if (!a.empty()) r.address_history.push_back(a);
                }
            }
            records.push_back(r);
        } else if (kind == "candidate") {
            std::string fp, addr, first, last, iface, sightings;
            if (!(ss >> fp >> addr >> first >> last >> iface >> sightings)) {
                return fail("malformed candidate record at line " + std::to_string(line_no));
            }
            CandidateRecord c;
            if (!HardwareFingerprint::normalize(fp, &c.fingerprint, nullptr)) {
                return fail("invalid candidate fingerprint at line " + std::to_string(line_no));
            }
            c.last_address = (addr == "-") ? std::string() : addr;
            uint64_t v = 0;
            if (!parse_u64(first, &v)) return fail("bad candidate first_seen");
            c.first_seen_ms = static_cast<int64_t>(v);
            if (!parse_u64(last, &v)) return fail("bad candidate last_seen");
            c.last_seen_ms = static_cast<int64_t>(v);
            bool ok = false;
            c.interface_class = interface_from_token(iface, &ok);
            if (!ok) return fail("unknown candidate interface class");
            if (!parse_u64(sightings, &v)) return fail("bad candidate sightings");
            c.sightings = static_cast<uint32_t>(v);
            candidates.push_back(c);
        } else {
            return fail("unknown record kind at line " + std::to_string(line_no));
        }
    }

    if (records.size() > cfg_.max_cameras) return fail("camera records exceed the configured bound");
    if (candidates.size() > cfg_.max_candidates) return fail("candidate records exceed the bound");

    records_ = std::move(records);
    candidates_ = std::move(candidates);
    if (reason) reason->clear();
    return true;
}

}  // namespace homeguardian::camera
