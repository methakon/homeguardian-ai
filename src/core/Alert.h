#pragma once
#include <string>
#include <chrono>
#include <vector>
#include <nlohmann/json.hpp>
#include "Event.h"
#include <stdexcept>

namespace homeguardian {

class Alert {
public:
    std::string alert_id;
    Severity severity;
    std::string message;
    std::vector<std::string> contributing_event_ids;
    std::chrono::system_clock::time_point detected_at;
    double confidence;
    std::string rule_name;
    nlohmann::json details;

    inline Alert(std::string id, Severity sev, std::string msg, std::vector<std::string> event_ids,
          std::chrono::system_clock::time_point time, double conf, std::string rule, nlohmann::json dets)
        : alert_id(std::move(id)), severity(sev), message(std::move(msg)),
          contributing_event_ids(std::move(event_ids)), detected_at(time), confidence(conf),
          rule_name(std::move(rule)), details(std::move(dets)) {
        if (alert_id.empty()) throw std::invalid_argument("alert_id cannot be empty");
        if (confidence < 0.0 || confidence > 1.0) throw std::invalid_argument("confidence must be in [0.0, 1.0]");
    }

    static inline Alert create(std::string id, Severity sev, std::string msg, std::vector<std::string> event_ids,
                        double conf, std::string rule, nlohmann::json dets) {
        return Alert(std::move(id), sev, std::move(msg), std::move(event_ids),
                     std::chrono::system_clock::now(), conf, std::move(rule), std::move(dets));
    }

    inline nlohmann::json to_json() const {
        return {
            {"alert_id", alert_id},
            {"severity", severity},
            {"message", message},
            {"contributing_event_ids", contributing_event_ids},
            {"detected_at", std::chrono::duration_cast<std::chrono::milliseconds>(detected_at.time_since_epoch()).count()},
            {"confidence", confidence},
            {"rule_name", rule_name},
            {"details", details}
        };
    }

    static inline Alert from_json(const nlohmann::json& j) {
        auto time = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("detected_at").get<int64_t>()));
        return Alert(
            j.at("alert_id").get<std::string>(),
            j.at("severity").get<Severity>(),
            j.at("message").get<std::string>(),
            j.at("contributing_event_ids").get<std::vector<std::string>>(),
            time,
            j.at("confidence").get<double>(),
            j.at("rule_name").get<std::string>(),
            j.at("details")
        );
    }
};

} // namespace homeguardian
