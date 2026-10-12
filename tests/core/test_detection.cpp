#include "catch2/catch.hpp"
#include "core/Observation.h"
#include "core/AnomalyDetector.h"
#include "core/FusionEngine.h"
#include "core/SyntheticActivityDetector.h"
#include "core/Logger.h"
#include <chrono>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Detection framework: Observation model, explainable anomaly detection,
// multimodal fusion, and a synthetic activity detector. All synthetic; no
// physical sensor; no ML dependency. Feeds the EXISTING Event pipeline.
// ---------------------------------------------------------------------------

namespace {
using clk = std::chrono::system_clock;
clk::time_point T(long long ms) { return clk::time_point(std::chrono::milliseconds(ms)); }
}

// ===========================================================================
// Observation model
// ===========================================================================

TEST_CASE("Observation: valid observation converts to a standard Event",
          "[detection][observation]") {
    Logger::initialize("error");
    Observation o;
    o.observation_id = "obs-1";
    o.source = "sim_sensor";
    o.detector_id = "activity-energy";
    o.model_id = "synthetic-v1";
    o.status = DetectorStatus::detected;
    o.confidence = 0.9;
    o.quality = 0.8;
    o.features = {{"energy", 0.7}};
    REQUIRE_NOTHROW(o.validate());

    Event e = o.to_event();
    REQUIRE(e.get_type() == EventType::inference);
    REQUIRE(e.get_source() == "sim_sensor");
    REQUIRE(e.get_confidence().has_value());
    REQUIRE(e.get_confidence().value() == Approx(0.9));
    // Provenance/detector/status carried in payload + metadata.
    REQUIRE(e.get_payload()["provenance"] == "synthetic");
    REQUIRE(e.get_payload()["detector_id"] == "activity-energy");
    REQUIRE(e.get_payload()["status"] == "detected");
}

TEST_CASE("Observation: invalid observations are rejected (fail-fast)",
          "[detection][observation]") {
    Logger::initialize("error");
    Observation base;
    base.observation_id = "x"; base.source = "s"; base.detector_id = "d";

    auto bad = base; bad.observation_id = "";
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.source = "";
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.detector_id = "";
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.confidence = 1.5;
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.quality = -0.1;
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.note = std::string(Observation::kMaxNoteChars + 1, 'x');
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

    bad = base; bad.features = nlohmann::json::array();  // not an object
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);
}

TEST_CASE("Observation: oversized feature payload is rejected",
          "[detection][observation]") {
    Logger::initialize("error");
    Observation o;
    o.observation_id = "x"; o.source = "s"; o.detector_id = "d";
    // A single huge string value blows the byte budget.
    o.features = {{"big", std::string(Observation::kMaxFeatureJsonBytes + 10, 'a')}};
    REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);
}

TEST_CASE("Observation: explicit status distinguishes detected/not/unavailable/inconclusive",
          "[detection][observation]") {
    Logger::initialize("error");
    Observation o;
    o.observation_id = "x"; o.source = "s"; o.detector_id = "d";
    o.status = DetectorStatus::detected;
    REQUIRE(o.to_event().get_payload()["status"] == "detected");
    o.status = DetectorStatus::not_detected;
    REQUIRE(o.to_event().get_payload()["status"] == "not_detected");
    o.status = DetectorStatus::unavailable;
    REQUIRE(o.to_event().get_payload()["status"] == "unavailable");
    o.status = DetectorStatus::inconclusive;
    REQUIRE(o.to_event().get_payload()["status"] == "inconclusive");
}

// ===========================================================================
// AnomalyDetector — explainable statistical anomalies
// ===========================================================================

TEST_CASE("Anomaly: warm-up builds baseline and never flags",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector::Config cfg; cfg.min_samples = 5;
    AnomalyDetector det(cfg);
    for (int i = 0; i < 5; ++i) {
        auto r = det.record("k", 10.0, T(1000 + i));
        REQUIRE_FALSE(r.anomalous);  // warm-up
    }
    // A wildly different value after warm-up is flagged high, with a reason.
    auto r = det.record("k", 1000.0, T(2000));
    REQUIRE(r.anomalous);
    REQUIRE(r.kind == AnomalyKind::high);
    REQUIRE_FALSE(r.reason.empty());  // explainable
}

