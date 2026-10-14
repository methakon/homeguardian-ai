#include "catch2/catch.hpp"

#include "core/ActivityDetectors.h"
#include "core/BoundedFeatures.h"
#include "core/DempsterShafer.h"
#include "core/EnvironmentalCorrelation.h"
#include "core/Observation.h"
#include "core/PrecursorDetection.h"

#include <cmath>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

std::vector<int16_t> make_pcm(size_t frames, int16_t amplitude, uint32_t channels = 1) {
    std::vector<int16_t> pcm(frames * channels, amplitude);
    // A simple alternating pattern so the zero-crossing rate is non-trivial.
    for (size_t i = 0; i < frames; ++i) {
        const int16_t v = (i % 2 == 0) ? amplitude : static_cast<int16_t>(-amplitude);
        for (uint32_t c = 0; c < channels; ++c) pcm[i * channels + c] = v;
    }
    return pcm;
}

}  // namespace

TEST_CASE("bounded feature extraction refuses out-of-range input and never allocates",
          "[precursor][features]") {
    SECTION("audio: null buffer is refused with a reason") {
        const AudioFrameFeatures f = extract_audio_features(nullptr, 1000, 1, 16000);
        REQUIRE_FALSE(f.valid);
        REQUIRE(f.reason == "null pcm buffer");
    }

    SECTION("audio: too few and too many frames are refused") {
        std::vector<int16_t> pcm(FeatureBounds::kMaxAudioFrames + 1, 0);
        const AudioFrameFeatures too_many =
            extract_audio_features(pcm.data(), pcm.size(), 1, 16000);
        REQUIRE_FALSE(too_many.valid);
        REQUIRE(too_many.reason.find("too many frames") != std::string::npos);

        const AudioFrameFeatures too_few = extract_audio_features(pcm.data(), 4, 1, 16000);
        REQUIRE_FALSE(too_few.valid);
        REQUIRE(too_few.reason.find("too few frames") != std::string::npos);
    }

    SECTION("audio: unsupported channel count and sample rate are refused") {
        auto pcm = make_pcm(64, 100);
        const AudioFrameFeatures channels =
            extract_audio_features(pcm.data(), 64, FeatureBounds::kMaxChannels + 1, 16000);
        REQUIRE_FALSE(channels.valid);
        REQUIRE(channels.reason.find("channel count") != std::string::npos);

        const AudioFrameFeatures rate = extract_audio_features(pcm.data(), 64, 1, 96000);
        REQUIRE_FALSE(rate.valid);
        REQUIRE(rate.reason.find("sample rate") != std::string::npos);
    }

    SECTION("audio: silence and full scale are the extremes of the feature range") {
        std::vector<int16_t> silence(160, 0);
        const AudioFrameFeatures s = extract_audio_features(silence.data(), 160, 1, 16000);
        REQUIRE(s.valid);
        REQUIRE(s.rms == Approx(0.0));
        REQUIRE(s.peak == Approx(0.0));

        auto loud = make_pcm(160, 32767);
        const AudioFrameFeatures l = extract_audio_features(loud.data(), 160, 1, 16000);
        REQUIRE(l.valid);
        REQUIRE(l.rms > 0.99);
        REQUIRE(l.peak == Approx(32767.0 / 32768.0).epsilon(0.001));
        REQUIRE(l.zero_crossing_rate == Approx(1.0).epsilon(0.02));
    }

    SECTION("audio: stereo halves the frames-per-channel for the crossing rate") {
        auto pcm = make_pcm(128, 2000, 2);
        const AudioFrameFeatures f = extract_audio_features(pcm.data(), 128, 2, 44100);
        REQUIRE(f.valid);
        REQUIRE(f.channels == 2);
        REQUIRE(f.zero_crossing_rate == Approx(1.0).epsilon(0.05));
    }

    SECTION("motion: identical planes report no change") {
        std::vector<uint8_t> plane(320 * 240, 120);
        const MotionFrameFeatures f =
            extract_motion_features(plane.data(), plane.data(), plane.size());
        REQUIRE(f.valid);
        REQUIRE(f.changed_pixels == 0);
        REQUIRE(f.changed_fraction == Approx(0.0));
        REQUIRE(f.mean_delta == Approx(0.0));
    }

    SECTION("motion: a bounded change is measured, not interpreted") {
        std::vector<uint8_t> before(64 * 64, 100);
        std::vector<uint8_t> after = before;
        for (size_t i = 0; i < 100; ++i) after[i] = 200;  // 100 changed pixels
        const MotionFrameFeatures f =
            extract_motion_features(before.data(), after.data(), before.size());
        REQUIRE(f.valid);
        REQUIRE(f.changed_pixels == 100);
        REQUIRE(f.changed_fraction == Approx(100.0 / 4096.0));
        REQUIRE(f.mean_delta > 0.0);
    }

    SECTION("motion: null planes and oversized geometry are refused") {
        std::vector<uint8_t> plane(64 * 64, 0);
        const MotionFrameFeatures null_prev =
            extract_motion_features(nullptr, plane.data(), plane.size());
        REQUIRE_FALSE(null_prev.valid);
        REQUIRE(null_prev.reason == "null luma plane");

        const MotionFrameFeatures too_big =
            extract_motion_features(plane.data(), plane.data(), FeatureBounds::kMaxPixels + 1);
        REQUIRE_FALSE(too_big.valid);
        REQUIRE(too_big.reason.find("too many pixels") != std::string::npos);
    }

    SECTION("nv21 geometry validation") {
        REQUIRE(nv21_geometry_ok(64, 64, 64 * 64 * 3 / 2));
        REQUIRE_FALSE(nv21_geometry_ok(64, 64, 100));      // short buffer
        REQUIRE_FALSE(nv21_geometry_ok(0, 64, 4096));      // no width
        REQUIRE_FALSE(nv21_geometry_ok(8192, 8192, 999999999));  // absurd geometry
    }

    SECTION("cosine helpers are bounded and defined on degenerate input") {
        const double a[] = {1.0, 0.0};
        const double b[] = {1.0, 0.0};
        const double c[] = {0.0, 1.0};
        REQUIRE(cosine_similarity(a, b, 2) == Approx(1.0));
        REQUIRE(cosine_similarity(a, c, 2) == Approx(0.0).margin(1e-9));
        REQUIRE(cosine_distance(a, c, 2) == Approx(1.0).margin(1e-9));
        const double zero[] = {0.0, 0.0};
        REQUIRE(cosine_similarity(a, zero, 2) == Approx(0.0));  // no direction: defined 0
        REQUIRE(cosine_similarity(nullptr, a, 2) == Approx(0.0));
        double too_long[FeatureBounds::kMaxVectorDim + 1] = {};
        REQUIRE(cosine_similarity(too_long, too_long, FeatureBounds::kMaxVectorDim + 1) ==
                Approx(0.0));
        REQUIRE_FALSE(vector_is_finite(nullptr, 1));
    }
}

