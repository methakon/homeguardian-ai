#include "Application.h"
#include "Logger.h"
#include <thread>
#include <chrono>
#include <stdexcept>
#include <csignal>

namespace homeguardian {

Application* Application::instance_ = nullptr;

Application::Application() {
    if (instance_ != nullptr) {
        throw std::runtime_error("Application instance already exists");
    }
    instance_ = this;
}

Application::~Application() {
    instance_ = nullptr;
}

int Application::run(const std::string& config_path) {
    try {
        config_.load(config_path);
    } catch (const std::exception& e) {
        // Since logger might not be initialized yet, initialize with default info
        Logger::initialize("info");
        Logger::get()->critical("Failed to load config: {}", e.what());
        return 1;
    }

    Logger::initialize(config_.log_level);
    Logger::get()->info("Starting HomeGuardian AI Application...");
    Logger::get()->info("Server Host: {}", config_.server_host);
    Logger::get()->info("Server Port: {}", config_.server_port);
    Logger::get()->info("Data Directory: {}", config_.data_dir);

    // Register signal handlers
    std::signal(SIGINT, Application::signal_handler);
    std::signal(SIGTERM, Application::signal_handler);

    Logger::get()->info("Entering main loop...");

    while (!shutdown_flag_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Main loop logic would go here
    }

    Logger::get()->info("Main loop exited cleanly.");
    Logger::get()->flush();
    return 0;
}

void Application::request_shutdown() {
    shutdown_flag_ = true;
    if (auto logger = Logger::get()) {
        logger->info("Shutdown requested.");
    }
}

void Application::signal_handler([[maybe_unused]] int signal) {
    if (instance_) {
        instance_->request_shutdown();
    }
}

} // namespace homeguardian
