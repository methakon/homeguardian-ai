#include "core/SensorEventConsumer.h"

#include <algorithm>
#include <cmath>

namespace homeguardian {

const char* ingest_code_to_string(IngestCode code) {
    switch (code) {
        case IngestCode::accepted:            return "accepted";
        case IngestCode::not_an_event:        return "not_an_event";
        case IngestCode::unsupported_schema:  return "unsupported_schema";
        case IngestCode::malformed:           return "malformed";
        case IngestCode::source_identity:     return "source_identity";
        case IngestCode::provenance_invalid:  return "provenance_invalid";
        case IngestCode::provenance_conflict: return "provenance_conflict";
        case IngestCode::future_timestamp:    return "future_timestamp";
        case IngestCode::stale_or_replay:     return "stale_or_replay";
        case IngestCode::duplicate:           return "duplicate";
        case IngestCode::consent_missing:     return "consent_missing";
        case IngestCode::source_offline:      return "source_offline";
    }
    return "malformed";
}

namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& n) {
        return !n.empty() && haystack.find(n) != std::string::npos;
    });
}

// The observation id, whichever name the producer used for it.
std::string observation_id_of(const nlohmann::json& event) {
    for (const char* key : {"observation_id", "event_id", "id"}) {
        const auto it = event.find(key);
        if (it != event.end() && it->is_string()) return it->get<std::string>();
    }
    return {};
}

}  // namespace

SensorEventConsumer::SensorEventConsumer(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.max_window == 0) cfg_.max_window = kDefaultMaxWindow;
}

bool SensorEventConsumer::saw_observation(const std::string& observation_id) const {
    return std::find(seen_ids_.begin(), seen_ids_.end(), observation_id) != seen_ids_.end();
}

