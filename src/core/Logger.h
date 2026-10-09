#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>
#include <string>
#include <memory>

namespace homeguardian {

class Logger {
public:
    static void initialize(const std::string& log_level);
    static std::shared_ptr<spdlog::logger> get();
    static void reset();

private:
    static std::shared_ptr<spdlog::logger> logger_;
};

} // namespace homeguardian