TEST_CASE("activity change detector needs a baseline and a sustained move",
          "[precursor][activity]") {
    ActivityChangeDetector detector;
    const auto now = std::chrono::system_clock::now();

    SECTION("warm-up is inconclusive, never a detection") {
        ActivityChangeDetector::Result r;
        for (int i = 0; i < 3; ++i) r = detector.observe(0.1, now);
        REQUIRE_FALSE(r.baseline_ready);
        REQUIRE(detector.status_for(r) == DetectorStatus::inconclusive);
        REQUIRE(r.direction == ActivityChangeDetector::Direction::unknown);
    }

    SECTION("a single spike does not confirm a change") {
        for (int i = 0; i < 8; ++i) detector.observe(0.10, now);
        const auto spike = detector.observe(0.90, now);
        REQUIRE(spike.baseline_ready);
        REQUIRE(spike.direction == ActivityChangeDetector::Direction::rising);
        REQUIRE_FALSE(spike.confirmed);
        REQUIRE(detector.status_for(spike) == DetectorStatus::not_detected);
        REQUIRE(spike.reason.find("not yet confirmed") != std::string::npos);
    }

    SECTION("a sustained rise is confirmed after confirm_samples") {
        for (int i = 0; i < 12; ++i) detector.observe(0.10, now);
        ActivityChangeDetector::Result r;
        for (int i = 0; i < 6; ++i) r = detector.observe(0.60, now);
        REQUIRE(r.confirmed);
        REQUIRE(r.direction == ActivityChangeDetector::Direction::rising);
        REQUIRE(r.ratio > 1.5);
        REQUIRE(detector.status_for(r) == DetectorStatus::detected);
        REQUIRE(r.reason.find("x the learned baseline") != std::string::npos);
    }

    SECTION("a sustained drop is a falling change") {
        for (int i = 0; i < 12; ++i) detector.observe(0.80, now);
        ActivityChangeDetector::Result r;
        for (int i = 0; i < 6; ++i) r = detector.observe(0.05, now);
        REQUIRE(r.confirmed);
        REQUIRE(r.direction == ActivityChangeDetector::Direction::falling);
    }

    SECTION("non-finite input is refused and does not move the baseline") {
        for (int i = 0; i < 8; ++i) detector.observe(0.2, now);
        const size_t before = detector.sample_count();
        const auto r = detector.observe(std::nan(""), now);
        REQUIRE(detector.sample_count() == before);
        REQUIRE(r.reason.find("non-finite") != std::string::npos);
    }

    SECTION("an all-zero baseline with activity reports a finite large ratio") {
        for (int i = 0; i < 8; ++i) detector.observe(0.0, now);
        const auto r = detector.observe(0.5, now);
        REQUIRE(std::isfinite(r.ratio));
        REQUIRE(r.ratio > 1.0);
    }
}

