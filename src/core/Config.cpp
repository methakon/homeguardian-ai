#include "Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace homeguardian {

void Config::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open config file for reading: " + filepath);
    }
    
    json j;
    file >> j;
    
    if (j.contains("log_level")) log_level = j["log_level"].get<std::string>();
    if (j.contains("server_host")) server_host = j["server_host"].get<std::string>();
    if (j.contains("server_port")) server_port = j["server_port"].get<int>();
    if (j.contains("data_dir")) data_dir = j["data_dir"].get<std::string>();
    
    if (j.contains("max_event_history")) max_event_history = j["max_event_history"].get<size_t>();
    if (j.contains("correlation_window_ms")) correlation_window_ms = j["correlation_window_ms"].get<int>();
    if (j.contains("alert_confidence_threshold")) alert_confidence_threshold = j["alert_confidence_threshold"].get<double>();

    if (j.contains("media_capture_enabled")) media_capture_enabled = j["media_capture_enabled"].get<bool>();
    if (j.contains("cloud_processing_enabled")) cloud_processing_enabled = j["cloud_processing_enabled"].get<bool>();
    if (j.contains("consent_policy_version")) consent_policy_version = j["consent_policy_version"].get<std::string>();
    if (j.contains("routine_retention_days")) routine_retention_days = j["routine_retention_days"].get<int>();
    if (j.contains("profile_retention_days")) profile_retention_days = j["profile_retention_days"].get<int>();

    validate();
}

void Config::save(const std::string& filepath) const {
    validate();
    
    json j;
    j["log_level"] = log_level;
    j["server_host"] = server_host;
    j["server_port"] = server_port;
    j["data_dir"] = data_dir;
    
    j["max_event_history"] = max_event_history;
    j["correlation_window_ms"] = correlation_window_ms;
    j["alert_confidence_threshold"] = alert_confidence_threshold;

    j["media_capture_enabled"] = media_capture_enabled;
    j["cloud_processing_enabled"] = cloud_processing_enabled;
    j["consent_policy_version"] = consent_policy_version;
    j["routine_retention_days"] = routine_retention_days;
    j["profile_retention_days"] = profile_retention_days;

    std::ofstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open config file for writing: " + filepath);
    }
    
    file << j.dump(4);
}

void Config::validate() const {
    if (server_port < 1 || server_port > 65535) {
        throw std::invalid_argument("Invalid server_port. Must be between 1 and 65535.");
    }
    
    if (log_level != "trace" && log_level != "debug" && log_level != "info" && 
        log_level != "warn" && log_level != "error" && log_level != "critical") {
        throw std::invalid_argument("Invalid log_level. Must be one of {trace, debug, info, warn, error, critical}.");
    }
    
    if (max_event_history == 0) {
        throw std::invalid_argument("max_event_history must be > 0");
    }
    if (correlation_window_ms <= 0 || correlation_window_ms > 60000) {
        throw std::invalid_argument("correlation_window_ms must be > 0 and <= 60000");
    }
    if (alert_confidence_threshold < 0.0 || alert_confidence_threshold > 1.0) {
        throw std::invalid_argument("alert_confidence_threshold must be between 0.0 and 1.0");
    }

    if (consent_policy_version.empty()) {
        throw std::invalid_argument("consent_policy_version must not be empty");
    }
    if (routine_retention_days < 0 || routine_retention_days > 36500) {
        throw std::invalid_argument("routine_retention_days must be between 0 and 36500");
    }
    if (profile_retention_days < 0 || profile_retention_days > 36500) {
        throw std::invalid_argument("profile_retention_days must be between 0 and 36500");
    }
}

} // namespace homeguardian
