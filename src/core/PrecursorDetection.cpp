#include "PrecursorDetection.h"

#include "core/UUID.h"

#include <algorithm>
#include <cmath>

namespace homeguardian {

namespace {

using Direction = ActivityChangeDetector::Direction;

// Map an activity-change result to a Basic Probability Assignment. Warming up is
// expressed as IGNORANCE (vacuous), not as evidence of routine behaviour: a
// detector that has no baseline must not vote.
MassFunction activity_mass(const ActivityChangeDetector::Result& r, bool present) {
    if (!present || !r.baseline_ready) return DempsterShafer::vacuous();
    if (r.confirmed && r.direction != Direction::stable) {
        // Strength saturates: a 3x rise is already maximal support for "the
        // activity level changed" as a general hypothesis.
        const double excess = r.ratio > 1.0 ? r.ratio - 1.0 : 0.0;
        const double strength = std::min(0.85, 0.45 + 0.15 * std::min(excess, 3.0));
        return DempsterShafer::simple(Hypothesis::activity_change, strength);
    }
    const double excess = r.ratio > 1.0 ? r.ratio - 1.0 : 1.0 - r.ratio;
    // Within the band: weak support for routine, scaled down as the ratio moves
    // toward the confirmation threshold, never above the change mass.
    const double routine = std::max(0.1, 0.4 - 0.2 * std::min(excess, 1.5));
    return DempsterShafer::simple(Hypothesis::routine, routine);
}

std::string label_reason(PrecursorLabel l, const PrecursorFusion& f) {
    switch (l) {
        case PrecursorLabel::activity_change_detected:
            return "sustained activity level change against the learned baseline (" +
                   std::to_string(f.confirming_sources) + " confirming source(s))";
        case PrecursorLabel::audio_activity_deviation:
            return "bounded audio energy outside its learned band; no speech, species or "
                   "content interpretation is performed";
        case PrecursorLabel::possible_animal_activity:
            return "HYPOTHESIS ONLY: a sustained multi-sensor activity regime change with no "
                   "environmental explanation. This is NOT a species identification and must "
                   "not be presented as one";
        case PrecursorLabel::environmental_correlation:
            return "detected change coincides with an authorized environmental measurement "
                   "moving; correlation is not causation";
        case PrecursorLabel::insufficient_evidence:
            return "insufficient or contradictory evidence";
    }
    return "insufficient or contradictory evidence";
}

}  // namespace

PrecursorSuite::Config PrecursorSuite::defaults() {
    Config c;
    c.motion_activity.feature = "motion-changed-fraction";
    c.motion_activity.detector_id = "motion-activity";
    c.motion_activity.model_id = "ema-baseline-v1";
    c.motion_activity.confirm_samples = 3;

    c.audio_activity.feature = "audio-rms";
    c.audio_activity.detector_id = "audio-activity";
    c.audio_activity.model_id = "ema-baseline-v1";
    c.audio_activity.confirm_samples = 3;

    // Reuse of the EXISTING statistical detector: the temporal-deviation path is
    // deliberately not re-implemented here.
    c.audio_deviation.feature = "audio-rms";
    c.audio_deviation.key = "audio-rms";
    c.audio_deviation.detector_id = "audio-activity-deviation";
    c.audio_deviation.zscore_threshold = 2.0;
    c.audio_deviation.min_samples = 8;
    c.audio_deviation.max_window = 32;

    c.vector.detector_id = "vector-drift";
    c.vector.model_id = "unsupervised-centroid-v1";
    return c;
}

PrecursorSuite::PrecursorSuite(Config cfg)
    : cfg_(std::move(cfg)),
      motion_detector_(cfg_.motion_activity),
      audio_detector_(cfg_.audio_activity),
      audio_deviation_monitor_(cfg_.audio_deviation),
      vector_detector_(cfg_.vector) {}

void PrecursorSuite::reset() {
    motion_detector_.reset();
    audio_detector_.reset();
    audio_deviation_monitor_.reset();
    vector_detector_.reset();
    motion_result_ = ActivityChangeDetector::Result{};
    audio_result_ = ActivityChangeDetector::Result{};
    audio_deviation_result_ = AnomalyResult{};
    vector_result_ = VectorDriftDetector::Result{};
    has_motion_ = has_audio_ = has_deviation_ = has_vector_ = has_environmental_ = false;
    environmental_strength_ = 0.0;
    last_observation_at_ = {};
    has_any_observation_ = false;
}

ActivityChangeDetector::Result PrecursorSuite::observe_motion(
    double changed_fraction, std::chrono::system_clock::time_point now) {
    motion_result_ = motion_detector_.observe(changed_fraction, now);
    has_motion_ = true;
    last_observation_at_ = now;
    has_any_observation_ = true;
    return motion_result_;
}

ActivityChangeDetector::Result PrecursorSuite::observe_audio_features(
    const AudioFrameFeatures& f, std::chrono::system_clock::time_point now) {
    if (!f.valid) {
        // A refused buffer is NOT a sample: it must not move a baseline. Report
        // the refusal in the result so the caller can surface it.
        ActivityChangeDetector::Result r = motion_detector_.snapshot();
        r.reason = "audio features unavailable: " + f.reason;
        audio_result_ = r;
        has_audio_ = true;
        return r;
    }
    audio_result_ = audio_detector_.observe(f.rms, now);
    has_audio_ = true;
    last_observation_at_ = now;
    has_any_observation_ = true;
    // Statistical deviation over the same bounded feature, using the existing
    // AnomalyMonitor (a different target from the level detector: deviation from
    // the learned band, not "presence of activity").
    audio_deviation_result_ = audio_deviation_monitor_.observe(f.rms, now);
    has_deviation_ = true;
    return audio_result_;
}

AudioFrameFeatures PrecursorSuite::observe_audio(const int16_t* pcm, size_t frames, uint32_t channels,
                                                 uint32_t sample_rate,
                                                 std::chrono::system_clock::time_point now) {
    const AudioFrameFeatures f = extract_audio_features(pcm, frames, channels, sample_rate);
    observe_audio_features(f, now);
    return f;
}

VectorDriftDetector::Result PrecursorSuite::observe_vector(
    const std::vector<double>& v, std::chrono::system_clock::time_point now) {
    vector_result_ = vector_detector_.observe(v, now);
    has_vector_ = true;
    last_observation_at_ = now;
    has_any_observation_ = true;
    return vector_result_;
}

void PrecursorSuite::observe_environmental(double deviation_strength,
                                           std::chrono::system_clock::time_point now) {
    double s = deviation_strength;
    if (!(s > 0.0)) s = 0.0;
    if (s > 1.0) s = 1.0;
    environmental_strength_ = s;
    has_environmental_ = true;
    last_observation_at_ = now;
    has_any_observation_ = true;
}

PrecursorFusion PrecursorSuite::fusion(std::chrono::system_clock::time_point now) const {
    PrecursorFusion out;

    // A stale suite reports insufficient evidence rather than repeating an old
    // finding: silence is not agreement.
    if (has_any_observation_ && cfg_.stale_after.count() > 0 &&
        now - last_observation_at_ > cfg_.stale_after) {
        out.label = PrecursorLabel::insufficient_evidence;
        out.hypothesis = hypothesis_to_string(Hypothesis::routine);
        out.suppressed = false;
        out.explanation = "no fresh observation within " +
                          std::to_string(cfg_.stale_after.count()) +
                          "s; a stale finding is not an update";
        return out;
    }

    MassFunction m_motion = activity_mass(motion_result_, has_motion_);
    MassFunction m_audio = activity_mass(audio_result_, has_audio_);

    // A statistical audio deviation is a second, independent view of the same
    // bounded feature. Folded in only when it actually flagged.
    if (has_deviation_ && audio_deviation_result_.anomalous) {
        const Combination fold =
            DempsterShafer::combine(m_audio, DempsterShafer::simple(Hypothesis::activity_change, 0.3));
        if (!fold.suppressed) m_audio = fold.combined;
    }

    MassFunction m_env = DempsterShafer::vacuous();
    if (has_environmental_ && environmental_strength_ > 0.0) {
        m_env = DempsterShafer::simple(Hypothesis::environmental_anomaly,
                                       std::min(0.8, environmental_strength_));
    }

    // Pairwise fold, re-normalising at each step. K is reported as the LARGEST
    // conflict seen, which is the conservative reading: it is "how much did any
    // one source disagree with everything else".
    const Combination first = DempsterShafer::combine(m_motion, m_audio);
    if (first.suppressed) {
        out.suppressed = true;
        out.conflict_k = first.conflict_k;
        out.label = PrecursorLabel::insufficient_evidence;
        out.explanation = "sources contradict (" + first.reason + "); contradiction is not evidence";
        out.hypothesis = hypothesis_to_string(Hypothesis::routine);
        return out;
    }

    const Combination second = DempsterShafer::combine(first.combined, m_env);
    out.conflict_k = std::max(first.conflict_k, second.conflict_k);
    if (second.suppressed) {
        out.suppressed = true;
        out.label = PrecursorLabel::insufficient_evidence;
        out.explanation = "sources contradict (" + second.reason + "); contradiction is not evidence";
        out.hypothesis = hypothesis_to_string(Hypothesis::routine);
        return out;
    }

    bool tie = false;
    const Hypothesis top = DempsterShafer::argmax_belief(second.combined, &tie);
    out.hypothesis = hypothesis_to_string(top);
    out.belief = DempsterShafer::belief(second.combined, top);
    out.ignorance = DempsterShafer::ignorance_mass(second.combined);

    size_t sources = 0;
    if (has_motion_ && motion_result_.baseline_ready && motion_result_.confirmed) ++sources;
    if (has_audio_ && audio_result_.baseline_ready && audio_result_.confirmed) ++sources;
    out.confirming_sources = sources;

    const bool motion_confirmed = has_motion_ && motion_result_.confirmed;
    const bool audio_confirmed = has_audio_ && audio_result_.confirmed;
    const bool deviation_flagged = has_deviation_ && audio_deviation_result_.anomalous;
    const bool vector_shift = has_vector_ && vector_result_.state == VectorDriftDetector::Drift::confirmed_shift;
    const bool something_changed = motion_confirmed || audio_confirmed || deviation_flagged || vector_shift;

    // Precedence: a real environmental correlate outranks the animal HYPOTHESIS
    // (but never outranks nothing - a lone reading is not a correlation).
    const bool env_explains = has_environmental_ && something_changed &&
                              environmental_strength_ >= cfg_.environmental_explains_strength;

    if (env_explains) {
        out.label = PrecursorLabel::environmental_correlation;
    } else if (motion_confirmed && (audio_confirmed || deviation_flagged) && vector_shift &&
               sources >= cfg_.possible_animal_min_sources &&
               out.belief >= cfg_.possible_animal_belief) {
        // The ONLY path to this label: a sustained regime shift confirmed on at
        // least two independent modalities, with no environmental explanation.
        out.label = PrecursorLabel::possible_animal_activity;
    } else if (motion_confirmed || audio_confirmed) {
        out.label = PrecursorLabel::activity_change_detected;
    } else if (deviation_flagged) {
        out.label = PrecursorLabel::audio_activity_deviation;
    } else {
        out.label = PrecursorLabel::insufficient_evidence;
    }

    out.explanation = label_reason(out.label, out);
    if (tie) out.explanation += " (top hypotheses tied: undecided)";
    return out;
}

std::vector<Observation> PrecursorSuite::observations(const std::string& source,
                                                      Provenance provenance) const {
    std::vector<Observation> out;

    if (has_motion_) {
        Observation o = motion_detector_.to_observation(motion_result_, UUID::generate(), source, provenance);
        o.observation_timestamp = std::chrono::system_clock::now();
        out.push_back(std::move(o));
    }
    if (has_audio_) {
        Observation o = audio_detector_.to_observation(audio_result_, UUID::generate(), source, provenance);
        o.observation_timestamp = std::chrono::system_clock::now();
        out.push_back(std::move(o));
    }
    if (has_deviation_ && audio_deviation_result_.sample_count >= cfg_.audio_deviation.min_samples) {
        Observation o = audio_deviation_monitor_.to_observation(
            audio_deviation_result_, UUID::generate(), std::chrono::system_clock::now(),
            cfg_.audio_deviation.min_samples);
        o.source = source;
        o.provenance = provenance;
        out.push_back(std::move(o));
    }
    if (has_vector_) {
        Observation o = vector_detector_.to_observation(vector_result_, UUID::generate(), source, provenance);
        o.observation_timestamp = std::chrono::system_clock::now();
        out.push_back(std::move(o));
    }

    const PrecursorFusion f = fusion();
    Observation fused;
    fused.observation_id = UUID::generate();
    fused.source = source;
    fused.provenance = provenance;
    fused.detector_id = "precursor-fusion";
    fused.model_id = "dst-v1";
    fused.observation_timestamp = std::chrono::system_clock::now();
    fused.status = f.label == PrecursorLabel::insufficient_evidence ? DetectorStatus::inconclusive
                                                                   : DetectorStatus::detected;
    fused.features = nlohmann::json::object();
    fused.features["result"] = precursor_label_to_string(f.label);
    fused.features["hypothesis"] = f.hypothesis;
    fused.features["belief"] = f.belief;
    fused.features["ignorance"] = f.ignorance;
    fused.features["conflict_k"] = f.conflict_k;
    fused.features["confirming_sources"] = f.confirming_sources;
    fused.features["suppressed"] = f.suppressed;
    fused.confidence = f.suppressed ? 0.0 : f.belief;
    fused.note = f.explanation;
    out.push_back(std::move(fused));
    return out;
}

}  // namespace homeguardian
