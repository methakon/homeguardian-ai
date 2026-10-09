#pragma once

#include "Config.h"
#include <atomic>

namespace homeguardian {

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run(const std::string& config_path);
    void request_shutdown();

    static void signal_handler(int signal);

private:
    Config config_;
    std::atomic<bool> shutdown_flag_{false};
    static Application* instance_;
};

} // namespace homeguardian
