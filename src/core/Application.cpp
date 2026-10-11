#include "Application.h"
#include "Logger.h"
#include "TimeWindowCorrelation.h"
#include <thread>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <csignal>

namespace homeguardian {

Application* Application::instance_ = nullptr;

Application::Application() {
    if (instance_ != nullptr) {
        throw std::runtime_error("Application instance already exists");
    }
    instance_ = this;
    
    // Existing time-window correlation rule: fires a synthetic info alert once
    // >=2 observations arrive within the window. This is the app's existing
    // alert engine — reused unchanged; the simulator feeds it authorized
    // observations only. It is a SYNTHETIC test alert, never a real-world
    // danger claim.
    std::vector<std::shared_ptr<ICorrelationRule>> default_rules;
    default_rules.push_back(std::make_shared<TimeWindowCorrelation>(
        "sim-presence", std::vector<EventType>{EventType::observation},
        /*min_count=*/2, std::chrono::milliseconds(60000),
        Severity::info, "synthetic simulator presence notice"));
    pipeline_ = std::make_unique<Pipeline>(default_rules);
    // Simulator runtime, built with capture DISABLED by default (config's
    // media_capture_enabled is applied in run()). It never opens real hardware.
    simulator_ = std::make_unique<SimulatorRuntime>(/*media_capture_enabled=*/false);
    simulator_->register_default_devices();
}

Application::~Application() {
    instance_ = nullptr;
}

int Application::run(const std::string& config_path) {
    try {
        config_.load(config_path);
    } catch (const std::exception& e) {
        Logger::initialize("info");
        Logger::get()->critical("Failed to load config: {}", e.what());
        return 1;
    }

    Logger::initialize(config_.log_level);
    Logger::get()->info("Starting HomeGuardian AI Application...");

    pipeline_->set_max_history_size(config_.max_event_history);

    // Apply the global capture kill switch from config. Simulator mode honors it
    // exactly like a real backend would: when false, NO media is delivered.
    simulator_->set_media_capture_enabled(config_.media_capture_enabled);

    std::signal(SIGINT, Application::signal_handler);
    std::signal(SIGTERM, Application::signal_handler);

    Logger::get()->info("Simulator mode: capture_enabled={} consumers={}",
                        config_.media_capture_enabled, simulator_->consumer_count());

    // Simulator demonstration: when capture is enabled AND synthetic consent is
    // present, run a bounded, deterministic set of media cycles so the running
    // application exercises the consent gate, shared fan-out, bounded speaker
    // queue, and clean shutdown. When capture is disabled (the default), no
    // media is generated or delivered.
    if (config_.media_capture_enabled) {
        // Synthetic grants for both consumers (simulator test consent only).
        simulator_->grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        simulator_->grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");

        // Route every AUTHORIZED synthetic observation through the real
        // application event path: process_event() -> Pipeline validation +
        // correlation + alert. Denied/withdrawn consumers never reach this sink.
        simulator_->set_event_sink([this](const Event& ev) { this->process_event(ev); });

        simulator_->start();

        const int kSimulatorCycles = 5;
        for (int i = 0; i < kSimulatorCycles && !shutdown_flag_; ++i) {
            int delivered = simulator_->tick();
            Logger::get()->info("Simulator cycle {}: delivered={} blocked={} "
                                "speaker_played={} pipeline_history={}",
                                i, delivered, simulator_->last_fanout_blocked(),
                                simulator_->speaker_played_count(),
                                pipeline_->get_recent_events(std::chrono::hours(1)).size());
        }
        Logger::get()->info("Simulator cycles complete. speaker_played={} queue={} "
                            "pipeline_history={}",
                            simulator_->speaker_played_count(),
                            simulator_->speaker_queue_size(),
                            pipeline_->get_recent_events(std::chrono::hours(1)).size());
    } else {
        Logger::get()->info("Simulator idle: capture disabled; no media delivered.");
    }

    Logger::get()->info("Entering main loop...");

    while (!shutdown_flag_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Deterministic shutdown of the simulator runtime (stops capture, clears the
    // bounded queue, releases the speaker). Idempotent.
    simulator_->shutdown();

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

void Application::process_event(const Event& event) {
    if (pipeline_) {
        auto alert = pipeline_->process(event);
        if (alert) {
            Logger::get()->warn("Alert generated: {}", alert->message);
        }
    }
}

Pipeline& Application::get_pipeline() {
    return *pipeline_;
}

SimulatorRuntime& Application::get_simulator_runtime() {
    return *simulator_;
}

void Application::signal_handler([[maybe_unused]] int signal) {
    if (instance_) {
        instance_->request_shutdown();
    }
}

} // namespace homeguardian
