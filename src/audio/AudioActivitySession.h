#pragma once

// The audio activity session: real PCM in, ONE explainable finding out.
//
// WHAT IT DOES, IN ORDER (the milestone's section I workflow):
//   1. take a window of REAL PCM,
//   2. compute bounded signal features (BoundedFeatures: rms, peak, zero-crossing rate),
//   3. keep a learned baseline of the level,
//   4. detect a sustained deviation from that baseline (ActivityChangeDetector, and
//      AnomalyMonitor for the statistical deviation),
//   5. distinguish silence from a microphone that stopped delivering samples,
//   6. produce ONE finding, with its provenance and its uncertainty.
//
// WHAT IT REFUSES TO SAY
// ----------------------
// The vocabulary is CLOSED and deliberately boring: "an increase in ambient sound
// activity", "within the established baseline", "a sustained change", "the microphone
// stopped delivering samples", "insufficient evidence to classify". There is no species,
// speech, emotion, dog, rat, bird, intruder, fall, fire or earthquake claim anywhere in
// this file, and no code path can emit one - audio energy is energy.
//
// PROVENANCE IS CARRIED, NOT GUESSED: a finding computed from real device PCM is
// `Provenance::real`; from a fixture it is `synthetic`. A synthetic window can never be
// reported as real, which is what stops a demo from being mistaken for a measurement.
//
// CONSENT: the session does NOT check consent itself. It is fed by a caller that has
// already passed the ConsentGuardedDevice delivery gate, and it re-checks nothing, so
// there is exactly ONE place where authorization is decided.

#include "core/ActivityDetectors.h"
#include "core/AnomalyMonitor.h"
#include "core/BoundedFeatures.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {

enum class AudioFindingKind {
    warming_up,           // not enough evidence for a baseline yet
    quiet_baseline,       // activity is within the established baseline
    sustained_increase,   // a sustained rise above the baseline
    sustained_decrease,   // a sustained fall below the baseline
    silence,              // the room is quiet, and the microphone is delivering samples
    microphone_stalled,   // no samples arrived at all: the device stopped delivering
    insufficient_evidence // samples arrived but cannot be classified
};

inline const char* audio_finding_kind_to_string(AudioFindingKind kind) {
    switch (kind) {
        case AudioFindingKind::warming_up: return "warming_up";
        case AudioFindingKind::quiet_baseline: return "quiet_baseline";
        case AudioFindingKind::sustained_increase: return "sustained_increase";
        case AudioFindingKind::sustained_decrease: return "sustained_decrease";
        case AudioFindingKind::silence: return "silence";
        case AudioFindingKind::microphone_stalled: return "microphone_stalled";
        case AudioFindingKind::insufficient_evidence: return "insufficient_evidence";
    }
    return "unknown";
}

// The full, auditable result of one window.
struct AudioFinding {
    AudioFindingKind kind = AudioFindingKind::warming_up;
    bool valid = false;                  // the window had usable samples
    std::string reason;                  // why it is not valid / why it is inconclusive
    // The sentence shown in the dashboard and (optionally) spoken. Fixed vocabulary, no
    // inference beyond "activity changed relative to this room's own baseline".
    std::string sentence;
    // Where the samples came from. NEVER derived from the payload.
    bool real_source = false;
    double confidence = 0.0;             // 0 when the evidence is insufficient
    std::string uncertainty;             // plain-language statement of what is unknown

    AudioFrameFeatures features{};
    bool baseline_ready = false;
    double baseline_level = 0.0;
    double recent_level = 0.0;
    double ratio = 1.0;
    bool sustained = false;              // the change held for the confirmation window
    bool statistical_deviation = false;  // AnomalyMonitor flagged the level
    uint64_t windows_observed = 0;
    uint64_t silent_windows = 0;
};

class AudioActivitySession {
public:
    struct Config {
        uint32_t sample_rate = 48000;
        uint32_t channels = 1;
        // A window is ~100 ms at 48 kHz: small enough to be responsive, large enough for a
        // meaningful RMS, and bounded by BoundedFeatures::kMaxAudioFrames.
        size_t window_frames = 4800;
        // Below this rms the window counts as silence rather than activity.
        double silence_rms = 0.002;
        // How many consecutive empty reads before the microphone is called stalled.
        int stalled_after_empty_reads = 3;
        // Confirm a change only if it holds for this many windows.
        int confirmation_windows = 3;
    };

    struct Clock {
        // Injectable so the whole session is deterministic in tests.
        using Fn = std::function<int64_t()>;
    };

    AudioActivitySession(Config config, std::function<int64_t()> clock);

    // Feed one window of interleaved PCM. `real_source` states where the samples came
    // from; a caller that cannot vouch for them passes false.
    AudioFinding observe(const int16_t* pcm, size_t frames, bool real_source);

    // Tell the session that a read returned NO samples. Consecutive empty reads become
    // `microphone_stalled` - a device that stopped delivering must not be reported as a
    // quiet room, which is the single most misleading thing this could say.
    AudioFinding observe_no_samples();

    void reset();
    const Config& config() const { return config_; }
    uint64_t observed_windows() const { return observed_windows_; }

private:
    AudioFinding build_finding(AudioFindingKind kind, const std::string& phrase, bool real_source) const;

    Config config_;
    std::function<int64_t()> clock_;
    ActivityChangeDetector change_detector_;
    AnomalyMonitor anomaly_monitor_;
    int consecutive_empty_reads_ = 0;
    int consecutive_rise_ = 0;
    int consecutive_fall_ = 0;
    uint64_t observed_windows_ = 0;
    uint64_t silent_windows_ = 0;
    double last_level_ = 0.0;
};

}  // namespace homeguardian