IngestResult SensorEventConsumer::ingest(const nlohmann::json& event,
                                         std::chrono::system_clock::time_point now) {
    const auto refuse = [&](IngestCode code, std::string detail) {
        ++rejected_;
        return IngestResult{false, code, std::move(detail)};
    };

    if (!event.is_object()) return refuse(IngestCode::malformed, "the event is not a JSON object");

    // ---- schema ---------------------------------------------------------------------------------
    const auto type_it = event.find("type");
    if (type_it == event.end() || !type_it->is_string()) {
        return refuse(IngestCode::malformed, "no string 'type' field");
    }
    if (type_it->get<std::string>() != "inference") {
        return refuse(IngestCode::not_an_event,
                      "type is '" + type_it->get<std::string>() + "', not an observation");
    }
    const auto schema_it = event.find("schema_version");
    if (schema_it == event.end() || !schema_it->is_number_integer()) {
        return refuse(IngestCode::malformed, "no integer 'schema_version'");
    }
    if (schema_it->get<int>() != 1) {
        return refuse(IngestCode::unsupported_schema,
                      "schema_version " + std::to_string(schema_it->get<int>()) + " is not supported");
    }
    for (const char* key : {"source", "detector_id", "provenance", "status"}) {
        const auto it = event.find(key);
        if (it == event.end()) return refuse(IngestCode::malformed, std::string("no '") + key + "' field");
        if (!it->is_string()) {
            return refuse(IngestCode::malformed, std::string("'") + key + "' is not a string");
        }
    }
    const auto features_it = event.find("features");
    if (features_it == event.end() || !features_it->is_object()) {
        return refuse(IngestCode::malformed, "'features' is missing or not an object");
    }
    const auto confidence_it = event.find("confidence");
    if (confidence_it != event.end() && !confidence_it->is_number()) {
        return refuse(IngestCode::malformed, "'confidence' is not a number");
    }

    const std::string source = event["source"].get<std::string>();
    const std::string detector_id = event["detector_id"].get<std::string>();
    const std::string provenance = event["provenance"].get<std::string>();
    const std::string status = event["status"].get<std::string>();

    if (source.empty()) {
        return refuse(IngestCode::source_identity, "the source is empty: an observation with no origin "
                                                   "cannot be attributed");
    }
    if (detector_id.empty()) return refuse(IngestCode::malformed, "the detector_id is empty");
    if (observation_id_of(event).empty()) {
        return refuse(IngestCode::malformed, "no observation id (observation_id/event_id/id)");
    }

    // ---- identity -------------------------------------------------------------------------------
    if (!cfg_.trusted_source_prefixes.empty() &&
        !std::any_of(cfg_.trusted_source_prefixes.begin(), cfg_.trusted_source_prefixes.end(),
                     [&](const std::string& p) { return starts_with(source, p); })) {
        return refuse(IngestCode::source_identity,
                      "source '" + source + "' is not one this consumer is configured to trust");
    }

    // ---- provenance integrity -------------------------------------------------------------------
    if (provenance != "real" && provenance != "synthetic") {
        return refuse(IngestCode::provenance_invalid,
                      provenance == "mixed"
                          ? "an observation cannot claim to be mixed: mixed evidence is derived by the "
                            "fusion engine from its members, never asserted by a producer"
                          : "provenance '" + provenance + "' is not real or synthetic");
    }
    if (provenance == "real" && contains_any(source, cfg_.fixture_source_hints)) {
        return refuse(IngestCode::provenance_conflict,
                      "source '" + source + "' identifies itself as a fixture but claims real evidence");
    }

    // ---- consent --------------------------------------------------------------------------------
    if (cfg_.require_consent) {
        const auto consent_it = event.find(cfg_.consent_field);
        const bool granted = consent_it != event.end() &&
                             ((consent_it->is_string() && consent_it->get<std::string>() == "granted") ||
                              (consent_it->is_boolean() && consent_it->get<bool>()));
        if (!granted) {
            // Consent is not a validation nicety: without it there is no capture, so the source is
            // treated as unavailable rather than as a source reporting quiet.
            source_available_ = false;
            return refuse(IngestCode::consent_missing,
                          "consent is required and '" + cfg_.consent_field +
                              "' is not granted: capture stops");
        }
    }

    // ---- the producer says the source is not there ----------------------------------------------
    if (status == "unavailable") {
        source_available_ = false;
        return refuse(IngestCode::source_offline,
                      "the producer reports its source unavailable: no evidence is recorded, and no "
                      "quiet is invented");
    }
    // The status vocabulary is the observation type's own: a refusal is expressed as "unavailable"
    // with a reason, because DetectorStatus has no separate refused member.
    if (status != "detected" && status != "not_detected" && status != "inconclusive") {
        return refuse(IngestCode::malformed, "status '" + status + "' is not a known status");
    }

    // ---- timestamps and replay ------------------------------------------------------------------
    const auto ts_it = event.find("observed_at") != event.end() ? event.find("observed_at")
                                                                : event.find("timestamp");
    if (ts_it == event.end() || !ts_it->is_number()) {
        return refuse(IngestCode::malformed, "no numeric 'observed_at'/'timestamp' (unix seconds)");
    }
    const double seconds = ts_it->get<double>();
    const auto observed_at = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(seconds)));
    if (observed_at > now + cfg_.clock_skew) {
        return refuse(IngestCode::future_timestamp, "the observation is stamped in the future");
    }
    if (have_last_timestamp_ && observed_at < last_timestamp_) {
        return refuse(IngestCode::stale_or_replay,
                      "older than the last accepted observation: out of order or a replay");
    }
    const std::string id = observation_id_of(event);
    if (saw_observation(id)) {
        return refuse(IngestCode::duplicate, "observation '" + id + "' was already accepted");
    }

    // ---- normalize ------------------------------------------------------------------------------
    Observation observation;
    observation.observation_id = id;
    observation.source = source;
    observation.detector_id = detector_id;
    if (const auto model_it = event.find("model_id"); model_it != event.end() && model_it->is_string()) {
        observation.model_id = model_it->get<std::string>();
    }
    observation.provenance = provenance == "real" ? Provenance::real : Provenance::synthetic;
    observation.status = status == "detected"       ? DetectorStatus::detected
                         : status == "not_detected" ? DetectorStatus::not_detected
                                                    : DetectorStatus::inconclusive;
    observation.confidence = confidence_it != event.end()
                                 ? std::optional<double>(confidence_it->get<double>())
                                 : std::nullopt;
    observation.features = *features_it;
    observation.observation_timestamp = observed_at;

    if (window_.size() >= cfg_.max_window) window_.erase(window_.begin());
    window_.push_back(std::move(observation));
    seen_ids_.push_back(id);
    have_last_timestamp_ = true;
    last_timestamp_ = observed_at;
    source_available_ = true;
    ++accepted_;
    return IngestResult{true, IngestCode::accepted, "accepted as " + id};
}

FusionResult SensorEventConsumer::fuse() const {
    const FusionEngine engine;
    return engine.fuse(window_);
}

}  // namespace homeguardian
