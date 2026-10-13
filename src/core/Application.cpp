#include "Application.h"
#include "DetectionFusionRule.h"
#include "Logger.h"
#include "Observation.h"
#include "SyntheticActivityDetector.h"
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
    app_state_ = AppState::Constructed;

    // L-11: alerts are DETECTION-DRIVEN. The default rule set contains only the
    // detection/fusion rule: it reconstructs the detector evidence from the
    // inference events carried by the pipeline, fuses it, and raises an alert
    // only when the fused verdict is `detected` with sufficient confidence.
    // Raw observations alone never raise an alert — they are the detector's
    // INPUT. The previous independent, hardcoded "sim-presence" presence alert
    // has been removed.
    std::vector<std::shared_ptr<ICorrelationRule>> default_rules;
    detection_rule_ = std::make_shared<DetectionFusionRule>("detection-fusion");
    default_rules.push_back(detection_rule_);
    pipeline_ = std::make_unique<Pipeline>(default_rules);

    // Simulator runtime, built with capture DISABLED by default (config's
    // media_capture_enabled is applied in run()). It never opens real hardware.
    simulator_ = std::make_unique<SimulatorRuntime>(/*media_capture_enabled=*/false);
    simulator_->register_default_devices();

    // Alert deduplication/cooldown policy (conservative default 60s). Applied
    // in process_event(); does not suppress unrelated alerts.
    alert_policy_ = std::make_unique<AlertPolicy>(/*cooldown_ms=*/std::chrono::milliseconds(60000));
}

Application::~Application() {
    // Ensure any started component is torn down even on abnormal exit.
    if (simulator_) {
        simulator_->shutdown();
    }
    if (persistence_) {
        persistence_->close();
    }
    instance_ = nullptr;
}

