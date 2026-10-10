#pragma once
// A stored record is household configuration, not proof of legally valid consent;
// withdrawal prevents future processing for that purpose.

#include <string>
#include <chrono>
#include <vector>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace homeguardian {

enum class ConsentDecision {
    granted,
    denied,
    withdrawn
};

NLOHMANN_JSON_SERIALIZE_ENUM(ConsentDecision, {
    {ConsentDecision::granted, "granted"},
    {ConsentDecision::denied, "denied"},
    {ConsentDecision::withdrawn, "withdrawn"}
})

class ConsentRecord {
public:
    inline ConsentRecord(std::string consent_id,
                         std::string subject_scope,
                         std::string purpose,
                         std::vector<std::string> data_categories,
                         ConsentDecision decision,
                         std::string policy_version,
                         std::chrono::system_clock::time_point recorded_at,
                         std::optional<std::chrono::system_clock::time_point> expires_at,
                         std::string provenance,
                         uint32_t schema_version = 1)
        : consent_id_(std::move(consent_id)),
          subject_scope_(std::move(subject_scope)),
          purpose_(std::move(purpose)),
          data_categories_(std::move(data_categories)),
          decision_(decision),
          policy_version_(std::move(policy_version)),
          recorded_at_(recorded_at),
          expires_at_(expires_at),
          provenance_(std::move(provenance)),
          schema_version_(schema_version) {
        if (consent_id_.empty()) {
            throw std::invalid_argument("consent_id cannot be empty");
        }
        if (subject_scope_.empty()) {
            throw std::invalid_argument("subject_scope cannot be empty");
        }
        if (purpose_.empty()) {
            throw std::invalid_argument("purpose cannot be empty");
        }
        if (decision_ == ConsentDecision::granted && data_categories_.empty()) {
            throw std::invalid_argument("data_categories cannot be empty when decision is granted");
        }
        if (policy_version_.empty()) {
            throw std::invalid_argument("policy_version cannot be empty");
        }
        if (provenance_.empty()) {
            throw std::invalid_argument("provenance cannot be empty");
        }
        if (schema_version_ < 1) {
            throw std::invalid_argument("schema_version must be >= 1");
        }
    }

    inline ConsentRecord(std::string consent_id,
                         std::string subject_scope,
                         std::string purpose,
                         std::vector<std::string> data_categories,
                         ConsentDecision decision,
                         std::string policy_version,
                         std::string provenance,
                         std::optional<std::chrono::system_clock::time_point> expires_at = std::nullopt)
        : ConsentRecord(std::move(consent_id),
                        std::move(subject_scope),
                        std::move(purpose),
                        std::move(data_categories),
                        decision,
                        std::move(policy_version),
                        std::chrono::system_clock::now(),
                        expires_at,
                        std::move(provenance),
                        1) {}

    static inline ConsentRecord create_granted(std::string consent_id,
                                               std::string subject_scope,
                                               std::string purpose,
                                               std::vector<std::string> data_categories,
                                               std::string policy_version,
                                               std::string provenance,
                                               std::optional<std::chrono::system_clock::time_point> expires_at = std::nullopt) {
        return ConsentRecord(std::move(consent_id),
                             std::move(subject_scope),
                             std::move(purpose),
                             std::move(data_categories),
                             ConsentDecision::granted,
                             std::move(policy_version),
                             std::chrono::system_clock::now(),
                             expires_at,
                             std::move(provenance),
                             1);
    }

    static inline ConsentRecord create_denied(std::string consent_id,
                                              std::string subject_scope,
                                              std::string purpose,
                                              std::string policy_version,
                                              std::string provenance) {
        return ConsentRecord(std::move(consent_id),
                             std::move(subject_scope),
                             std::move(purpose),
                             {},
                             ConsentDecision::denied,
                             std::move(policy_version),
                             std::chrono::system_clock::now(),
                             std::nullopt,
                             std::move(provenance),
                             1);
    }

    static inline ConsentRecord create_withdrawn(std::string consent_id,
                                                 std::string subject_scope,
                                                 std::string purpose,
                                                 std::string policy_version,
                                                 std::string provenance) {
        return ConsentRecord(std::move(consent_id),
                             std::move(subject_scope),
                             std::move(purpose),
                             {},
                             ConsentDecision::withdrawn,
                             std::move(policy_version),
                             std::chrono::system_clock::now(),
                             std::nullopt,
                             std::move(provenance),
                             1);
    }

