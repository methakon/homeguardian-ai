#include "audio/AudioActivitySession.h"

#include <cmath>
#include <stdexcept>

namespace homeguardian {

namespace {

constexpr double kPeakProbabilityThreshold = 0.02;  // below this, the window is treated as quiet

std::chrono::system_clock::time_point from_ms(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

}  // namespace

namespace {

ActivityChangeDetector::Config change_config_for(const AudioActivitySession::Config& config) {
    ActivityChangeDetector::Config change;
    change.recent_window = 4;
    change.warmup = 4;
    change.confirm_samples =
        static_cast<size_t>(config.confirmation_windows > 0 ? config.confirmation_windows : 3);
    return change;
}

AnomalyMonitor::Config anomaly_config_for(const AudioActivitySession::Config&) {
    AnomalyMonitor::Config anomaly;
    anomaly.min_samples = 6;
    anomaly.max_window = 32;
    anomaly.zscore_threshold = 2.0;
    return anomaly;
}

}  // namespace

// NOTE: the detectors are constructed in the INITIALIZER LIST, not assigned in the body:
// AnomalyMonitor holds a std::mutex, so it is not move-assignable, and an assignment here
// does not compile (which is a good thing - a monitor half-configured in a body would be a
// silent state bug).
AudioActivitySession::AudioActivitySession(Config config, std::function<int64_t()> clock)
    : config_(std::move(config)),
      clock_(std::move(clock)),
      change_detector_(change_config_for(config_)),
      anomaly_monitor_(anomaly_config_for(config_)) {}

void AudioActivitySession::reset() {
    change_detector_.reset();
    anomaly_monitor_.reset();
    consecutive_empty_reads_ = 0;
    consecutive_rise_ = 0;
    consecutive_fall_ = 0;
    observed_windows_ = 0;
    silent_windows_ = 0;
    last_level_ = 0.0;
}

AudioFinding AudioActivitySession::build_finding(AudioFindingKind kind, const std::string& phrase,
                                                 bool real_source) const {
    AudioFinding finding;
    finding.kind = kind;
    finding.sentence = phrase;
    finding.real_source = real_source;
    finding.windows_observed = observed_windows_;
    finding.silent_windows = silent_windows_;
    return finding;
}

AudioFinding AudioActivitySession::observe_no_samples() {
    ++consecutive_empty_reads_;
    const int64_t now_ms = clock_ ? clock_() : 0;

    AudioFinding finding;
    if (consecutive_empty_reads_ >= (config_.stalled_after_empty_windows > 0
                                         ? config_.stalled_after_empty_windows
                                         : 50)) {
        // A device that stopped delivering samples is NOT a quiet room, and the difference
        // matters: one is a finding about the world, the other about our own equipment.
        finding = build_finding(AudioFindingKind::microphone_stalled,
                                "The microphone has delivered no samples for several seconds.",
                                false);
        finding.valid = false;
        // State the FACT and the bound, not a diagnosis of the hardware. The device may be
        // suspended by the sound server, released by the OS, or genuinely gone; this component
        // can only report that nothing arrived, and for how long.
        finding.reason = "no samples arrived for " +
                         std::to_string(consecutive_empty_reads_) +
                         " consecutive windows (the capture child may have exited, or the sound "
                         "server may have suspended the source)";
        finding.uncertainty =
            "Whether anything was happening in the room is unknown: no audio reached the hub.";
        finding.real_source = false;  // nothing was measured, so no provenance is claimed
        return finding;
    }
    finding = build_finding(AudioFindingKind::insufficient_evidence,
                            "The available evidence is insufficient to classify the sound.",
                            false);
    finding.valid = false;
    finding.reason = "no samples arrived for this window";
    finding.uncertainty = "A single empty window can be a scheduling stall rather than a "
                          "device problem, so no device conclusion is drawn yet.";
    (void)now_ms;
    return finding;
}

AudioFinding AudioActivitySession::observe(const int16_t* pcm, size_t frames, bool real_source) {
    consecutive_empty_reads_ = 0;
    ++observed_windows_;

    AudioFinding finding;
    finding.real_source = real_source;
    finding.windows_observed = observed_windows_;

    const AudioFrameFeatures features =
        extract_audio_features(pcm, frames, config_.channels, config_.sample_rate);
    finding.features = features;
    if (!features.valid) {
        // Refused by the bounded extractor (too few frames, wrong geometry). Report why
        // rather than publishing a statistic computed from noise.
        finding = build_finding(AudioFindingKind::insufficient_evidence,
                                "The available evidence is insufficient to classify the sound.",
                                real_source);
        finding.valid = false;
        finding.reason = features.reason.empty() ? "audio features were not extractable"
                                                 : features.reason;
        finding.uncertainty = "The window was not usable, so no level comparison was made.";
        finding.features = features;
        return finding;
    }
    finding.valid = true;

    const double level = features.rms;
    last_level_ = level;

    // Baseline + trend (level view).
    const auto now = from_ms(clock_ ? clock_() : 0);
    const auto change = change_detector_.observe(level, now);
    finding.baseline_ready = change.baseline_ready;
    finding.baseline_level = change.baseline_level;
    finding.recent_level = change.recent_level;
    finding.ratio = change.ratio;
    finding.sustained = change.confirmed;

    // Statistical deviation (z-score view) - a SEPARATE question from the trend, which is
    // why the existing AnomalyMonitor is reused instead of re-deriving a threshold here.
    const AnomalyResult anomaly = anomaly_monitor_.observe(level, now);
    finding.statistical_deviation = anomaly.anomalous;

    const bool quiet_window = features.rms <= config_.silence_rms &&
                              features.peak <= kPeakProbabilityThreshold;
    if (quiet_window) ++silent_windows_;

    if (!change.baseline_ready) {
        finding = build_finding(AudioFindingKind::warming_up,
                                "Learning the room's sound baseline; no conclusion yet.",
                                real_source);
        finding.valid = true;
        finding.baseline_ready = false;
        finding.baseline_level = change.baseline_level;
        finding.recent_level = change.recent_level;
        finding.ratio = change.ratio;
        finding.confidence = 0.0;
        finding.uncertainty = "The baseline is not established yet, so any change would be "
                             "indistinguishable from normal variation.";
        finding.features = features;
        return finding;
    }

    if (quiet_window) {
        AudioFinding quiet = build_finding(AudioFindingKind::silence,
                                           "Ambient activity is within the established baseline.",
                                           real_source);
        quiet.valid = true;
        quiet.baseline_ready = true;
        quiet.baseline_level = change.baseline_level;
        quiet.recent_level = change.recent_level;
        quiet.ratio = change.ratio;
        quiet.sustained = change.confirmed;
        quiet.confidence = 0.5;
        quiet.uncertainty = "The room is quiet; this says nothing about whether a quiet sound "
                            "was present below the microphone's sensitivity.";
        quiet.features = features;
        return quiet;
    }

    if (change.confirmed && change.direction == ActivityChangeDetector::Direction::rising) {
        AudioFinding rise = build_finding(
            AudioFindingKind::sustained_increase,
            "HomeGuardian detected an increase in ambient sound activity. The source is not "
            "identified.",
            real_source);
        rise.valid = true;
        rise.baseline_ready = true;
        rise.baseline_level = change.baseline_level;
        rise.recent_level = change.recent_level;
        rise.ratio = change.ratio;
        rise.sustained = true;
        rise.statistical_deviation = anomaly.anomalous;
        rise.confidence = anomaly.anomalous ? 0.8 : 0.6;
        rise.uncertainty =
            "What produced the sound is not identified: no species, speech, person or event "
            "classification is performed or implied.";
        rise.features = features;
        return rise;
    }

    if (change.confirmed && change.direction == ActivityChangeDetector::Direction::falling) {
        AudioFinding fall = build_finding(
            AudioFindingKind::sustained_decrease,
            "A sustained reduction in sound activity was detected.", real_source);
        fall.valid = true;
        fall.baseline_ready = true;
        fall.baseline_level = change.baseline_level;
        fall.recent_level = change.recent_level;
        fall.ratio = change.ratio;
        fall.sustained = true;
        fall.confidence = 0.6;
        fall.uncertainty = "A quiet period is not evidence that anything is wrong or right.";
        fall.features = features;
        return fall;
    }

    AudioFinding stable = build_finding(AudioFindingKind::quiet_baseline,
                                        "Ambient activity is within the established baseline.",
                                        real_source);
    stable.valid = true;
    stable.baseline_ready = true;
    stable.baseline_level = change.baseline_level;
    stable.recent_level = change.recent_level;
    stable.ratio = change.ratio;
    stable.sustained = change.confirmed;
    stable.statistical_deviation = anomaly.anomalous;
    stable.confidence = anomaly.anomalous ? 0.5 : 0.4;
    stable.uncertainty = "A single window above the baseline is not treated as a change until "
                         "it is sustained.";
    stable.features = features;
    return stable;
}

}  // namespace homeguardian
