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
    
    if (j.contains("log_level")) {
        log_level = j["log_level"].get<std::string>();
    }
    if (j.contains("server_host")) {
        server_host = j["server_host"].get<std::string>();
    }
    if (j.contains("server_port")) {
        server_port = j["server_port"].get<int>();
    }
    if (j.contains("data_dir")) {
        data_dir = j["data_dir"].get<std::string>();
    }
    
    validate();
}

void Config::save(const std::string& filepath) const {
    validate();
    
    json j;
    j["log_level"] = log_level;
    j["server_host"] = server_host;
    j["server_port"] = server_port;
    j["data_dir"] = data_dir;
    
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
}

} // namespace homeguardian
