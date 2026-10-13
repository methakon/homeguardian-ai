#include "Observation.h"

#include <stdexcept>

namespace homeguardian {

void Observation::validate() const {
    if (observation_id.empty()) throw std::invalid_argument("observation_id cannot be empty");
    if (source.empty()) throw std::invalid_argument("source cannot be empty");
    if (detector_id.empty()) throw std::invalid_argument("detector_id cannot be empty");
    if (schema_version < 1) throw std::invalid_argument("schema_version must be >= 1");
    if (note.size() > kMaxNoteChars) throw std::invalid_argument("note too long");

    if (confidence.has_value() && (confidence.value() < 0.0 || confidence.value() > 1.0)) {
        throw std::invalid_argument("confidence must be in [0.0, 1.0]");
    }
    if (quality.has_value() && (quality.value() < 0.0 || quality.value() > 1.0)) {
        throw std::invalid_argument("quality must be in [0.0, 1.0]");
    }

    if (!features.is_object()) {
        throw std::invalid_argument("features must be a JSON object");
    }
    if (features.size() > kMaxFeatureKeys) {
        throw std::invalid_argument("too many feature keys");
    }
    // Bound the serialized size so a malformed detector cannot blat an
    // observation into the pipeline.
    if (features.dump().size() > kMaxFeatureJsonBytes) {
        throw std::invalid_argument("features payload too large");
    }
}

Event Observation::to_event() const {
    validate();

    nlohmann::json payload = features;
    payload["provenance"] = provenance_to_string(provenance);
    payload["detector_id"] = detector_id;
    if (!model_id.empty()) payload["model_id"] = model_id;
    payload["status"] = detector_status_to_string(status);
    if (quality.has_value()) payload["quality"] = quality.value();
    if (!note.empty()) payload["note"] = note;

    nlohmann::json meta = {
        {"provenance", provenance_to_string(provenance)},
        {"detector_id", detector_id},
        {"status", detector_status_to_string(status)}
    };
    if (!model_id.empty()) meta["model_id"] = model_id;

    auto now = observation_timestamp.time_since_epoch().count() == 0
                   ? std::chrono::system_clock::now()
                   : observation_timestamp;

    return Event(
        observation_id,
        schema_version,
        EventType::inference,
        source,
        now,                 // observation_timestamp
        std::chrono::system_clock::now(),  // ingestion_timestamp
        std::nullopt,
        confidence,          // mapped onto Event confidence when present
        std::nullopt,        // severity: set by correlation/alert layer, not here
        std::move(payload),
        std::nullopt,        // evidence_ref
        std::nullopt,        // correlation_id
        std::move(meta)      // processing_metadata (provenance/detector/status)
    );
}

std::optional<Observation> Observation::from_event(const Event& event) {
    // Only inference events produced by to_event() are detection observations.
    if (event.get_type() != EventType::inference) return std::nullopt;
    const nlohmann::json& p = event.get_payload();
    if (!p.is_object()) return std::nullopt;
    if (!p.contains("detector_id") || !p.contains("status")) return std::nullopt;
    if (!p["detector_id"].is_string() || !p["status"].is_string())
        return std::nullopt;

    Observation o;
    o.observation_id = event.get_event_id();
    o.source = event.get_source();
    o.provenance = (p.value("provenance", std::string("synthetic")) == "real")
                       ? Provenance::real
                       : Provenance::synthetic;
    o.detector_id = p["detector_id"].get<std::string>();
    o.model_id = p.value("model_id", std::string());
    o.schema_version = event.get_schema_version();
    o.observation_timestamp = event.get_observation_timestamp();

    const std::string status = p["status"].get<std::string>();
    if (status == "detected") o.status = DetectorStatus::detected;
    else if (status == "not_detected") o.status = DetectorStatus::not_detected;
    else if (status == "unavailable") o.status = DetectorStatus::unavailable;
    else if (status == "inconclusive") o.status = DetectorStatus::inconclusive;
    else return std::nullopt;  // unknown status is not a usable detection

    o.confidence = event.get_confidence();
    if (p.contains("quality") && p["quality"].is_number())
        o.quality = p["quality"].get<double>();
    if (p.contains("note") && p["note"].is_string())
        o.note = p["note"].get<std::string>();

    // Carry only the bounded, known non-sensitive features.
    nlohmann::json features = nlohmann::json::object();
    if (p.contains("energy") && p["energy"].is_number())
        features["energy"] = p["energy"];
    if (p.contains("detect_threshold") && p["detect_threshold"].is_number())
        features["detect_threshold"] = p["detect_threshold"];
    o.features = std::move(features);

    return o;
}

}  // namespace homeguardian