int Application::run(const std::string& config_path) {
    app_state_ = AppState::Starting;
    status_.app_state = AppState::Starting;
    status_.startups.fetch_add(1);

    // --- Startup phase 1: config (fail-closed; invalid config => Failed) ---
    try {
        config_.load(config_path);
    } catch (const std::exception& e) {
        Logger::initialize("info");
        Logger::get()->critical("Failed to load config: {}", e.what());
        app_state_ = AppState::Failed;
        status_.app_state = AppState::Failed;
        return 1;
    }

    Logger::initialize(config_.log_level);
    Logger::get()->info("Starting HomeGuardian AI Application...");

    // --- Startup phase 2a: persistence (degraded on failure, not fatal) ---
    // Open the audit database. If it cannot be opened/initialized, run DEGRADED
    // (persistence_ == nullptr, status.persistence_healthy == false) rather than
    // failing the whole startup. Persistence is metadata-only; its absence does
    // not affect consent or capture safety.
    try {
        persistence_ = std::make_unique<PersistenceAudit>("homeguardian_audit.db");
        status_.persistence_healthy.store(true);
        Logger::get()->info("Persistence ready. historical_alerts={}",
                            persistence_->historical_alert_count());

        // --- Startup retention: prune alerts older than the configured window
        // once, before serving. alert_retention_days == 0 keeps records
        // forever (retention disabled). This bounds the audit DB on every
        // start; the main loop then re-prunes at most once per interval.
        if (config_.alert_retention_days > 0) {
            size_t pruned = persistence_->prune_older_than_age(
                std::chrono::hours(24) * config_.alert_retention_days);
            if (pruned > 0) {
                Logger::get()->info("Startup retention pruned {} old alert(s) "
                                    "(retention_days={})",
                                    pruned, config_.alert_retention_days);
            }
        }
    } catch (const std::exception& e) {
        persistence_.reset();
        status_.persistence_healthy.store(false);
        Logger::get()->warn("Persistence unavailable — running degraded: {}", e.what());
    }

    // --- Startup phase 2b: components (with cleanup on partial failure) ---
    try {
        pipeline_->set_max_history_size(config_.max_event_history);

        // Apply the global capture kill switch from config. Simulator mode
        // honors it exactly like a real backend would: when false, NO media is
        // delivered. Configuration cannot silently grant consent.
        simulator_->set_media_capture_enabled(config_.media_capture_enabled);
        status_.capture_enabled.store(config_.media_capture_enabled);
    } catch (const std::exception& e) {
        // Partial startup failure: tear down anything already started so the
        // runtime never reports healthy while components are half-initialized.
        Logger::get()->critical("Startup failed: {} — cleaning up", e.what());
        simulator_->shutdown();
        if (persistence_) persistence_->close();
        app_state_ = AppState::Failed;
        status_.app_state = AppState::Failed;
        return 1;
    }

    std::signal(SIGINT, Application::signal_handler);
    std::signal(SIGTERM, Application::signal_handler);

    Logger::get()->info("Simulator mode: capture_enabled={} consumers={} state={}",
                        config_.media_capture_enabled, simulator_->consumer_count(),
                        app_state_to_string(app_state_.load()));

    // --- Simulator demonstration: bounded, deterministic ---
    // When capture is enabled AND synthetic consent is present, run a bounded
    // set of media cycles exercising consent, fan-out, pipeline, alert policy,
    // durable audit, bounded speaker, and clean shutdown. When disabled
    // (default), no media.
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
            status_.media_authorized.fetch_add(static_cast<uint64_t>(delivered));
            status_.speaker_queue_depth.store(simulator_->speaker_queue_size());
            status_.speaker_played.store(simulator_->speaker_played_count());
            // L-11: detection runs because an AUTHORIZED acquisition occurred.
            // The resulting inference event flows through the SAME pipeline,
            // where the detection/fusion rule decides whether to alert. No
            // acquisition => no detection => no detection-driven alert.
            if (delivered > 0) run_detection_step(i);
            Logger::get()->info("Simulator cycle {}: delivered={} blocked={} "
                                "speaker_played={} pipeline_history={} "
                                "alerts_delivered={} alerts_suppressed={}",
                                i, delivered, simulator_->last_fanout_blocked(),
                                simulator_->speaker_played_count(),
                                pipeline_->get_recent_events(std::chrono::hours(1)).size(),
                                alert_policy_->delivered_count(),
                                alert_policy_->suppressed_count());
        }
        Logger::get()->info("Simulator cycles complete. speaker_played={} queue={} "
                            "pipeline_history={} alerts_delivered={} alerts_suppressed={}",
                            simulator_->speaker_played_count(),
                            simulator_->speaker_queue_size(),
                            pipeline_->get_recent_events(std::chrono::hours(1)).size(),
                            alert_policy_->delivered_count(),
                            alert_policy_->suppressed_count());
    } else {
        Logger::get()->info("Simulator idle: capture disabled; no media delivered.");
    }

    // --- Main loop ---
    app_state_ = AppState::Running;
    status_.app_state = AppState::Running;
    Logger::get()->info("Entering main loop... status: {}", status_.summary());

    // Bounded periodic retention: re-prune at most once per this interval, not
    // on every 100ms tick (avoids excessive DB writes). alert_retention_days ==
    // 0 disables periodic pruning (keep forever).
    constexpr auto kRetentionInterval = std::chrono::minutes(10);
    while (!shutdown_flag_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (persistence_ && config_.alert_retention_days > 0) {
            size_t pruned = persistence_->maybe_prune_retention(
                std::chrono::hours(24) * config_.alert_retention_days, kRetentionInterval);
            if (pruned > 0) {
                Logger::get()->info("Periodic retention pruned {} old alert(s)", pruned);
            }
        }
    }

    // --- Deterministic shutdown (idempotent) ---
    app_state_ = AppState::Stopping;
    status_.app_state = AppState::Stopping;
    simulator_->shutdown();       // stops capture, clears bounded queue, releases speaker
    alert_policy_->reset();
    if (persistence_) persistence_->close();
    app_state_ = AppState::Stopped;
    status_.app_state = AppState::Stopped;
    status_.clean_shutdowns.fetch_add(1);

    Logger::get()->info("Main loop exited cleanly. final_state={} status: {}",
                        app_state_to_string(app_state_.load()), status_.summary());
    Logger::get()->flush();
    return 0;
}

