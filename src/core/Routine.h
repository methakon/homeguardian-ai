#pragma once
// This model records CONFIGURATION only. It must never be treated as confirmation
// that a person completed a meal/medication/activity. A missing event remains
// "not confirmed", never a negative finding.

#include <string>
#include <chrono>
#include <optional>
#include <cstdint>
#include <cctype>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace homeguardian {

class Routine {
public:
    inline Routine(std::string routine_id,
                   std::string profile_id,
                   std::string label,
                   std::optional<std::string> schedule,
                   std::optional<std::string> time_zone,
                   bool enabled,
                   std::chrono::system_clock::time_point created_at,
                   std::chrono::system_clock::time_point updated_at,
                   uint32_t schema_version = 1)
        : routine_id_(std::move(routine_id)),
          profile_id_(std::move(profile_id)),
          label_(std::move(label)),
          schedule_(std::move(schedule)),
          time_zone_(std::move(time_zone)),
          enabled_(enabled),
          created_at_(created_at),
          updated_at_(updated_at),
          schema_version_(schema_version) {
        if (routine_id_.empty()) {
            throw std::invalid_argument("routine_id cannot be empty");
        }
        if (profile_id_.empty()) {
            throw std::invalid_argument("profile_id cannot be empty");
        }
        if (label_.empty()) {
            throw std::invalid_argument("label cannot be empty");
        }
        if (label_.length() > 128) {
            throw std::invalid_argument("label cannot exceed 128 characters");
        }
        if (schedule_.has_value()) {
            const auto& s = *schedule_;
            bool valid = false;
            if (s == "daily" || s == "weekly" || s == "weekdays") {
                valid = true;
            } else if (s.length() == 5 && s[2] == ':' &&
                       std::isdigit(static_cast<unsigned char>(s[0])) &&
                       std::isdigit(static_cast<unsigned char>(s[1])) &&
                       std::isdigit(static_cast<unsigned char>(s[3])) &&
                       std::isdigit(static_cast<unsigned char>(s[4]))) {
                int hh = (s[0] - '0') * 10 + (s[1] - '0');
                int mm = (s[3] - '0') * 10 + (s[4] - '0');
                if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
                    valid = true;
                }
            }
            if (!valid) {
                throw std::invalid_argument("schedule must be 'daily', 'weekly', 'weekdays', or 24-hour time 'HH:MM'");
            }
        }
        if (time_zone_.has_value()) {
            const auto& tz = *time_zone_;
            if (tz != "UTC" && tz != "Asia/Kolkata" && tz != "America/New_York" && tz != "Europe/London") {
                throw std::invalid_argument("time_zone must be one of: UTC, Asia/Kolkata, America/New_York, Europe/London");
            }
        }
        if (schema_version_ < 1) {
            throw std::invalid_argument("schema_version must be >= 1");
        }
    }

    // If schedule is present but time_zone is absent, that is allowed (interpretation defaults to UTC).
    inline Routine(std::string routine_id,
                   std::string profile_id,
                   std::string label,
                   std::optional<std::string> schedule = std::nullopt,
                   std::optional<std::string> time_zone = std::nullopt,
                   bool enabled = true)
        : Routine(std::move(routine_id),
                  std::move(profile_id),
                  std::move(label),
                  std::move(schedule),
                  std::move(time_zone),
                  enabled,
                  std::chrono::system_clock::now(),
                  std::chrono::system_clock::now(),
                  1) {}

    static inline Routine create(std::string routine_id,
                                 std::string profile_id,
                                 std::string label,
                                 std::optional<std::string> schedule = std::nullopt,
                                 std::optional<std::string> time_zone = std::nullopt,
                                 bool enabled = true) {
        return Routine(std::move(routine_id),
                       std::move(profile_id),
                       std::move(label),
                       std::move(schedule),
                       std::move(time_zone),
                       enabled);
    }

    inline nlohmann::json to_json() const {
        nlohmann::json j = {
            {"routine_id", routine_id_},
            {"profile_id", profile_id_},
            {"label", label_},
            {"enabled", enabled_},
            {"created_at", std::chrono::duration_cast<std::chrono::milliseconds>(created_at_.time_since_epoch()).count()},
            {"updated_at", std::chrono::duration_cast<std::chrono::milliseconds>(updated_at_.time_since_epoch()).count()},
            {"schema_version", schema_version_}
        };
        if (schedule_.has_value()) {
            j["schedule"] = *schedule_;
        }
        if (time_zone_.has_value()) {
            j["time_zone"] = *time_zone_;
        }
        return j;
    }

    static inline Routine from_json(const nlohmann::json& j) {
        auto created_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(j.at("created_at").get<int64_t>()));
        auto updated_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(j.at("updated_at").get<int64_t>()));

        std::optional<std::string> schedule;
        if (j.contains("schedule") && !j["schedule"].is_null()) {
            schedule = j["schedule"].get<std::string>();
        }

        std::optional<std::string> time_zone;
        if (j.contains("time_zone") && !j["time_zone"].is_null()) {
            time_zone = j["time_zone"].get<std::string>();
        }

        bool enabled = true;
        if (j.contains("enabled")) {
            enabled = j["enabled"].get<bool>();
        }

        uint32_t schema_version = 1;
        if (j.contains("schema_version")) {
            schema_version = j.at("schema_version").get<uint32_t>();
        }

        return Routine(
            j.at("routine_id").get<std::string>(),
            j.at("profile_id").get<std::string>(),
            j.at("label").get<std::string>(),
            schedule,
            time_zone,
            enabled,
            created_at,
            updated_at,
            schema_version
        );
    }

    const std::string& get_routine_id() const { return routine_id_; }
    const std::string& get_profile_id() const { return profile_id_; }
    const std::string& get_label() const { return label_; }
    const std::optional<std::string>& get_schedule() const { return schedule_; }
    const std::optional<std::string>& get_time_zone() const { return time_zone_; }
    bool is_enabled() const { return enabled_; }
    bool get_enabled() const { return enabled_; }
    std::chrono::system_clock::time_point get_created_at() const { return created_at_; }
    std::chrono::system_clock::time_point get_updated_at() const { return updated_at_; }
    uint32_t get_schema_version() const { return schema_version_; }

    const std::string& routine_id() const { return routine_id_; }
    const std::string& profile_id() const { return profile_id_; }
    const std::string& label() const { return label_; }
    const std::optional<std::string>& schedule() const { return schedule_; }
    const std::optional<std::string>& time_zone() const { return time_zone_; }
    bool enabled() const { return enabled_; }
    std::chrono::system_clock::time_point created_at() const { return created_at_; }
    std::chrono::system_clock::time_point updated_at() const { return updated_at_; }
    uint32_t schema_version() const { return schema_version_; }

private:
    std::string routine_id_;
    std::string profile_id_;
    std::string label_;
    std::optional<std::string> schedule_;
    std::optional<std::string> time_zone_;
    bool enabled_{true};
    std::chrono::system_clock::time_point created_at_;
    std::chrono::system_clock::time_point updated_at_;
    uint32_t schema_version_{1};
};

} // namespace homeguardian