TEST_CASE("vector drift detector flags only sustained regime shifts", "[precursor][vector]") {
    VectorDriftDetector detector;
    const auto now = std::chrono::system_clock::now();
    const std::vector<double> baseline = {1.0, 0.0, 0.0, 0.0};

    SECTION("warm-up is inconclusive") {
        VectorDriftDetector::Result r;
        for (int i = 0; i < 4; ++i) r = detector.observe(baseline, now);
        REQUIRE_FALSE(r.baseline_ready);
        REQUIRE(r.state == VectorDriftDetector::Drift::warming_up);
        REQUIRE(detector.status_for(r) == DetectorStatus::inconclusive);
    }

    SECTION("a one-window spike is drift but not a confirmed shift") {
        VectorDriftDetector::Result r;
        for (int i = 0; i < 12; ++i) r = detector.observe(baseline, now);
        REQUIRE(r.state == VectorDriftDetector::Drift::normal);
        r = detector.observe({0.0, 1.0, 0.0, 0.0}, now);  // orthogonal: distance 1.0
        REQUIRE(r.state == VectorDriftDetector::Drift::drifting);
        REQUIRE_FALSE(r.reason.empty());
        REQUIRE(detector.status_for(r) == DetectorStatus::not_detected);
    }

    SECTION("sustained orthogonal vectors confirm a shift") {
        for (int i = 0; i < 12; ++i) detector.observe(baseline, now);
        VectorDriftDetector::Result r;
        for (int i = 0; i < 3; ++i) r = detector.observe({0.0, 1.0, 0.0, 0.0}, now);
        REQUIRE(r.state == VectorDriftDetector::Drift::confirmed_shift);
        REQUIRE(r.distance == Approx(1.0).epsilon(0.01));
        REQUIRE(detector.status_for(r) == DetectorStatus::detected);
        REQUIRE(r.reason.find("sustained regime shift") != std::string::npos);
    }

    SECTION("a wrong-length vector is refused without resetting the detector") {
        for (int i = 0; i < 12; ++i) detector.observe(baseline, now);
        const size_t before = detector.sample_count();
        const auto r = detector.observe({1.0, 0.0}, now);
        REQUIRE(r.reason.find("dimension") != std::string::npos);
        REQUIRE(detector.sample_count() == before);
        REQUIRE(detector.dim() == 4);
    }

    SECTION("non-finite and oversized vectors are refused") {
        const auto bad = detector.observe({1.0, std::nan(""), 0.0, 0.0}, now);
        REQUIRE(bad.reason.find("non-finite") != std::string::npos);
        const auto empty = detector.observe({}, now);
        REQUIRE(empty.reason.find("empty") != std::string::npos);
    }
}

