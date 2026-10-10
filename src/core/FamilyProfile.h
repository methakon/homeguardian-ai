#pragma once
#include <string>
#include <chrono>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace homeguardian {

enum class AgeBand {
    child,
    teen,
    adult,
    senior
};

NLOHMANN_JSON_SERIALIZE_ENUM(AgeBand, {
    {AgeBand::child, "child"},
    {AgeBand::teen, "teen"},
    {AgeBand::adult, "adult"},
    {AgeBand::senior, "senior"}
})

class FamilyProfile {
public:
    inline FamilyProfile(std::string profile_id,
                         std::string display_name,
                         std::optional<AgeBand> age_band,
                         bool enabled,
                         std::chrono::system_clock::time_point created_at,
                         std::chrono::system_clock::time_point updated_at,
                         uint32_t schema_version = 1)
        : profile_id_(std::move(profile_id)),
          display_name_(std::move(display_name)),
          age_band_(age_band),
          enabled_(enabled),
          created_at_(created_at),
          updated_at_(updated_at),
          schema_version_(schema_version) {
        if (profile_id_.empty()) {
            throw std::invalid_argument("profile_id cannot be empty");
        }
        if (display_name_.empty()) {
            throw std::invalid_argument("display_name cannot be empty");
        }
        if (display_name_.length() > 128) {
            throw std::invalid_argument("display_name cannot exceed 128 characters");
        }
        if (schema_version_ < 1) {
            throw std::invalid_argument("schema_version must be >= 1");
        }
    }

    inline FamilyProfile(std::string profile_id,
                         std::string display_name,
                         std::optional<AgeBand> age_band = std::nullopt,
                         bool enabled = true)
        : FamilyProfile(std::move(profile_id),
                        std::move(display_name),
                        age_band,
                        enabled,
                        std::chrono::system_clock::now(),
                        std::chrono::system_clock::now(),
                        1) {}

    static inline FamilyProfile create(std::string profile_id,
                                       std::string display_name,
                                       std::optional<AgeBand> age_band = std::nullopt,
                                       bool enabled = true) {
        return FamilyProfile(std::move(profile_id), std::move(display_name), age_band, enabled);
    }

    inline nlohmann::json to_json() const {
        nlohmann::json j = {
            {"profile_id", profile_id_},
            {"display_name", display_name_},
            {"enabled", enabled_},
            {"created_at", std::chrono::duration_cast<std::chrono::milliseconds>(created_at_.time_since_epoch()).count()},
            {"updated_at", std::chrono::duration_cast<std::chrono::milliseconds>(updated_at_.time_since_epoch()).count()},
            {"schema_version", schema_version_}
        };
        if (age_band_.has_value()) {
            j["age_band"] = *age_band_;
        }
        return j;
    }

    static inline FamilyProfile from_json(const nlohmann::json& j) {
        auto created_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(j.at("created_at").get<int64_t>()));
        auto updated_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(j.at("updated_at").get<int64_t>()));

        std::optional<AgeBand> age_band;
        if (j.contains("age_band") && !j["age_band"].is_null()) {
            age_band = j["age_band"].get<AgeBand>();
        }

        bool enabled = true;
        if (j.contains("enabled")) {
            enabled = j["enabled"].get<bool>();
        }

        uint32_t schema_version = 1;
        if (j.contains("schema_version")) {
            schema_version = j.at("schema_version").get<uint32_t>();
        }

        return FamilyProfile(
            j.at("profile_id").get<std::string>(),
            j.at("display_name").get<std::string>(),
            age_band,
            enabled,
            created_at,
            updated_at,
            schema_version
        );
    }

    const std::string& get_profile_id() const { return profile_id_; }
    const std::string& get_display_name() const { return display_name_; }
    const std::optional<AgeBand>& get_age_band() const { return age_band_; }
    bool is_enabled() const { return enabled_; }
    bool get_enabled() const { return enabled_; }
    std::chrono::system_clock::time_point get_created_at() const { return created_at_; }
    std::chrono::system_clock::time_point get_updated_at() const { return updated_at_; }
    uint32_t get_schema_version() const { return schema_version_; }

    const std::string& profile_id() const { return profile_id_; }
    const std::string& display_name() const { return display_name_; }
    const std::optional<AgeBand>& age_band() const { return age_band_; }
    bool enabled() const { return enabled_; }
    std::chrono::system_clock::time_point created_at() const { return created_at_; }
    std::chrono::system_clock::time_point updated_at() const { return updated_at_; }
    uint32_t schema_version() const { return schema_version_; }

private:
    std::string profile_id_;
    std::string display_name_;
    std::optional<AgeBand> age_band_;
    bool enabled_{true};
    std::chrono::system_clock::time_point created_at_;
    std::chrono::system_clock::time_point updated_at_;
    uint32_t schema_version_{1};
};

} // namespace homeguardian
