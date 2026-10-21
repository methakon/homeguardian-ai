#include "SecurityObservation.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace homeguardian {

namespace {

// Millisecond epoch <-> JSON. Timestamps are carried as epoch milliseconds so a
// record is unambiguous across systems (no local-time ambiguity).
long long to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_ms(long long ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

bool state_in(const std::string& state, std::initializer_list<const char*> values) {
    for (const char* v : values) {
        if (state == v) return true;
    }
    return false;
}

}  // namespace

std::optional<SecuritySourceType> security_source_type_from_string(const std::string& value) {
    static const SecuritySourceType all[] = {
        SecuritySourceType::motion, SecuritySourceType::contact, SecuritySourceType::presence,
        SecuritySourceType::camera_availability, SecuritySourceType::microphone_availability,
        SecuritySourceType::device_availability, SecuritySourceType::tamper,
        SecuritySourceType::environmental_threshold};
    for (SecuritySourceType t : all) {
        if (value == security_source_type_to_string(t)) return t;
    }
    return std::nullopt;
}

void SecurityObservation::validate() const {
    if (device_id.empty()) throw std::invalid_argument("device_id cannot be empty");
    if (device_id.size() > kMaxDeviceIdChars) throw std::invalid_argument("device_id too long");
    if (source.empty()) throw std::invalid_argument("source cannot be empty");
    if (source.size() > kMaxSourceChars) throw std::invalid_argument("source too long");
    if (state.size() > kMaxStateChars) throw std::invalid_argument("state too long");
    if (note.size() > kMaxNoteChars) throw std::invalid_argument("note too long");
    if (consent_subject.size() > kMaxDeviceIdChars) throw std::invalid_argument("consent_subject too long");
    if (consent_purpose.size() > kMaxStateChars) throw std::invalid_argument("consent_purpose too long");
    if (acquired_at.time_since_epoch().count() == 0) {
        throw std::invalid_argument("acquired_at is not set");
    }
    if (ingested_at.time_since_epoch().count() == 0) {
        throw std::invalid_argument("ingested_at is not set");
    }
    // An observation must be acquired before (or as) it is ingested. A source
    // cannot report the future.
    if (acquired_at > ingested_at + std::chrono::milliseconds(50)) {
        throw std::invalid_argument("acquired_at is after ingested_at");
    }
    if (confidence.has_value() && (confidence.value() < 0.0 || confidence.value() > 1.0)) {
        throw std::invalid_argument("confidence must be in [0,1]");
    }
    if (quality.has_value() && (quality.value() < 0.0 || quality.value() > 1.0)) {
        throw std::invalid_argument("quality must be in [0,1]");
    }
    if (!features.is_object()) throw std::invalid_argument("features must be a JSON object");
    if (features.size() > kMaxFeatureKeys) throw std::invalid_argument("too many feature keys");
    if (features.dump().size() > kMaxFeatureJsonBytes) {
        throw std::invalid_argument("features payload too large");
    }
}

DetectorStatus security_status_for(SecuritySourceType type, const std::string& state) {
    switch (type) {
        case SecuritySourceType::motion:
            if (state_in(state, {"on", "detected", "active"})) return DetectorStatus::detected;
            if (state_in(state, {"off", "clear", "idle"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
        case SecuritySourceType::contact:
            if (state_in(state, {"open", "opened"})) return DetectorStatus::detected;
            if (state_in(state, {"closed", "shut"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
        case SecuritySourceType::presence:
            if (state_in(state, {"on", "present", "occupied"})) return DetectorStatus::detected;
            if (state_in(state, {"off", "absent", "clear"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
        case SecuritySourceType::camera_availability:
        case SecuritySourceType::microphone_availability:
        case SecuritySourceType::device_availability:
            // For availability the DETECTED target is the problem: an unavailable
            // sensor is a finding, and a healthy one is not.
            if (state_in(state, {"offline", "unavailable", "error", "unknown"})) {
                return DetectorStatus::detected;
            }
            if (state_in(state, {"online", "available", "ok"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
        case SecuritySourceType::tamper:
            if (state_in(state, {"tamper", "detected", "unexpected_disconnect"})) {
                return DetectorStatus::detected;
            }
            if (state_in(state, {"clear", "ok"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
        case SecuritySourceType::environmental_threshold:
            if (state_in(state, {"crossed", "above", "below"})) return DetectorStatus::detected;
            if (state_in(state, {"normal", "within"})) return DetectorStatus::not_detected;
            return DetectorStatus::inconclusive;
    }
    return DetectorStatus::inconclusive;
}

Observation SecurityObservation::to_observation() const {
    validate();

    Observation o;
    // Deterministic, non-random id: device + type + acquisition time. Two
    // identical reports from one device therefore share an id, which is what a
    // downstream de-duplicator wants.
    o.observation_id = "sec-" + device_id + "-" + security_source_type_to_string(source_type) + "-" +
                       std::to_string(to_ms(acquired_at));
    o.source = device_id;
    o.provenance = provenance;
    o.detector_id = std::string("sensor.") + security_source_type_to_string(source_type);
    o.model_id = "device-report-v1";
    o.observation_timestamp = acquired_at;
    o.status = authorized ? security_status_for(source_type, state) : DetectorStatus::unavailable;
    o.confidence = confidence;
    o.quality = quality;

    o.features = features.is_object() ? features : nlohmann::json::object();
    o.features["source_type"] = security_source_type_to_string(source_type);
    o.features["state"] = state;
    o.features["producer"] = source;
    o.features["acquired_at_ms"] = to_ms(acquired_at);
    o.features["ingested_at_ms"] = to_ms(ingested_at);
    o.features["authorized"] = authorized;
    if (!consent_subject.empty()) o.features["consent_subject"] = consent_subject;
    if (!consent_purpose.empty()) o.features["consent_purpose"] = consent_purpose;

    std::string explanation = note;
    if (!authorized) {
        explanation = explanation.empty() ? std::string("not authorized; the report is recorded but "
                                                        "claims no detection")
                                         : explanation;
    }
    o.note = explanation.size() > Observation::kMaxNoteChars
                 ? explanation.substr(0, Observation::kMaxNoteChars)
                 : explanation;
    return o;
}

Severity SecurityObservation::suggested_severity() const {
    // Conservative by construction. Critically: this function has no branch that
    // returns Severity::critical, so no sensor report can raise a critical alert.
    if (!authorized) return Severity::info;
    switch (source_type) {
        case SecuritySourceType::tamper:
            return Severity::warning;  // an unexpected disconnect/tamper is credible
        case SecuritySourceType::device_availability:
            return security_status_for(source_type, state) == DetectorStatus::detected
                       ? Severity::warning
                       : Severity::info;
        case SecuritySourceType::camera_availability:
        case SecuritySourceType::microphone_availability:
            return security_status_for(source_type, state) == DetectorStatus::detected
                       ? Severity::warning
                       : Severity::info;
        case SecuritySourceType::contact:
        case SecuritySourceType::motion:
        case SecuritySourceType::presence:
        case SecuritySourceType::environmental_threshold:
            // A single one of these is advisory. Escalation requires correlation
            // with other evidence, which the existing AlertPolicy owns.
            return Severity::info;
    }
    return Severity::info;
}

std::optional<SecurityObservation> SecurityObservation::from_json(const nlohmann::json& value,
                                                                  std::string* reason) {
    auto fail = [reason](const std::string& why) -> std::optional<SecurityObservation> {
        if (reason != nullptr) *reason = why;
        return std::nullopt;
    };
    if (!value.is_object()) return fail("payload is not an object");

    SecurityObservation o;
    if (!value.contains("device_id") || !value["device_id"].is_string()) {
        return fail("device_id missing or not a string");
    }
    o.device_id = value["device_id"].get<std::string>();

    o.source = value.value("source", std::string("unknown"));
    if (!value.contains("source_type") || !value["source_type"].is_string()) {
        return fail("source_type missing or not a string");
    }
    const auto type = security_source_type_from_string(value["source_type"].get<std::string>());
    if (!type.has_value()) return fail("unknown source_type");
    o.source_type = type.value();

    if (!value.contains("acquired_at_ms") || !value["acquired_at_ms"].is_number_integer()) {
        return fail("acquired_at_ms missing or not an integer");
    }
    o.acquired_at = from_ms(value["acquired_at_ms"].get<long long>());
    if (value.contains("ingested_at_ms")) {
        if (!value["ingested_at_ms"].is_number_integer()) {
            return fail("ingested_at_ms is not an integer");
        }
        o.ingested_at = from_ms(value["ingested_at_ms"].get<long long>());
    } else {
        o.ingested_at = std::chrono::system_clock::now();
    }

    const std::string provenance = value.value("provenance", std::string("synthetic"));
    if (provenance != "synthetic" && provenance != "real") return fail("unknown provenance");
    o.provenance = provenance == "real" ? Provenance::real : Provenance::synthetic;

    o.state = value.value("state", std::string());
    o.consent_subject = value.value("consent_subject", std::string());
    o.consent_purpose = value.value("consent_purpose", std::string());
    o.authorized = value.value("authorized", false);
    o.note = value.value("note", std::string());

    if (value.contains("confidence")) {
        if (!value["confidence"].is_number()) return fail("confidence is not a number");
        o.confidence = value["confidence"].get<double>();
    }
    if (value.contains("quality")) {
        if (!value["quality"].is_number()) return fail("quality is not a number");
        o.quality = value["quality"].get<double>();
    }
    if (value.contains("features")) {
        if (!value["features"].is_object()) return fail("features is not an object");
        o.features = value["features"];
    }

    try {
        o.validate();
    } catch (const std::exception& e) {
        return fail(std::string("validation failed: ") + e.what());
    }
    if (reason != nullptr) reason->clear();
    return o;
}

nlohmann::json SecurityObservation::to_json(const SecurityObservation& observation) {
    observation.validate();
    nlohmann::json j;
    j["device_id"] = observation.device_id;
    j["source"] = observation.source;
    j["source_type"] = security_source_type_to_string(observation.source_type);
    j["acquired_at_ms"] = to_ms(observation.acquired_at);
    j["ingested_at_ms"] = to_ms(observation.ingested_at);
    j["provenance"] = provenance_to_string(observation.provenance);
    if (observation.confidence.has_value()) j["confidence"] = observation.confidence.value();
    if (observation.quality.has_value()) j["quality"] = observation.quality.value();
    j["state"] = observation.state;
    if (!observation.consent_subject.empty()) j["consent_subject"] = observation.consent_subject;
    if (!observation.consent_purpose.empty()) j["consent_purpose"] = observation.consent_purpose;
    j["authorized"] = observation.authorized;
    j["features"] = observation.features;
    if (!observation.note.empty()) j["note"] = observation.note;
    return j;
}

// ---------------------------------------------------------------------------
// DeviceAvailabilityTracker
// ---------------------------------------------------------------------------

DeviceAvailabilityTracker::Entry* DeviceAvailabilityTracker::find(const std::string& device_id) {
    for (Entry& e : devices_) {
        if (e.device_id == device_id) return &e;
    }
    return nullptr;
}

std::optional<DeviceAvailabilityTracker::Transition> DeviceAvailabilityTracker::mark_seen(
    const std::string& device_id, std::chrono::system_clock::time_point now) {
    if (device_id.empty()) return std::nullopt;

    Entry* entry = find(device_id);
    if (entry == nullptr) {
        if (devices_.size() >= cfg_.max_devices) {
            // Bounded registry: evict the least recently seen device.
            size_t oldest = 0;
            for (size_t i = 1; i < devices_.size(); ++i) {
                if (devices_[i].last_seen < devices_[oldest].last_seen) oldest = i;
            }
            devices_.erase(devices_.begin() + static_cast<long>(oldest));
        }
        Entry fresh;
        fresh.device_id = device_id;
        fresh.online = true;
        fresh.last_seen = now;
        fresh.offline_reported = false;
        devices_.push_back(fresh);
        Transition t;
        t.device_id = device_id;
        t.online = true;
        t.first_seen = true;
        t.at = now;
        t.reason = "device seen for the first time";
        return t;
    }

    entry->last_seen = now;
    const bool was_offline = !entry->online;
    entry->online = true;
    if (was_offline) {
        // A reconnect is an AVAILABILITY transition. It is not a state replay and
        // not a fresh detection: the caller emits an availability observation
        // only, so a network blip cannot fabricate events.
        entry->offline_reported = false;
        Transition t;
        t.device_id = device_id;
        t.online = true;
        t.first_seen = false;
        t.at = now;
        t.reason = "device reconnected; availability restored (no state event is implied)";
        return t;
    }
    return std::nullopt;  // unchanged: silence keeps 'dead' distinguishable from 'silent'
}

std::vector<DeviceAvailabilityTracker::Transition> DeviceAvailabilityTracker::sweep(
    std::chrono::system_clock::time_point now) {
    std::vector<Transition> out;
    for (Entry& e : devices_) {
        if (!e.online || e.offline_reported) continue;
        if (now - e.last_seen < cfg_.offline_after) continue;
        e.online = false;
        e.offline_reported = true;
        Transition t;
        t.device_id = e.device_id;
        t.online = false;
        t.first_seen = false;
        t.at = now;
        t.reason = "no contact for " + std::to_string(cfg_.offline_after.count()) +
                   "s; device treated as offline";
        out.push_back(std::move(t));
    }
    return out;
}

bool DeviceAvailabilityTracker::online(const std::string& device_id) const {
    for (const Entry& e : devices_) {
        if (e.device_id == device_id) return e.online;
    }
    return false;  // unknown device is not assumed online
}

void DeviceAvailabilityTracker::clear() { devices_.clear(); }

// ---------------------------------------------------------------------------
// SensorDeduplicator
// ---------------------------------------------------------------------------

namespace {

bool same_key(const SensorDeduplicator::Key& a, const SensorDeduplicator::Key& b) {
    return a.device_id == b.device_id && a.source_type == b.source_type && a.state == b.state;
}

}  // namespace

bool SensorDeduplicator::duplicate(const Key& key, std::chrono::system_clock::time_point now) {
    for (Entry& e : entries_) {
        if (!same_key(e.key, key)) continue;
        if (now - e.seen_at < cfg_.cooldown) {
            return true;  // inside the cooldown: suppress
        }
        e.seen_at = now;
        return false;
    }
    if (entries_.size() >= cfg_.max_entries) {
        size_t oldest = 0;
        for (size_t i = 1; i < entries_.size(); ++i) {
            if (entries_[i].seen_at < entries_[oldest].seen_at) oldest = i;
        }
        entries_.erase(entries_.begin() + static_cast<long>(oldest));
    }
    Entry fresh;
    fresh.key = key;
    fresh.seen_at = now;
    entries_.push_back(fresh);
    return false;
}

void SensorDeduplicator::clear() { entries_.clear(); }

}  // namespace homeguardian
