// test_fusion_provenance: a fused result and the alert built from it must state the provenance of the
// EVIDENCE, never a fixed string.
//
// The defect this covers: DetectionFusionRule stamped `{"provenance", "synthetic"}` into every alert,
// which was true while the only inputs were the simulator's. With a physical-camera observation in the
// window the same field became a false statement. Provenance is now derived from the contributing
// observations inside FusionEngine, and these tests pin every case that derivation can take.

#include <catch2/catch.hpp>

#include "core/DetectionFusionRule.h"
#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

constexpr const char* kDetectorId = "camera-motion-activity";
constexpr const char* kModelId = "ema-baseline-v1";

std::chrono::system_clock::time_point base_time() {
    return std::chrono::system_clock::now() - std::chrono::seconds(20);
}

Observation vote(int index, Provenance provenance, double confidence, DetectorStatus status,
                 std::chrono::system_clock::time_point now) {
    Observation o;
    o.observation_id = "prov-" + std::to_string(index);
    o.source = provenance == Provenance::real ? "/dev/video0" : "simulator";
    o.provenance = provenance;
    o.detector_id = kDetectorId;
    o.model_id = kModelId;
    o.status = status;
    o.confidence = confidence;
    o.observation_timestamp = now;
    o.features = nlohmann::json::object();
    o.features["direction"] = "rising";
    return o;
}

std::vector<Observation> window_of(size_t real_votes, size_t synthetic_votes, size_t baselines) {
    std::vector<Observation> obs;
    const auto base = base_time();
    int i = 0;
    for (size_t k = 0; k < real_votes; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(vote(i++, Provenance::real, 0.9, DetectorStatus::detected, at));
    }
    for (size_t k = 0; k < synthetic_votes; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(vote(i++, Provenance::synthetic, 0.9, DetectorStatus::detected, at));
    }
    for (size_t k = 0; k < baselines; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(vote(i++, Provenance::synthetic, 0.0, DetectorStatus::not_detected, at));
    }
    return obs;
}

std::vector<Event> as_events(const std::vector<Observation>& obs) {
    std::vector<Event> events;
    for (const auto& o : obs) events.push_back(o.to_event());
    return events;
}

DetectionFusionRule camera_rule() {
    DetectionFusionRule rule("detection-fusion");
    rule.set_evidence_detector_ids({kDetectorId});
    return rule;
}

}  // namespace

TEST_CASE("provenance derivation: the four cases and nothing else", "[fusion][provenance]") {
    using EP = EvidenceProvenance;
    REQUIRE(derive_evidence_provenance(0, 0) == EP::uncertain);
    REQUIRE(derive_evidence_provenance(3, 0) == EP::real);
    REQUIRE(derive_evidence_provenance(0, 3) == EP::synthetic);
    REQUIRE(derive_evidence_provenance(1, 1) == EP::mixed);
    REQUIRE(derive_evidence_provenance(4, 2) == EP::mixed);
    // The fail-safe default: a result that was never computed cannot read as real evidence.
    REQUIRE(FusionResult{}.provenance == EP::uncertain);
    REQUIRE(std::string(evidence_provenance_to_string(EP::mixed)) == "mixed");
}

TEST_CASE("fusion engine: provenance follows the contributing observations", "[fusion][provenance]") {
    const FusionEngine engine;

    const auto real_only = engine.fuse(window_of(2, 0, 0));
    REQUIRE(real_only.verdict == FusionVerdict::detected);
    REQUIRE(real_only.provenance == EvidenceProvenance::real);
    REQUIRE(real_only.real_inputs == 2);
    REQUIRE(real_only.synthetic_inputs == 0);

    const auto synthetic_only = engine.fuse(window_of(0, 2, 0));
    REQUIRE(synthetic_only.provenance == EvidenceProvenance::synthetic);
    REQUIRE(synthetic_only.real_inputs == 0);

    const auto mixed = engine.fuse(window_of(1, 1, 0));
    REQUIRE(mixed.provenance == EvidenceProvenance::mixed);
    REQUIRE(mixed.real_inputs == 1);
    REQUIRE(mixed.synthetic_inputs == 1);
}

