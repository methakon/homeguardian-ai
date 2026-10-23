#pragma once

// Per-device detector wiring for ingested observations (milestone D-03, section G).
//
// WHY: the D-02x review recorded an honest limitation - the new detectors were
// compiled and tested but NOT linked into the shipped hub runtime. This class is
// that correction: it owns the D-02x components the hub actually needs, keyed per
// device, and turns an accepted wire observation into the derived detections that
// flow into the SAME pipeline.
//
// WHAT IS WIRED, and only for the observation type it fits:
//   * `motion-changed-fraction` -> ActivityChangeDetector (activity level/trend)
//   * `audio-rms`               -> ActivityChangeDetector + AnomalyMonitor
//                                  (level AND statistical deviation; the anomaly
//                                  monitor is the existing one, reused)
//   * every accepted observation -> SensorDeduplicator (repeat suppression) and
//                                  DeviceAvailabilityTracker (liveness)
//   * `temperature`/`humidity`/`pressure` -> EnvironmentalAdapter (validated
//                                  environmental sample; correlation stays an
//                                  observation, never an alert)
//
// WHAT IS DELIBERATELY NOT WIRED: VectorDriftDetector and the full PrecursorSuite
// fusion. They need a bounded multi-modal feature VECTOR, and D-03 carries scalar
// observations; wiring them to a single scalar would manufacture a "regime shift"
// from one number. They stay available for D-05, where media-derived features are
// produced. This is a scope decision, not an omission - see
// docs/DISTRIBUTED_DEPLOYMENT.md.
//
// Every derived observation is ADVISORY in the default pipeline configuration
// (its detector id is not in DetectionFusionRule's evidence allowlist), so nothing
// here can change the existing alert behaviour. No detector result is ever
// hardcoded, and no derived observation can reach Severity::critical.

#include "core/ActivityDetectors.h"
#include "core/AnomalyMonitor.h"
#include "core/EnvironmentalCorrelation.h"
#include "core/Observation.h"
#include "core/SecurityObservation.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace homeguardian::net {

class IngestDetectors {
public:
    struct Config {
        size_t max_devices = 64;            // bounded per-device state
        size_t max_pending = 128;           // bounded derived-observation queue
        ActivityChangeDetector::Config motion_activity;
        ActivityChangeDetector::Config audio_activity;
        AnomalyMonitor::Config audio_anomaly;
        std::chrono::seconds dedup_cooldown{5};
        std::chrono::seconds offline_after{90};
        Config() = default;
    };

    struct Outcome {
        bool fed = false;
        std::string detector;      // which detector consumed it, empty if none
        bool duplicate = false;    // suppressed as a repeat
        std::string reason;        // bounded explanation
    };

    using Clock = std::function<int64_t()>;
    static Clock system_clock_ms();

    // NOTE: a nested Config with member initializers cannot be used directly as a
    // default argument on this compiler ("default member initializer required
    // before the end of its enclosing class"), so the default is a helper call
    // evaluated at the call site. Same fix as the project's earlier recurrence.
    static Config defaults();
    explicit IngestDetectors(Config cfg = defaults(), Clock clock = system_clock_ms());
    ~IngestDetectors();

    IngestDetectors(const IngestDetectors&) = delete;
    IngestDetectors& operator=(const IngestDetectors&) = delete;

    // Feed one ACCEPTED observation. Derived observations are queued; call
    // drain() to move them into the pipeline.
    Outcome observe(const SecurityObservation& observation);

    // Move queued derived observations into `out` (bounded).
    size_t drain(std::vector<Observation>& out);

    // Sweep liveness: a device that has gone quiet produces ONE availability
    // observation here (never a stream of them, and never a fabricated event on
    // reconnect).
    std::vector<SecurityObservation> sweep_availability();

    size_t device_count() const { return devices_.size(); }
    size_t derived_count() const { return derived_; }
    size_t duplicate_count() const { return duplicates_; }
    size_t queue_overflow_count() const { return overflow_; }
    void reset_counters();

private:
    struct DeviceState {
        ActivityChangeDetector motion{ActivityChangeDetector::defaults()};
        ActivityChangeDetector audio{ActivityChangeDetector::defaults()};
        AnomalyMonitor anomaly{AnomalyMonitor::defaults()};
        explicit DeviceState(ActivityChangeDetector::Config motion_cfg,
                             ActivityChangeDetector::Config audio_cfg,
                             AnomalyMonitor::Config anomaly_cfg)
            : motion(motion_cfg), audio(audio_cfg), anomaly(anomaly_cfg) {}
    };

    DeviceState* state_for(const std::string& device_id);
    void enqueue(Observation observation);

    Config cfg_;
    Clock clock_;
    std::map<std::string, std::unique_ptr<DeviceState>> devices_{};
    SensorDeduplicator dedup_{};
    DeviceAvailabilityTracker availability_{};
    std::vector<Observation> pending_{};
    size_t derived_ = 0;
    size_t duplicates_ = 0;
    size_t overflow_ = 0;
};

}  // namespace homeguardian::net
