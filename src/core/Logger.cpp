#include "Logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace homeguardian {

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;

void Logger::initialize(const std::string& log_level) {
    if (logger_) {
        return;
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("homeguardian.log", true);

    std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
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
    
    spdlog::register_logger(logger_);
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
