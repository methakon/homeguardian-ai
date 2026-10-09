#pragma once

#include <string>

namespace homeguardian {

class Config {
public:
    std::string log_level = "info";
    std::string server_host = "127.0.0.1";
    int server_port = 8080;
    std::string data_dir = "/var/lib/homeguardian";

    void load(const std::string& filepath);
    void save(const std::string& filepath) const;

private:
    void validate() const;
};

} // namespace homeguardian
