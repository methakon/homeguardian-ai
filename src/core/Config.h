#pragma once

#include <string>

namespace homeguardian {

class Config {
public:
    std::string log_level = "info";
    std::string server_host = "127.0.0.1";
    int server_port = 8080;
    std::string data_dir = "/var/lib/homeguardian";
    
    // New fields
    size_t max_event_history = 1000;
    int correlation_window_ms = 5000;
    double alert_confidence_threshold = 0.5;

    // Phase E — family profiles, consent, routines. Safe defaults only.
    // Media capture and external/cloud processing are OFF by default.
    bool media_capture_enabled = false;      // no camera/audio capture by default
    bool cloud_processing_enabled = false;   // no external upload by default
    std::string consent_policy_version = "1.0.0"; // policy label applied to new consent records
    int routine_retention_days = 730;        // retention for routine/consent history (days)
    int profile_retention_days = 1825;       // retention for profiles (days; 0 = keep until explicit deletion)

    void load(const std::string& filepath);
    void save(const std::string& filepath) const;

private:
    void validate() const;
};

} // namespace homeguardian
