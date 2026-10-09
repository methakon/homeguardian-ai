#pragma once
#include <string>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace homeguardian {

enum class EventType { observation, inference, alert };
enum class Severity { info, warning, critical };

NLOHMANN_JSON_SERIALIZE_ENUM(EventType, {
    {EventType::observation, "observation"},
    {EventType::inference, "inference"},
    {EventType::alert, "alert"}
})

NLOHMANN_JSON_SERIALIZE_ENUM(Severity, {
    {Severity::info, "info"},
    {Severity::warning, "warning"},
    {Severity::critical, "critical"}
})

class Event {
public:
    inline Event(std::string event_id,
          uint32_t schema_version,
          EventType type,
          std::string source,
          std::chrono::system_clock::time_point observation_timestamp,
          std::chrono::system_clock::time_point ingestion_timestamp,
          std::optional<std::chrono::steady_clock::time_point> monotonic_timestamp,
          std::optional<double> confidence,
          std::optional<Severity> severity,
          nlohmann::json payload,
          std::optional<std::string> evidence_ref,
          std::optional<std::string> correlation_id,
          std::optional<nlohmann::json> processing_metadata)
        : event_id_(std::move(event_id)), schema_version_(schema_version), type_(type),
          source_(std::move(source)), observation_timestamp_(observation_timestamp),
          ingestion_timestamp_(ingestion_timestamp), monotonic_timestamp_(monotonic_timestamp),
          confidence_(confidence), severity_(severity), payload_(std::move(payload)),
          evidence_ref_(std::move(evidence_ref)), correlation_id_(std::move(correlation_id)),
          processing_metadata_(std::move(processing_metadata)) {
        
        if (event_id_.empty()) throw std::invalid_argument("event_id cannot be empty");
        if (schema_version_ < 1) throw std::invalid_argument("schema_version must be >= 1");
        if (source_.empty()) throw std::invalid_argument("source cannot be empty");
        
        if (confidence_.has_value() && (confidence_.value() < 0.0 || confidence_.value() > 1.0)) {
            throw std::invalid_argument("confidence must be in [0.0, 1.0]");
        }
        
        if (!payload_.is_object() && !payload_.is_null()) {
            throw std::invalid_argument("payload must be an object or null");
        }
    }

    static inline Event create_observation(std::string event_id, std::string source, nlohmann::json payload) {
        auto now = std::chrono::system_clock::now();
        return Event(std::move(event_id), 1, EventType::observation, std::move(source), now, now, std::nullopt, std::nullopt, std::nullopt, std::move(payload), std::nullopt, std::nullopt, std::nullopt);
    }

    static inline Event create_inference(std::string event_id, std::string source, nlohmann::json payload, double confidence) {
        auto now = std::chrono::system_clock::now();
        return Event(std::move(event_id), 1, EventType::inference, std::move(source), now, now, std::nullopt, confidence, std::nullopt, std::move(payload), std::nullopt, std::nullopt, std::nullopt);
    }

    static inline Event create_alert(std::string event_id, std::string source, nlohmann::json payload, Severity severity) {
        auto now = std::chrono::system_clock::now();
        return Event(std::move(event_id), 1, EventType::alert, std::move(source), now, now, std::nullopt, std::nullopt, severity, std::move(payload), std::nullopt, std::nullopt, std::nullopt);
    }

    inline nlohmann::json to_json() const {
        nlohmann::json j = {
            {"event_id", event_id_},
            {"schema_version", schema_version_},
            {"type", type_},
            {"source", source_},
            {"observation_timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(observation_timestamp_.time_since_epoch()).count()},
            {"ingestion_timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(ingestion_timestamp_.time_since_epoch()).count()},
            {"payload", payload_}
        };
        if (confidence_) j["confidence"] = *confidence_;
        if (severity_) j["severity"] = *severity_;
        if (evidence_ref_) j["evidence_ref"] = *evidence_ref_;
        if (correlation_id_) j["correlation_id"] = *correlation_id_;
        if (processing_metadata_) j["processing_metadata"] = *processing_metadata_;
        return j;
    }

    static inline Event from_json(const nlohmann::json& j) {
        auto obs_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("observation_timestamp").get<int64_t>()));
        auto ing_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("ingestion_timestamp").get<int64_t>()));
        
        std::optional<double> conf;
        if (j.contains("confidence") && !j["confidence"].is_null()) conf = j["confidence"].get<double>();
        
        std::optional<Severity> sev;
        if (j.contains("severity") && !j["severity"].is_null()) sev = j["severity"].get<Severity>();
        
        std::optional<std::string> ev;
        if (j.contains("evidence_ref") && !j["evidence_ref"].is_null()) ev = j["evidence_ref"].get<std::string>();
        
        std::optional<std::string> corr;
        if (j.contains("correlation_id") && !j["correlation_id"].is_null()) corr = j["correlation_id"].get<std::string>();
        
        std::optional<nlohmann::json> meta;
        if (j.contains("processing_metadata") && !j["processing_metadata"].is_null()) meta = j["processing_metadata"];

        return Event(
            j.at("event_id").get<std::string>(),
            j.at("schema_version").get<uint32_t>(),
            j.at("type").get<EventType>(),
            j.at("source").get<std::string>(),
            obs_ts,
            ing_ts,
            std::nullopt,
            conf,
            sev,
            j.at("payload"),
            ev,
            corr,
            meta
        );
    }

    const std::string& get_event_id() const { return event_id_; }
    uint32_t get_schema_version() const { return schema_version_; }
    EventType get_type() const { return type_; }
    const std::string& get_source() const { return source_; }
    std::chrono::system_clock::time_point get_observation_timestamp() const { return observation_timestamp_; }
    std::chrono::system_clock::time_point get_ingestion_timestamp() const { return ingestion_timestamp_; }
    const std::optional<std::chrono::steady_clock::time_point>& get_monotonic_timestamp() const { return monotonic_timestamp_; }
    std::optional<double> get_confidence() const { return confidence_; }
    std::optional<Severity> get_severity() const { return severity_; }
    const nlohmann::json& get_payload() const { return payload_; }
    const std::optional<std::string>& get_evidence_ref() const { return evidence_ref_; }
    const std::optional<std::string>& get_correlation_id() const { return correlation_id_; }
    const std::optional<nlohmann::json>& get_processing_metadata() const { return processing_metadata_; }

private:
    std::string event_id_;
    uint32_t schema_version_;
    EventType type_;
    std::string source_;
    std::chrono::system_clock::time_point observation_timestamp_;
    std::chrono::system_clock::time_point ingestion_timestamp_;
    std::optional<std::chrono::steady_clock::time_point> monotonic_timestamp_;
    std::optional<double> confidence_;
    std::optional<Severity> severity_;
    nlohmann::json payload_;
    std::optional<std::string> evidence_ref_;
    std::optional<std::string> correlation_id_;
    std::optional<nlohmann::json> processing_metadata_;
};

} // namespace homeguardian
