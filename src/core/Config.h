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

    void load(const std::string& filepath);
    void save(const std::string& filepath) const;

private:
    void validate() const;
};

} // namespace homeguardian
