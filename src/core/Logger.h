#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>
#include <string>
#include <memory>

namespace homeguardian {

class Logger {
public:
    static void initialize(const std::string& log_level, const std::string& log_file_path = "");
    static std::shared_ptr<spdlog::logger> get();
    static void reset();
    static std::string current_log_path();

private:
    static std::shared_ptr<spdlog::logger> logger_;
    static std::string current_log_path_;
};

} // namespace homeguardian