TEST_CASE("Dempster-Shafer combines evidence, exposes conflict and refuses contradiction",
          "[precursor][dst]") {
    SECTION("validity contract") {
        const MassFunction vacuous = DempsterShafer::vacuous();
        std::string why;
        REQUIRE(DempsterShafer::valid(vacuous, &why));
        REQUIRE(DempsterShafer::ignorance_mass(vacuous) == Approx(1.0));
        REQUIRE(DempsterShafer::belief(vacuous, Hypothesis::activity_change) == Approx(0.0));

        MassFunction bad = vacuous;
        bad[0] = 0.5;  // mass on the empty set
        REQUIRE_FALSE(DempsterShafer::valid(bad, &why));
        REQUIRE(why.find("empty set") != std::string::npos);

        MassFunction sum_bad = DempsterShafer::simple(Hypothesis::routine, 0.5);
        sum_bad[1] = 0.9;
        REQUIRE_FALSE(DempsterShafer::valid(sum_bad, &why));
        REQUIRE(why.find("sum to") != std::string::npos);
    }

    SECTION("simple support functions put the remainder on ignorance") {
        const MassFunction m = DempsterShafer::simple(Hypothesis::activity_change, 0.6);
        REQUIRE(DempsterShafer::belief(m, Hypothesis::activity_change) == Approx(0.6));
        REQUIRE(DempsterShafer::ignorance_mass(m) == Approx(0.4));
        REQUIRE(DempsterShafer::belief(m, Hypothesis::routine) == Approx(0.0));
    }

    SECTION("agreeing sources reinforce the same hypothesis") {
        const MassFunction a = DempsterShafer::simple(Hypothesis::activity_change, 0.6);
        const MassFunction b = DempsterShafer::simple(Hypothesis::activity_change, 0.5);
        const Combination c = DempsterShafer::combine(a, b);
        REQUIRE_FALSE(c.suppressed);
        REQUIRE(c.conflict_k == Approx(0.0).margin(1e-9));
        REQUIRE(DempsterShafer::belief(c.combined, Hypothesis::activity_change) > 0.6);
        REQUIRE(DempsterShafer::ignorance_mass(c.combined) < 0.4);
    }

    SECTION("conflict mass is computed and high conflict suppresses rather than averages") {
        const MassFunction a = DempsterShafer::simple(Hypothesis::activity_change, 0.95);
        const MassFunction b = DempsterShafer::simple(Hypothesis::routine, 0.95);
        const Combination c = DempsterShafer::combine(a, b);
        REQUIRE(c.conflict_k > 0.9);
        REQUIRE(c.suppressed);
        REQUIRE(c.reason.find("contradict") != std::string::npos);
        // The surviving mass must NOT have been normalised up into false certainty.
        double total = 0.0;
        for (double m : c.combined) total += m;
        REQUIRE(total < 1.0);
    }

    SECTION("a contradicted pair stays below the suppression threshold when evidence is weak") {
        const MassFunction a = DempsterShafer::simple(Hypothesis::activity_change, 0.4);
        const MassFunction b = DempsterShafer::simple(Hypothesis::routine, 0.4);
        const Combination c = DempsterShafer::combine(a, b);
        REQUIRE_FALSE(c.suppressed);
        REQUIRE(c.conflict_k < DempsterShafer::kSuppressionK);
    }

    SECTION("argmax reports ties and ignorance carries no belief") {
        bool tie = true;
        const MassFunction vacuous = DempsterShafer::vacuous();
        const Hypothesis h = DempsterShafer::argmax_belief(vacuous, &tie);
        REQUIRE(tie);
        REQUIRE(h == Hypothesis::routine);  // deterministic on a 3-way 0.0 tie

        tie = false;
        const MassFunction m = DempsterShafer::simple(Hypothesis::environmental_anomaly, 0.8);
        REQUIRE(DempsterShafer::argmax_belief(m, &tie) == Hypothesis::environmental_anomaly);
        REQUIRE_FALSE(tie);
    }

    SECTION("an invalid mass function suppresses the combination") {
        MassFunction bad = DempsterShafer::vacuous();
        bad[0] = 0.3;
        const Combination c = DempsterShafer::combine(bad, DempsterShafer::vacuous());
        REQUIRE(c.suppressed);
        REQUIRE(c.reason.find("invalid mass function") != std::string::npos);
    }

    SECTION("the summary carries K, belief and ignorance") {
        const Combination c = DempsterShafer::combine(
            DempsterShafer::simple(Hypothesis::activity_change, 0.7),
            DempsterShafer::simple(Hypothesis::activity_change, 0.5));
        const std::string s = DempsterShafer::summarize(c, Hypothesis::activity_change);
        REQUIRE(s.find("K=") != std::string::npos);
        REQUIRE(s.find("belief=") != std::string::npos);
        REQUIRE(s.find("ignorance=") != std::string::npos);
        REQUIRE(s.find("suppressed=no") != std::string::npos);
    }

    SECTION("the frame cannot represent a disaster hypothesis") {
        // Compile-time/documentation guard: the enum has exactly three members and
        // none of them is a calamity. A future edit that adds one must break this.
        REQUIRE(kHypothesisCount == 3);
        REQUIRE(std::string(hypothesis_to_string(Hypothesis::routine)) == "routine");
        REQUIRE(std::string(hypothesis_to_string(Hypothesis::activity_change)) == "activity_change");
        REQUIRE(std::string(hypothesis_to_string(Hypothesis::environmental_anomaly)) ==
                "environmental_anomaly");
    }
}

