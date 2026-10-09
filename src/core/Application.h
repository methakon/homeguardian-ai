#pragma once

#include "Config.h"
#include "Pipeline.h"
#include <atomic>
#include <memory>

namespace homeguardian {

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run(const std::string& config_path);
    void request_shutdown();
    
    void process_event(const Event& event);
    Pipeline& get_pipeline();

    static void signal_handler(int signal);

private:
    Config config_;
    std::atomic<bool> shutdown_flag_{false};
    std::unique_ptr<Pipeline> pipeline_;
    static Application* instance_;
};

} // namespace homeguardian