TEST_CASE("Anomaly: low deviation is flagged low",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector::Config cfg; cfg.min_samples = 5;
    AnomalyDetector det(cfg);
    for (int i = 0; i < 8; ++i) det.record("k", 100.0, T(1000 + i));
    auto r = det.record("k", 0.0, T(2000));
    REQUIRE(r.anomalous);
    REQUIRE(r.kind == AnomalyKind::low);
}

TEST_CASE("Anomaly: in-baseline value is not flagged",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector det;
    for (int i = 0; i < 20; ++i) det.record("k", 50.0 + (i % 3), T(1000 + i));
    auto r = det.record("k", 51.0, T(3000));
    REQUIRE_FALSE(r.anomalous);
    REQUIRE(r.kind == AnomalyKind::none);
}

TEST_CASE("Anomaly: missing observation is surfaced",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector::Config cfg; cfg.max_gap = std::chrono::seconds(60);
    AnomalyDetector det(cfg);
    det.record("k", 5.0, T(1000));
    // Within the gap: not missing.
    auto ok = det.check_missing("k", T(1000 + 30000));
    REQUIRE_FALSE(ok.anomalous);
    // Beyond the gap: missing.
    auto miss = det.check_missing("k", T(1000 + 120000));
    REQUIRE(miss.anomalous);
    REQUIRE(miss.kind == AnomalyKind::missing);
}

TEST_CASE("Anomaly: bounded keys and window (memory safety)",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector::Config cfg; cfg.max_keys = 4; cfg.max_window = 3; cfg.min_samples = 2;
    AnomalyDetector det(cfg);
    // Exceed max_keys: oldest evicted, table stays bounded.
    for (int i = 0; i < 10; ++i) det.record("key" + std::to_string(i), 1.0, T(1000 + i));
    REQUIRE(det.tracked_keys() <= cfg.max_keys);
    // Exceed max_window: sample_count stays bounded.
    for (int i = 0; i < 20; ++i) det.record("solo", 1.0, T(2000 + i));
    REQUIRE(det.sample_count("solo") <= cfg.max_window);
}

TEST_CASE("Anomaly: reset clears state",
          "[detection][anomaly]") {
    Logger::initialize("error");
    AnomalyDetector det;
    det.record("k", 1.0, T(1000));
    REQUIRE(det.tracked_keys() == 1);
    det.reset();
    REQUIRE(det.tracked_keys() == 0);
}

// ===========================================================================
// FusionEngine — explainable multimodal fusion
// ===========================================================================

namespace {
Observation mk_obs(const std::string& id, DetectorStatus st, std::optional<double> conf,
                   long long ts_ms) {
    Observation o;
    o.observation_id = id; o.source = "sim"; o.detector_id = id;
    o.status = st; o.confidence = conf; o.observation_timestamp = T(ts_ms);
    return o;
}
}

TEST_CASE("Fusion: agreeing detectors yield detected with propagated confidence",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine f;
    std::vector<Observation> obs = {
        mk_obs("a", DetectorStatus::detected, 0.9, 1000),
        mk_obs("b", DetectorStatus::detected, 0.8, 1000),
    };
    auto r = f.fuse(obs);
    REQUIRE(r.verdict == FusionVerdict::detected);
    REQUIRE(r.contributing == 2);
    REQUIRE(r.confidence > 0.0);
    REQUIRE(r.confidence <= 0.9);  // never exceeds mean input confidence
    REQUIRE_FALSE(r.reason.empty());
}

TEST_CASE("Fusion: conflicting detectors yield inconclusive (no silent winner)",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine::Config cfg; cfg.min_contributing = 2; cfg.agreement_threshold = 0.6;
    FusionEngine f(cfg);
    std::vector<Observation> obs = {
        mk_obs("a", DetectorStatus::detected, 0.9, 1000),
        mk_obs("b", DetectorStatus::not_detected, 0.9, 1000),
    };
    auto r = f.fuse(obs);
    REQUIRE(r.verdict == FusionVerdict::inconclusive);
}