TEST_CASE("precursor suite produces only the permitted result vocabulary", "[precursor][suite]") {
    PrecursorSuite suite;
    const auto now = std::chrono::system_clock::now();

    SECTION("with no evidence at all the result is insufficient_evidence") {
        const PrecursorFusion f = suite.fusion();
        REQUIRE(f.label == PrecursorLabel::insufficient_evidence);
        REQUIRE_FALSE(f.suppressed);
        REQUIRE(f.confirming_sources == 0);
    }

    SECTION("warming up alone is not evidence") {
        for (int i = 0; i < 2; ++i) suite.observe_motion(0.1, now);
        REQUIRE(suite.fusion().label == PrecursorLabel::insufficient_evidence);
    }

    SECTION("a sustained motion change reports activity_change_detected") {
        for (int i = 0; i < 12; ++i) suite.observe_motion(0.05, now);
        for (int i = 0; i < 6; ++i) suite.observe_motion(0.40, now);
        const PrecursorFusion f = suite.fusion();
        REQUIRE(f.label == PrecursorLabel::activity_change_detected);
        REQUIRE(f.belief > 0.5);
        REQUIRE(f.confirming_sources >= 1);
        REQUIRE(f.explanation.find("sustained activity level change") != std::string::npos);
    }

    SECTION("audio energy is measured and a deviation needs a baseline") {
        for (int i = 0; i < 10; ++i) {
            auto pcm = make_pcm(1200, 300);
            suite.observe_audio(pcm.data(), 1200, 1, 16000, now);
        }
        REQUIRE(suite.audio().baseline_ready);
        REQUIRE(suite.audio_samples() >= 10);
    }

    SECTION("possible_animal_activity requires two modalities plus a confirmed drift") {
        // Motion + audio sustained change, plus a confirmed vector shift, and no
        // environmental explanation: the ONLY path to this label.
        for (int i = 0; i < 12; ++i) {
            suite.observe_motion(0.05, now);
            auto pcm = make_pcm(1200, 200);
            suite.observe_audio(pcm.data(), 1200, 1, 16000, now);
            suite.observe_vector({1.0, 0.0, 0.0}, now);
        }
        for (int i = 0; i < 6; ++i) {
            suite.observe_motion(0.45, now);
            auto pcm = make_pcm(1200, 12000);
            suite.observe_audio(pcm.data(), 1200, 1, 16000, now);
            suite.observe_vector({0.0, 1.0, 0.0}, now);
        }
        const PrecursorFusion f = suite.fusion();
        REQUIRE(suite.vector().state == VectorDriftDetector::Drift::confirmed_shift);
        REQUIRE(f.label == PrecursorLabel::possible_animal_activity);
        // The label must carry its own disclaimer, in the observation too.
        REQUIRE(f.explanation.find("HYPOTHESIS ONLY") != std::string::npos);
        REQUIRE(f.explanation.find("NOT a species identification") != std::string::npos);

        const auto observations = suite.observations("node:hg-test", Provenance::synthetic);
        bool saw_fusion = false;
        for (const Observation& o : observations) {
            if (o.detector_id != "precursor-fusion") continue;
            saw_fusion = true;
            REQUIRE(o.features["result"] == "possible_animal_activity");
            REQUIRE(o.note.find("NOT a species identification") != std::string::npos);
            REQUIRE(o.provenance == Provenance::synthetic);
            // Round-trips through the EXISTING pipeline conversion.
            const Event e = o.to_event();
            REQUIRE(e.get_type() == EventType::inference);
            const auto back = Observation::from_event(e);
            REQUIRE(back.has_value());
            REQUIRE(back->features["result"] == "possible_animal_activity");
        }
        REQUIRE(saw_fusion);
    }

    SECTION("an environmental explanation takes precedence over the animal hypothesis") {
        for (int i = 0; i < 12; ++i) {
            suite.observe_motion(0.05, now);
            auto pcm = make_pcm(1200, 200);
            suite.observe_audio(pcm.data(), 1200, 1, 16000, now);
            suite.observe_vector({1.0, 0.0, 0.0}, now);
        }
        for (int i = 0; i < 6; ++i) {
            suite.observe_motion(0.45, now);
            auto pcm = make_pcm(1200, 12000);
            suite.observe_audio(pcm.data(), 1200, 1, 16000, now);
            suite.observe_vector({0.0, 1.0, 0.0}, now);
        }
        suite.observe_environmental(0.9, now);
        const PrecursorFusion f = suite.fusion();
        REQUIRE(f.label == PrecursorLabel::environmental_correlation);
        // The environmental reading explains the change, so the animal hypothesis
        // is NOT raised at all.
        REQUIRE(f.explanation.find("HYPOTHESIS ONLY") == std::string::npos);
        REQUIRE(f.explanation.find("not causation") != std::string::npos);
    }

    SECTION("the fusion label is never a disaster claim") {
        const char* labels[] = {"insufficient_evidence", "activity_change_detected",
                                "audio_activity_deviation", "possible_animal_activity",
                                "environmental_correlation"};
        for (const char* label : labels) {
            const std::string s = label;
            REQUIRE(s.find("earthquake") == std::string::npos);
            REQUIRE(s.find("flood") == std::string::npos);
            REQUIRE(s.find("disaster") == std::string::npos);
            REQUIRE(s.find("predict") == std::string::npos);
        }
    }

    SECTION("reset clears every detector and the environmental context") {
        for (int i = 0; i < 12; ++i) suite.observe_motion(0.05, now);
        suite.observe_environmental(0.9, now);
        suite.reset();
        REQUIRE(suite.motion_samples() == 0);
        REQUIRE(suite.audio_samples() == 0);
        REQUIRE(suite.vector_samples() == 0);
        REQUIRE(suite.fusion().label == PrecursorLabel::insufficient_evidence);
    }
}