    inline bool is_active(std::chrono::system_clock::time_point now) const {
        if (decision_ != ConsentDecision::granted) {
            return false;
        }
        if (expires_at_.has_value() && now >= expires_at_.value()) {
            return false;
        }
        return true;
    }

    inline bool is_active() const {
        return is_active(std::chrono::system_clock::now());
    }

    // Rule: Withdrawn and denied decisions are never active under any circumstance (default-deny).
    static inline bool is_decision_active(ConsentDecision decision,
                                          std::chrono::system_clock::time_point now,
                                          const std::optional<std::chrono::system_clock::time_point>& expires_at) {
        if (decision != ConsentDecision::granted) {
            return false;
        }
        if (expires_at.has_value() && now >= expires_at.value()) {
            return false;
        }
        return true;
    }

    // Static helper: Withdrawn and denied are never active. Only granted can be active.
    static inline bool can_decision_be_active(ConsentDecision decision) {
        return decision == ConsentDecision::granted;
    }

    inline nlohmann::json to_json() const {
        nlohmann::json j = {
            {"consent_id", consent_id_},
            {"subject_scope", subject_scope_},
            {"purpose", purpose_},
            {"data_categories", data_categories_},
            {"decision", decision_},
            {"policy_version", policy_version_},
            {"recorded_at", std::chrono::duration_cast<std::chrono::milliseconds>(recorded_at_.time_since_epoch()).count()},
            {"provenance", provenance_},
            {"schema_version", schema_version_}
        };
        if (expires_at_.has_value()) {
            j["expires_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at_->time_since_epoch()).count();
        }
        return j;
    }

    static inline ConsentRecord from_json(const nlohmann::json& j) {
        auto rec_ts = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(j.at("recorded_at").get<int64_t>()));

        std::optional<std::chrono::system_clock::time_point> exp_ts;
        if (j.contains("expires_at") && !j["expires_at"].is_null()) {
            exp_ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(j["expires_at"].get<int64_t>()));
        }

        uint32_t schema_version = 1;
        if (j.contains("schema_version")) {
            schema_version = j.at("schema_version").get<uint32_t>();
        }

        std::vector<std::string> data_categories;
        if (j.contains("data_categories") && !j["data_categories"].is_null()) {
            data_categories = j.at("data_categories").get<std::vector<std::string>>();
        }

        return ConsentRecord(
            j.at("consent_id").get<std::string>(),
            j.at("subject_scope").get<std::string>(),
            j.at("purpose").get<std::string>(),
            data_categories,
            j.at("decision").get<ConsentDecision>(),
            j.at("policy_version").get<std::string>(),
            rec_ts,
            exp_ts,
            j.at("provenance").get<std::string>(),
            schema_version
        );
    }

    const std::string& get_consent_id() const { return consent_id_; }
    const std::string& get_subject_scope() const { return subject_scope_; }
    const std::string& get_purpose() const { return purpose_; }
    const std::vector<std::string>& get_data_categories() const { return data_categories_; }
    ConsentDecision get_decision() const { return decision_; }
    const std::string& get_policy_version() const { return policy_version_; }
    std::chrono::system_clock::time_point get_recorded_at() const { return recorded_at_; }
    const std::optional<std::chrono::system_clock::time_point>& get_expires_at() const { return expires_at_; }
    const std::string& get_provenance() const { return provenance_; }
    uint32_t get_schema_version() const { return schema_version_; }

    const std::string& consent_id() const { return consent_id_; }
    const std::string& subject_scope() const { return subject_scope_; }
    const std::string& purpose() const { return purpose_; }
    const std::vector<std::string>& data_categories() const { return data_categories_; }
    ConsentDecision decision() const { return decision_; }
    const std::string& policy_version() const { return policy_version_; }
    std::chrono::system_clock::time_point recorded_at() const { return recorded_at_; }
    const std::optional<std::chrono::system_clock::time_point>& expires_at() const { return expires_at_; }
    const std::string& provenance() const { return provenance_; }
    uint32_t schema_version() const { return schema_version_; }

private:
    std::string consent_id_;
    std::string subject_scope_;
    std::string purpose_;
    std::vector<std::string> data_categories_;
    ConsentDecision decision_;
    std::string policy_version_;
    std::chrono::system_clock::time_point recorded_at_;
    std::optional<std::chrono::system_clock::time_point> expires_at_;
    std::string provenance_;
    uint32_t schema_version_{1};
};

} // namespace homeguardian