void Application::run_detection_step(int index) {
    // Detection is driven by an AUTHORIZED acquisition: this is only called when
    // a simulator tick actually delivered media. The synthetic detector INPUT is
    // deterministic so the standalone demo is reproducible; the detector, the
    // fusion engine and the rule are the real implementations.
    static const double kEnergies[] = {0.8, 0.75, 0.52, 0.82, 0.78};
    constexpr size_t kN = sizeof(kEnergies) / sizeof(kEnergies[0]);
    const size_t idx = static_cast<size_t>(index) % kN;
    SyntheticActivityDetector detector;
    Observation o = detector.detect("app-det-obs-" + std::to_string(index),
                                    kEnergies[idx]);
    o.validate();
    process_event(o.to_event());
}

void Application::request_shutdown() {
    shutdown_flag_ = true;
    if (app_state_.load() == AppState::Running) {
        app_state_ = AppState::Stopping;
        status_.app_state = AppState::Stopping;
    }
    if (auto logger = Logger::get()) {
        logger->info("Shutdown requested.");
    }
}

void Application::process_event(const Event& event) {
    status_.events_processed.fetch_add(1);
    if (!pipeline_) return;
    std::optional<Alert> alert;
    try {
        alert = pipeline_->process(event);
    } catch (const std::exception& e) {
        // Processing failure: count it, keep running, do not corrupt state.
        status_.events_failed.fetch_add(1);
        Logger::get()->warn("Event processing failed: {}", e.what());
        return;
    }
    if (alert) {
        status_.alerts_generated.fetch_add(1);
        // Apply the deduplication/cooldown policy. Unrelated alerts (different
        // rule/severity) are delivered independently; repeated duplicates for
        // the same active condition are suppressed per the documented policy.
        bool suppressed = false;
        bool deliver = alert_policy_->admit(*alert, suppressed);
        if (deliver) {
            status_.alerts_delivered.fetch_add(1);
            Logger::get()->warn("Alert generated: {}", alert->message);
            // Durable audit (metadata only). A write failure degrades status
            // but never throws or corrupts runtime state.
            if (persistence_) {
                if (!persistence_->record_alert(*alert)) {
                    status_.alerts_persist_failed.fetch_add(1);
                    status_.persistence_healthy.store(false);
                }
            }
            // Optional passive observer: hand the DELIVERED alert to the sink
            // (e.g. the demo scenario runner captures the real alert so it — and
            // only it — is persisted/shown). Never changes delivery.
            if (alert_sink_) alert_sink_(*alert);
        } else if (suppressed) {
            status_.alerts_suppressed.fetch_add(1);
            Logger::get()->info("Alert suppressed (dedup/cooldown): {}", alert->message);
        }
    } else {
        // No alert this cycle: the rule's condition may have cleared. Re-arm
        // EVERY registered rule so each rule's next alert is treated as a
        // fresh condition. We clear per registered rule name (derived from the
        // pipeline) rather than a hardcoded name, so adding rules needs no
        // change here. mark_condition_cleared only affects that rule's own
        // cooldown state; unrelated active conditions on other rules are
        // re-armed the same way and each is independently re-delivered on its
        // next firing.
        for (const auto& rule_name : pipeline_->rule_names()) {
            alert_policy_->mark_condition_cleared(rule_name);
        }
    }
}

Pipeline& Application::get_pipeline() {
    return *pipeline_;
}

SimulatorRuntime& Application::get_simulator_runtime() {
    return *simulator_;
}

AlertPolicy& Application::get_alert_policy() {
    return *alert_policy_;
}

PersistenceAudit* Application::get_persistence() {
    return persistence_.get();
}

RuntimeStatus& Application::get_status() {
    return status_;
}

void Application::set_alert_sink(std::function<void(const Alert&)> sink) {
    alert_sink_ = std::move(sink);
}

std::vector<std::string> Application::evidence_detector_ids() const {
    return detection_rule_ ? detection_rule_->evidence_detector_ids()
                           : std::vector<std::string>{};
}

void Application::set_evidence_detectors(std::vector<std::string> ids) {
    if (detection_rule_) detection_rule_->set_evidence_detector_ids(std::move(ids));
}

AppState Application::state() const {
    return app_state_.load();
}

bool Application::is_healthy() const {
    AppState s = app_state_.load();
    // Healthy only while cleanly running or stopping; never while Failed.
    return s == AppState::Running || s == AppState::Stopping;
}

void Application::signal_handler([[maybe_unused]] int signal) {
    if (instance_) {
        instance_->request_shutdown();
    }
}

} // namespace homeguardian