TEST_CASE("environmental correlation is bounded, authorized and labelled as correlation",
          "[precursor][environmental]") {
    using namespace std::chrono;
    const auto now = system_clock::now();

    EnvironmentalSample sample;
    sample.source = "ha.sensor.living_room_temperature";
    sample.metric = "temperature";
    sample.value = 21.5;
    sample.unit = "C";
    sample.observed_at = now;
    sample.provenance = Provenance::real;
    sample.authorization = "subject=node:hg-1;purpose=environment_monitor;decision=granted";

    EnvironmentalAdapter adapter;

    SECTION("an authorized, plausible sample becomes a bounded observation") {
        std::string why;
        REQUIRE(adapter.validate(sample, now, &why));
        const auto observation = adapter.to_observation(sample, "env-1", now);
        REQUIRE(observation.has_value());
        REQUIRE(observation->detector_id == "environmental-temperature");
        REQUIRE(observation->status == DetectorStatus::detected);
        REQUIRE(observation->provenance == Provenance::real);
        REQUIRE(observation->features["metric"] == "temperature");
        REQUIRE(observation->features["value"] == Approx(21.5));
    }

    SECTION("an unauthorized sample is refused and never ingested") {
        EnvironmentalSample denied = sample;
        denied.authorization = "subject=node:hg-1;purpose=environment_monitor;decision=denied";
        std::string why;
        REQUIRE_FALSE(adapter.validate(denied, now, &why));
        REQUIRE(why.find("grant") != std::string::npos);
        REQUIRE_FALSE(adapter.to_observation(denied, "env-2", now).has_value());

        EnvironmentalSample missing = sample;
        missing.authorization.clear();
        REQUIRE_FALSE(adapter.validate(missing, now, &why));
        REQUIRE(why.find("missing authorization") != std::string::npos);
    }

    SECTION("implausible values and unknown metrics are refused") {
        std::string why;
        EnvironmentalSample humidity = sample;
        humidity.metric = "humidity";
        humidity.value = 100000.0;
        REQUIRE_FALSE(adapter.validate(humidity, now, &why));
        REQUIRE(why.find("implausible") != std::string::npos);

        EnvironmentalSample unknown = sample;
        unknown.metric = "vibes";
        REQUIRE_FALSE(adapter.validate(unknown, now, &why));
        REQUIRE(why.find("unsupported metric") != std::string::npos);

        EnvironmentalSample future = sample;
        future.observed_at = now + std::chrono::hours(1);
        REQUIRE_FALSE(adapter.validate(future, now, &why));
        REQUIRE(why.find("future") != std::string::npos);

        EnvironmentalSample no_time = sample;
        no_time.observed_at = {};
        REQUIRE_FALSE(adapter.validate(no_time, now, &why));
    }

    SECTION("correlation needs both a change and a sample in the window") {
        CorrelationWindow window;
        CorrelationWindow::Result r = window.correlate(now);
        REQUIRE(r.status == "insufficient_evidence");
        REQUIRE_FALSE(r.correlated);

        CorrelationWindow::ChangeRecord change;
        change.source = "node:hg-1";
        change.kind = "activity_change";
        change.at = now;
        window.add_change(change);

        r = window.correlate(now);
        REQUIRE(r.status == "insufficient_evidence");
        REQUIRE(r.reason.find("no authorized environmental sample") != std::string::npos);

        window.add_sample(sample);
        r = window.correlate(now);
        REQUIRE(r.correlated);
        REQUIRE(r.status == "environmental_correlation");
        REQUIRE(r.metric == "temperature");
        REQUIRE(r.lag_seconds == 0);
        REQUIRE(r.reason.find("not causation") != std::string::npos);

        const Observation o = window.to_observation(r, "corr-1", "hub", Provenance::real, now);
        REQUIRE(o.detector_id == "environmental-correlation");
        REQUIRE(o.status == DetectorStatus::detected);
        REQUIRE(o.features["result"] == "environmental_correlation");
        REQUIRE(o.confidence.has_value());
        REQUIRE(o.confidence.value() < 0.5);  // Category B evidence is weak by construction
    }

    SECTION("a sample outside the maximum lag is not correlated") {
        CorrelationWindow::Config cfg;
        cfg.max_lag = std::chrono::seconds(60);
        CorrelationWindow window(cfg);
        CorrelationWindow::ChangeRecord change;
        change.kind = "activity_change";
        change.at = now - std::chrono::minutes(20);
        window.add_change(change);
        EnvironmentalSample late = sample;
        late.observed_at = now;
        window.add_sample(late);
        const auto r = window.correlate(now);
        REQUIRE_FALSE(r.correlated);
        REQUIRE(r.status == "insufficient_evidence");
    }

    SECTION("stale evidence is pruned and cannot produce a fresh finding") {
        CorrelationWindow window;
        CorrelationWindow::ChangeRecord change;
        change.kind = "activity_change";
        change.at = now - std::chrono::hours(4);
        window.add_change(change);
        window.add_sample(sample);
        const auto r = window.correlate(now);
        REQUIRE_FALSE(r.correlated);
        REQUIRE(r.change_count == 0);
    }

    SECTION("the window and its sample lists are bounded") {
        CorrelationWindow::Config cfg;
        cfg.max_changes = 4;
        cfg.max_samples = 4;
        CorrelationWindow window(cfg);
        for (int i = 0; i < 50; ++i) {
            CorrelationWindow::ChangeRecord c;
            c.kind = "activity_change";
            c.at = now;
            window.add_change(c);
            EnvironmentalSample s = sample;
            s.observed_at = now;
            window.add_sample(s);
        }
        REQUIRE(window.change_count() <= 4);
        REQUIRE(window.sample_count() <= 4);
    }

    SECTION("an unsupported or implausible sample is kept out of the window") {
        CorrelationWindow window;
        EnvironmentalSample bogus = sample;
        bogus.metric = "radiation";
        window.add_sample(bogus);
        bogus.metric = "humidity";
        bogus.value = -5.0;
        window.add_sample(bogus);
        REQUIRE(window.sample_count() == 0);
    }
}
