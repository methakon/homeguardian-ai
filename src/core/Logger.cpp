#include "Logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>
#include <cstdlib>

namespace homeguardian {

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
std::string Logger::current_log_path_ = "homeguardian.log";

std::string Logger::current_log_path() {
    return current_log_path_;
}

void Logger::initialize(const std::string& log_level, const std::string& log_file_path) {
    if (logger_) {
        return;
    }

    std::string path = log_file_path;
    if (path.empty()) {
        if (const char* env_path = std::getenv("HG_LOG_PATH")) {
            if (*env_path != '\0') {
                path = env_path;
            }
        }
    }
    if (path.empty()) {
        path = "homeguardian.log";
    }
    current_log_path_ = path;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    // The file sink is BEST-EFFORT. A read-only, full or missing working
    // directory (or an unwritable log file) must not abort the process:
    // logging is diagnostic, and neither consent enforcement nor capture gating
    // depends on it. Previously the spdlog exception escaped here and
    // std::terminate killed the process (SIGABRT) before it could fail closed.
    std::vector<spdlog::sink_ptr> sinks {console_sink};
    std::string file_sink_error;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            current_log_path_, true));
    } catch (const std::exception& e) {
        file_sink_error = e.what();
    }

    logger_ = std::make_shared<spdlog::logger>("homeguardian", sinks.begin(), sinks.end());

    spdlog::level::level_enum level = spdlog::level::info;
    if (log_level == "trace") level = spdlog::level::trace;
    else if (log_level == "debug") level = spdlog::level::debug;
    else if (log_level == "info") level = spdlog::level::info;
    else if (log_level == "warn") level = spdlog::level::warn;
    else if (log_level == "error") level = spdlog::level::err;
    else if (log_level == "critical") level = spdlog::level::critical;

    logger_->set_level(level);
    logger_->flush_on(spdlog::level::warn);

    if (!file_sink_error.empty()) {
        // Reported at whatever level the caller configured; never fatal.
        logger_->warn("file logging unavailable ({}); console logging only",
                      file_sink_error);
    }

    try {
        spdlog::register_logger(logger_);
    } catch (const std::exception&) {
        // A duplicate name in the spdlog registry is not fatal: logger_ itself
        // is valid and Logger::get() serves it.
    }
}

std::shared_ptr<spdlog::logger> Logger::get() {
    if (!logger_) {
        throw std::runtime_error("Logger not initialized. Call Logger::initialize() first.");
    }
    return logger_;
}

void Logger::reset() {
    if (logger_) {
        spdlog::drop(logger_->name());
    }
    logger_.reset();
}

} // namespace homeguardian