TEST_CASE("fusion engine: non-contributing evidence is not counted as provenance",
          "[fusion][provenance]") {
    // A real observation that is too old to fuse must not make the result "real". Only the votes
    // count, which is the whole point of deriving provenance where the votes are collected.
    const FusionEngine engine;
    std::vector<Observation> obs = window_of(0, 2, 0);
    Observation stale = vote(99, Provenance::real, 0.9, DetectorStatus::detected,
                             base_time() - std::chrono::hours(2));
    obs.push_back(stale);

    const auto fused = engine.fuse(obs);
    INFO("fused confidence=" << fused.confidence << " provenance="
                             << evidence_provenance_to_string(fused.provenance));
    REQUIRE(fused.provenance == EvidenceProvenance::synthetic);
    REQUIRE(fused.real_inputs == 0);
}

TEST_CASE("alert provenance: synthetic-only evidence keeps the exact legacy wording",
          "[fusion][provenance][alert]") {
    const auto alert = camera_rule().evaluate(as_events(window_of(0, 2, 0)));
    REQUIRE(alert.has_value());
    REQUIRE(alert->details["provenance"] == "synthetic");
    REQUIRE(alert->details["provenance_derived"] == true);
    REQUIRE(alert->details["evidence_mode"] == "fusion");
    REQUIRE(alert->details["provenance_sources"]["real"] == 0);
    REQUIRE(alert->details["provenance_sources"]["synthetic"] == 2);
    // Documentation and existing assertions quote this wording; it must not drift.
    REQUIRE(alert->message.find("synthetic activity detected: fusion confidence") == 0);
}

TEST_CASE("alert provenance: real-only evidence is reported as real, never as synthetic",
          "[fusion][provenance][alert]") {
    const auto alert = camera_rule().evaluate(as_events(window_of(2, 0, 0)));
    REQUIRE(alert.has_value());
    REQUIRE(alert->details["provenance"] == "real");
    REQUIRE(alert->details["provenance_sources"]["real"] == 2);
    REQUIRE(alert->details["provenance_sources"]["synthetic"] == 0);
    REQUIRE(alert->message.find("real-sensor activity detected") == 0);
    REQUIRE(alert->message.find("synthetic") == std::string::npos);
}

TEST_CASE("alert provenance: mixed evidence is stated as mixed, not as either extreme",
          "[fusion][provenance][alert]") {
    const auto alert = camera_rule().evaluate(as_events(window_of(1, 1, 0)));
    REQUIRE(alert.has_value());
    REQUIRE(alert->details["provenance"] == "mixed");
    REQUIRE(alert->details["provenance_sources"]["real"] == 1);
    REQUIRE(alert->details["provenance_sources"]["synthetic"] == 1);
    REQUIRE(alert->message.find("mixed-provenance activity detected") == 0);
}

TEST_CASE("alert provenance: a synthetic window can never produce a real claim",
          "[fusion][provenance][alert]") {
    // The one-way door, asserted directly: no amount of synthetic evidence yields "real".
    for (size_t synthetic_votes : {2u, 3u, 5u}) {
        const auto alert = camera_rule().evaluate(as_events(window_of(0, synthetic_votes, 0)));
        REQUIRE(alert.has_value());
        REQUIRE(alert->details["provenance"] != "real");
        REQUIRE(alert->details["provenance"] == "synthetic");
    }
}

TEST_CASE("alert provenance: no usable evidence means no alert at all", "[fusion][provenance]") {
    // Nothing to fuse -> `uncertain` provenance and, separately, no alert. An uncertain result is
    // never promoted to a confirmation.
    const FusionEngine engine;
    std::vector<Observation> obs;
    const auto base = base_time();
    obs.push_back(vote(1, Provenance::real, 0.9, DetectorStatus::inconclusive, base));
    obs.push_back(vote(2, Provenance::real, 0.9, DetectorStatus::inconclusive,
                       base + std::chrono::milliseconds(10)));

    const auto fused = engine.fuse(obs);
    REQUIRE(fused.verdict == FusionVerdict::unavailable);
    REQUIRE(fused.provenance == EvidenceProvenance::uncertain);
    REQUIRE_FALSE(camera_rule().evaluate(as_events(obs)).has_value());
}

TEST_CASE("provenance is carried into the event payload unchanged", "[fusion][provenance]") {
    // The observation's own label is the input to the derivation; nothing downstream rewrites it.
    const auto obs = window_of(1, 1, 0);
    const auto events = as_events(obs);
    REQUIRE(events.at(0).get_payload()["provenance"] == "real");
    REQUIRE(events.at(1).get_payload()["provenance"] == "synthetic");
}