TEST_CASE("Fusion: unavailable inputs never count as detection and lower confidence",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine f;
    std::vector<Observation> obs = {
        mk_obs("a", DetectorStatus::detected, 0.9, 1000),
        mk_obs("b", DetectorStatus::detected, 0.9, 1000),
        mk_obs("c", DetectorStatus::unavailable, std::nullopt, 1000),
    };
    auto r = f.fuse(obs);
    REQUIRE(r.verdict == FusionVerdict::detected);
    REQUIRE(r.contributing == 2);       // unavailable not counted as a vote
    // Compare against the same two inputs without the unavailable one.
    std::vector<Observation> obs2 = {
        mk_obs("a", DetectorStatus::detected, 0.9, 1000),
        mk_obs("b", DetectorStatus::detected, 0.9, 1000),
    };
    auto r2 = f.fuse(obs2);
    REQUIRE(r.confidence < r2.confidence);  // unusable input dilutes confidence
}

TEST_CASE("Fusion: too few usable inputs yields unavailable",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine f;
    std::vector<Observation> obs = { mk_obs("a", DetectorStatus::detected, 0.9, 1000) };
    auto r = f.fuse(obs);
    REQUIRE(r.verdict == FusionVerdict::unavailable);
}

TEST_CASE("Fusion: stale inputs outside the window are excluded",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine::Config cfg; cfg.window = std::chrono::milliseconds(5000);
    FusionEngine f(cfg);
    std::vector<Observation> obs = {
        mk_obs("old", DetectorStatus::detected, 0.9, 0),      // far in the past
        mk_obs("new1", DetectorStatus::detected, 0.9, 100000),
        mk_obs("new2", DetectorStatus::detected, 0.9, 100000),
    };
    auto r = f.fuse(obs);
    REQUIRE(r.verdict == FusionVerdict::detected);
    REQUIRE(r.contributing == 2);  // the stale one excluded
}

TEST_CASE("Fusion: empty input yields unavailable",
          "[detection][fusion]") {
    Logger::initialize("error");
    FusionEngine f;
    auto r = f.fuse({});
    REQUIRE(r.verdict == FusionVerdict::unavailable);
}

// ===========================================================================
// SyntheticActivityDetector — deterministic synthetic inference
// ===========================================================================

TEST_CASE("SyntheticDetector: detects above threshold, not below",
          "[detection][synthetic]") {
    Logger::initialize("error");
    SyntheticActivityDetector d;
    auto hi = d.detect("o1", 0.8);
    REQUIRE(hi.status == DetectorStatus::detected);
    REQUIRE(hi.provenance == Provenance::synthetic);
    auto lo = d.detect("o2", 0.2);
    REQUIRE(lo.status == DetectorStatus::not_detected);
}

TEST_CASE("SyntheticDetector: near-threshold input is inconclusive (not fabricated)",
          "[detection][synthetic]") {
    Logger::initialize("error");
    SyntheticActivityDetector d;  // threshold 0.5, min_quality 0.3
    auto mid = d.detect("o1", 0.52);  // very close to threshold -> low quality
    REQUIRE(mid.status == DetectorStatus::inconclusive);
}

TEST_CASE("SyntheticDetector: out-of-range input is clamped and inconclusive",
          "[detection][synthetic]") {
    Logger::initialize("error");
    SyntheticActivityDetector d;
    auto over = d.detect("o1", 5.0);
    REQUIRE(over.status == DetectorStatus::inconclusive);
    REQUIRE(over.features["energy"] == Approx(1.0));  // clamped
    auto under = d.detect("o2", -3.0);
    REQUIRE(under.status == DetectorStatus::inconclusive);
    REQUIRE(under.features["energy"] == Approx(0.0));
}

TEST_CASE("SyntheticDetector: output flows through the Event pipeline unchanged",
          "[detection][synthetic]") {
    Logger::initialize("error");
    SyntheticActivityDetector d;
    auto o = d.detect("o1", 0.9);
    Event e = o.to_event();
    REQUIRE(e.get_type() == EventType::inference);
    REQUIRE(e.get_payload()["provenance"] == "synthetic");
    REQUIRE(e.get_payload()["model_id"] == "synthetic-v1");
    // No raw media / embedding / transcript fields anywhere in the payload.
    const std::string dumped = e.get_payload().dump();
    REQUIRE(dumped.find("audio") == std::string::npos);
    REQUIRE(dumped.find("video") == std::string::npos);
    REQUIRE(dumped.find("embedding") == std::string::npos);
    REQUIRE(dumped.find("transcript") == std::string::npos);
}
