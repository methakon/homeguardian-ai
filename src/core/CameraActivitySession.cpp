#include "core/CameraActivitySession.h"

#include <utility>

namespace homeguardian {

namespace {

// The detector is configured from the session config so there is exactly one vocabulary of
// thresholds in the camera path, and it stays the SAME detector the audio path uses.
ActivityChangeDetector::Config change_config_for(const CameraActivitySession::Config& c) {
    ActivityChangeDetector::Config change;
    change.warmup = c.warmup;
    change.recent_window = c.recent_window;
    change.history = c.history;
    change.slow_alpha = c.slow_alpha;
    change.confirm_ratio = c.confirm_ratio;
    change.fall_ratio = c.fall_ratio;
    change.confirm_samples = c.confirm_samples;
    change.feature = "motion-changed-fraction";
    change.detector_id = "camera-motion-activity";
    change.model_id = "ema-baseline-v1";
    return change;
}

constexpr const char* kUncertainty =
    "motion energy is a bounded scalar: it is not a person, an animal, a species, a face, a "
    "gesture, a fall or an emergency, and nothing in this build can infer one from it";

}  // namespace

CameraActivitySession::CameraActivitySession() : CameraActivitySession(Config{}) {}

CameraActivitySession::CameraActivitySession(Config cfg)
    : cfg_(std::move(cfg)), change_(change_config_for(cfg_)) {}

CameraFinding CameraActivitySession::build(CameraFindingKind kind, const std::string& sentence,
                                           const std::string& uncertainty, bool real_source,
                                           const ActivityChangeDetector::Result* result) const {
    CameraFinding f;
    f.kind = kind;
    f.sentence = sentence;
    f.uncertainty = uncertainty;
    f.real_source = real_source;
    const auto snap = result != nullptr ? *result : change_.snapshot();
    f.baseline = snap.baseline_level;
    f.ratio = snap.ratio;
    if (result != nullptr) f.reason = result->reason;
    f.windows_observed = windows_observed_;
    f.stalled_windows = stalled_windows_;
    f.valid = (kind != CameraFindingKind::insufficient_evidence);
    return f;
}

CameraFinding CameraActivitySession::ingest(const uint8_t* luma, size_t pixels,
                                            Provenance provenance,
                                            std::chrono::system_clock::time_point now) {
    const bool real_source = (provenance == Provenance::real);

    if (luma == nullptr) {
        last_ = build(CameraFindingKind::insufficient_evidence, "no frame was supplied",
                      "a missing frame is a caller error, not a quiet scene", real_source);
        last_.reason = "no frame was supplied";
        return last_;
    }
    if (pixels < FeatureBounds::kMinPixels || pixels > FeatureBounds::kMaxPixels) {
        // Refused, not resized: an oversized plane is outside the bounded feature contract.
        last_ = build(CameraFindingKind::insufficient_evidence,
                      "the frame geometry is outside the bounded feature limits",
                      "an unsupported geometry was refused rather than sampled", real_source);
        last_.reason = "the frame geometry is outside the bounded feature limits";
        return last_;
    }

    if (previous_luma_.empty()) {
        previous_luma_.assign(luma, luma + pixels);
        ++windows_observed_;
        last_ = build(CameraFindingKind::warming_up,
                      "no previous frame yet: the first frame establishes a reference",
                      "one frame cannot show change; this is not a quiet scene, it is an "
                      "unknown one",
                      real_source);
        return last_;
    }
    if (previous_luma_.size() != pixels) {
        // Geometry changed mid-session (a renegotiated format). Drop the reference instead of
        // comparing incomparable planes.
        previous_luma_.assign(luma, luma + pixels);
        ++windows_observed_;
        last_ = build(CameraFindingKind::warming_up,
                      "the frame geometry changed: the reference frame was restarted",
                      "motion cannot be measured across two different geometries", real_source);
        return last_;
    }

    const MotionFrameFeatures motion =
        extract_motion_features(previous_luma_.data(), luma, pixels, cfg_.delta_threshold);

    // Region analysis, in the same window as the motion descriptor (before the reference frame is
    // overwritten). Only when the caller supplied a geometry that describes this plane: without it the
    // character stays unknown, and unknown is never treated as either illumination or motion.
    RegionChangeFeatures region;
    ChangeCharacter character = ChangeCharacter::unknown;
    if (cfg_.width != 0 && cfg_.height != 0 &&
        static_cast<size_t>(cfg_.width) * cfg_.height == pixels) {
        region = extract_region_change_features(previous_luma_.data(), luma, pixels, cfg_.width,
                                                cfg_.height, cfg_.delta_threshold);
        if (region.valid) character = classify_change(region);
    }

    previous_luma_.assign(luma, luma + pixels);
    ++windows_observed_;

    if (!motion.valid) {
        last_ = build(CameraFindingKind::insufficient_evidence,
                      "the motion descriptor could not be computed: " + motion.reason,
                      "no measurement was made, so nothing is claimed", real_source);
        last_.reason = motion.reason;
        last_.motion_level = 0.0;
        return last_;
    }

    // The ONE number this whole path produces, fed to the SAME detector the audio path uses.
    const auto result = change_.observe(motion.changed_fraction, now);

    // `build()` constructs a fresh finding, so every field that is not part of the verdict has to be
    // applied AFTER it - once, in one place, or a later build silently wipes it. (It did: the change
    // character was assigned before the verdict and lost on the way back, which is why the first
    // version of this reported an empty character for every frame.)
    const auto apply_extras = [&]() {
        last_.motion_level = motion.changed_fraction;
        last_.mean_delta = motion.mean_delta;
        last_.character_valid = region.valid;
        last_.change_character = change_character_to_string(character);
        last_.spread_radius = region.spread_radius;
        last_.bipolar_balance = region.bipolar_balance;
    };

    if (!result.baseline_ready) {
        last_ = build(CameraFindingKind::warming_up,
                      "still establishing a baseline for motion in view",
                      "a baseline that is not ready cannot report a change", real_source, &result);
        apply_extras();
        return last_;
    }
    if (result.confirmed && result.direction == ActivityChangeDetector::Direction::rising) {
        last_ = build(CameraFindingKind::sustained_increase,
                      "a sustained increase in motion in view", kUncertainty, real_source, &result);
    } else if (result.confirmed && result.direction == ActivityChangeDetector::Direction::falling) {
        last_ = build(CameraFindingKind::sustained_decrease,
                      "a sustained decrease in motion in view", kUncertainty, real_source, &result);
    } else {
        last_ = build(CameraFindingKind::quiet_baseline,
                      "motion within the established baseline", kUncertainty, real_source, &result);
    }
    apply_extras();
    return last_;
}

CameraFinding CameraActivitySession::note_stalled_window(const std::string& reason) {
    ++stalled_windows_;
    // Deliberately NOT `quiet_baseline`: a camera that stopped delivering frames has told us
    // nothing about the scene, and reporting silence would be the worst possible failure mode.
    last_ = build(CameraFindingKind::frames_stalled,
                  "the camera delivered no frame this window",
                  "this is a finding about the capture device, not about what is in front of it",
                  /*real_source=*/false);   // nothing was measured, so no provenance is claimed
    last_.reason = reason;
    return last_;
}

Observation CameraActivitySession::observation_for(const std::string& observation_id,
                                                   const std::string& source) const {
    // The observation is built by the DETECTOR, so its provenance, detector id and model id
    // come from the component that produced the number rather than from this wrapper.
    Provenance provenance = last_.real_source ? Provenance::real : Provenance::synthetic;
    Observation o = change_.to_observation(change_.snapshot(), observation_id,
                                           source.empty() ? cfg_.source : source, provenance);
    // Added here, not in the detector: the detector classifies a level over time and has no idea about
    // pixels. These keys are additive, so nothing that reads the feature set breaks.
    if (last_.character_valid) {
        o.features["change_character"] = last_.change_character;
        o.features["spread_radius"] = last_.spread_radius;
        o.features["bipolar_balance"] = last_.bipolar_balance;
    }
    return o;
}

}  // namespace homeguardian
