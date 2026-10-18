#pragma once

// The camera activity session: REAL luma frames in, ONE explainable finding out.
//
// WHY THIS EXISTS
// ---------------
// The audio path already turns a real microphone into bounded scalars and one honest finding
// (AudioActivitySession). This is the camera equivalent, built on the SAME pieces: the luma
// plane from a real V4L2 frame goes through the existing `extract_motion_features`, its
// `changed_fraction` becomes the activity level fed to the existing `ActivityChangeDetector`,
// and the result is reported through the existing Observation model with its provenance.
// There is no second detector, no second feature extractor and no second event engine.
//
// WHAT IT REFUSES TO SAY
// ----------------------
// The vocabulary is CLOSED and deliberately boring: "no previous frame yet", "motion within
// the established baseline", "a sustained increase in motion", "a sustained decrease in
// motion", "the camera delivered no frame", "insufficient evidence to classify". There is no
// person, animal, species, face, gesture, fall, intruder or emergency claim anywhere in this
// file, and no code path can emit one - motion is motion.
//
// PROVENANCE IS CARRIED, NOT GUESSED
// ----------------------------------
// A finding computed from a real device frame is `Provenance::real`; from a fixture it is
// `synthetic`. The caller states which, and this class never upgrades it: a synthetic frame
// cannot be reported as real, which is what stops a rehearsal from being mistaken for a
// measurement.
//
// PRIVACY
// -------
// Frames are consumed, not stored: only the previous luma plane is retained (one bounded
// buffer, so the next frame has something to compare against), and nothing is written,
// queued or transmitted. The chroma a colour frame carries is never copied.

#include "core/ActivityDetectors.h"
#include "core/BoundedFeatures.h"
#include "core/Observation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {

enum class CameraFindingKind {
    warming_up,             // no baseline yet
    quiet_baseline,         // motion within the established baseline
    sustained_increase,     // a confirmed rise in motion
    sustained_decrease,     // a confirmed fall in motion
    frames_stalled,         // the DEVICE delivered nothing - our equipment, not the world
    insufficient_evidence   // not enough valid data to decide
};

inline const char* camera_finding_kind_to_string(CameraFindingKind k) {
    switch (k) {
        case CameraFindingKind::warming_up:           return "warming_up";
        case CameraFindingKind::quiet_baseline:       return "quiet_baseline";
        case CameraFindingKind::sustained_increase:   return "sustained_increase";
        case CameraFindingKind::sustained_decrease:   return "sustained_decrease";
        case CameraFindingKind::frames_stalled:       return "frames_stalled";
        case CameraFindingKind::insufficient_evidence:return "insufficient_evidence";
    }
    return "insufficient_evidence";
}

struct CameraFinding {
    bool valid = false;
    std::string reason;
    CameraFindingKind kind = CameraFindingKind::insufficient_evidence;
    std::string sentence;            // the closed, boring vocabulary
    std::string uncertainty;         // what this finding explicitly does NOT mean
    bool real_source = false;        // provenance, carried from the caller

    double motion_level = 0.0;       // latest window's changed_fraction [0,1]
    double mean_delta = 0.0;         // mean |luma delta| / 255
    double baseline = 0.0;           // the detector's learned baseline
    double ratio = 1.0;
    size_t windows_observed = 0;
    size_t stalled_windows = 0;
};

class CameraActivitySession {
public:
    struct Config {
        size_t warmup = 5;
        size_t recent_window = 4;
        size_t history = 32;
        double slow_alpha = 0.05;
        double confirm_ratio = 1.6;
        double fall_ratio = 0.6;
        size_t confirm_samples = 3;
        uint8_t delta_threshold = FeatureBounds::kDefaultPixelDelta;
        std::string source = "camera-activity";
        Config() = default;
    };

    CameraActivitySession();
    explicit CameraActivitySession(Config cfg);

    // Feed ONE luma plane. `provenance` states where it came from and is never guessed.
    // Returns false-invalid findings for garbage input rather than throwing.
    CameraFinding ingest(const uint8_t* luma, size_t pixels, Provenance provenance,
                         std::chrono::system_clock::time_point now = ActivityChangeDetector::now_default());

    // The device delivered no frame this window (timeout, stall, removal). This is a finding
    // about OUR EQUIPMENT, and it is never reported as a quiet scene - the distinction is the
    // entire safety argument for a camera-based sensor.
    CameraFinding note_stalled_window(const std::string& reason);

    // The current finding without new input.
    CameraFinding current() const { return last_; }

    // The detector's view of the latest input, for the caller that wants the raw status.
    DetectorStatus last_status() const { return change_.status_for(change_.snapshot()); }

    // Build the Observation for the current finding. Provenance and detector identity come
    // from the session; the caller supplies only the id and source label.
    Observation observation_for(const std::string& observation_id,
                                const std::string& source = "") const;

    size_t windows_observed() const { return windows_observed_; }
    const ActivityChangeDetector& detector() const { return change_; }

private:
    // `result` is the observation that DECIDED the kind. Passing it is what keeps a finding's
    // numbers consistent with its own verdict: reading a separate snapshot could report
    // "sustained increase" beside a ratio below 1, which is exactly the kind of contradiction
    // a human reviewer would rightly distrust.
    CameraFinding build(CameraFindingKind kind, const std::string& sentence,
                        const std::string& uncertainty, bool real_source,
                        const ActivityChangeDetector::Result* result = nullptr) const;

    Config cfg_{};
    ActivityChangeDetector change_;
    std::vector<uint8_t> previous_luma_{};   // the one bounded buffer this class retains
    CameraFinding last_{};
    size_t windows_observed_ = 0;
    size_t stalled_windows_ = 0;
};

}  // namespace homeguardian
