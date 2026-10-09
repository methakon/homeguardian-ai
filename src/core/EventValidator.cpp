#include "EventValidator.h"
#include <cmath>
#include <algorithm>
#include <cctype>

namespace homeguardian {

static inline void trim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

std::optional<std::string> EventValidator::validate(const Event& event) {
    if (event.get_event_id().empty()) return "event_id is empty";
    if (event.get_schema_version() < 1) return "schema_version is < 1";
    if (event.get_source().empty()) return "source is empty";
    
    auto now = std::chrono::system_clock::now();
    auto skew = std::chrono::seconds(1);
    if (event.get_observation_timestamp() > now + skew) {
        return "observation_timestamp is in the future";
    }
    
    if (event.get_confidence().has_value()) {
        double conf = event.get_confidence().value();
        if (conf < 0.0 || conf > 1.0) return "confidence not in [0.0, 1.0]";
    }
    
    if (!event.get_payload().is_object() && !event.get_payload().is_null()) {
        return "payload is not an object or null";
    }
    
    return std::nullopt;
}

Event EventValidator::normalize(const Event& event) {
    std::string src = event.get_source();
    trim(src);
    
    std::optional<double> conf = event.get_confidence();
    if (conf.has_value()) {
        conf = std::round(conf.value() * 10000.0) / 10000.0;
    }
    
    auto ingestion = event.get_ingestion_timestamp();
    if (ingestion.time_since_epoch().count() == 0) {
        ingestion = std::chrono::system_clock::now();
    }
    
    return Event(
        event.get_event_id(),
        event.get_schema_version(),
        event.get_type(),
        src,
        event.get_observation_timestamp(),
        ingestion,
        event.get_monotonic_timestamp(),
        conf,
        event.get_severity(),
        event.get_payload(),
        event.get_evidence_ref(),
        event.get_correlation_id(),
        event.get_processing_metadata()
    );
}

} // namespace homeguardian
