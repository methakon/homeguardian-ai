// test_sensor_event_consumer: the boundary where another platform's observations become evidence.
//
// Each test pins one promise of docs/SENSOR_EVENT_CONTRACT.md: schema and types, source identity,
// timestamps, replay, consent, provenance integrity, offline behaviour, and that what IS accepted reaches
// the fusion engine intact. The fixtures are synthetic JSON, so the ground truth is exact.

#include <catch2/catch.hpp>

#include "core/SensorEventConsumer.h"
#include "core/Observation.h"

#include <chrono>
#include <string>

using namespace homeguardian;

namespace {

auto now() { return std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)); }

double epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

nlohmann::json observation_json(const std::string& id, const std::string& source = "device:/dev/video0",
                                const std::string& provenance = "real",
                                const std::string& status = "detected",
                                std::chrono::system_clock::time_point at = now()) {
    return nlohmann::json{
        {"type", "inference"},
        {"schema_version", 1},
        {"observation_id", id},
        {"source", source},
        {"provenance", provenance},
        {"status", status},
        {"detector_id", "camera-motion-activity"},
        {"model_id", "rms-v1"},
        {"observed_at", epoch(at)},
        {"confidence", 0.8},
        {"consent", "granted"},
        {"features", {{"motion_level", 0.3}, {"mean_delta", 0.05}}},
    };
}

SensorEventConsumer consumer(SensorEventConsumer::Config cfg = SensorEventConsumer::Config{}) {
    if (cfg.trusted_source_prefixes.empty()) cfg.trusted_source_prefixes = {"device:", "alsa:"};
    return SensorEventConsumer(cfg);
}

}  // namespace

TEST_CASE("consumer: a well-formed observation is accepted and normalized", "[consumer][contract]") {
    auto c = consumer();
    const auto result = c.ingest(observation_json("obs-1"), now());
    INFO(result.detail);
    REQUIRE(result.accepted);
    REQUIRE(result.code == IngestCode::accepted);
    REQUIRE(c.accepted() == 1);
    REQUIRE(c.window().size() == 1);
    REQUIRE(c.window().front().detector_id == "camera-motion-activity");
    REQUIRE(c.window().front().provenance == Provenance::real);
    REQUIRE(c.window().front().status == DetectorStatus::detected);
    REQUIRE(c.window().front().confidence.has_value());
    REQUIRE(c.window().front().features.contains("motion_level"));
}

TEST_CASE("consumer: the schema and field types are enforced, not assumed", "[consumer][contract]") {
    auto c = consumer();

    auto not_an_event = observation_json("obs-1");
    not_an_event["type"] = "alert";
    REQUIRE(c.ingest(not_an_event, now()).code == IngestCode::not_an_event);

    auto future_schema = observation_json("obs-2");
    future_schema["schema_version"] = 2;
    REQUIRE(c.ingest(future_schema, now()).code == IngestCode::unsupported_schema);

    auto missing_features = observation_json("obs-3");
    missing_features.erase("features");
    REQUIRE(c.ingest(missing_features, now()).code == IngestCode::malformed);

    auto wrong_type = observation_json("obs-4");
    wrong_type["confidence"] = "high";                       // a number was promised
    REQUIRE(c.ingest(wrong_type, now()).code == IngestCode::malformed);

    auto no_id = observation_json("obs-5");
    no_id.erase("observation_id");
    REQUIRE(c.ingest(no_id, now()).code == IngestCode::malformed);

    auto bad_status = observation_json("obs-6");
    bad_status["status"] = "probably";
    REQUIRE(c.ingest(bad_status, now()).code == IngestCode::malformed);

    REQUIRE(c.accepted() == 0);
    REQUIRE(c.rejected() == 6);
}

TEST_CASE("consumer: source identity is checked against what it trusts", "[consumer][contract]") {
    auto c = consumer();
    const auto untrusted = c.ingest(observation_json("obs-1", "network:kitchen"), now());
    REQUIRE(untrusted.code == IngestCode::source_identity);

    auto empty = observation_json("obs-2");
    empty["source"] = "";
    REQUIRE(c.ingest(empty, now()).code == IngestCode::source_identity);
    REQUIRE(c.accepted() == 0);
}

TEST_CASE("consumer: provenance cannot be claimed, only stated", "[consumer][contract]") {
    auto c = consumer();

    // A producer cannot claim mixed evidence: that is a property of a set of observations, derived by
    // the fusion engine from its members.
    const auto mixed = c.ingest(observation_json("obs-1", "device:/dev/video0", "mixed"), now());
    REQUIRE(mixed.code == IngestCode::provenance_invalid);
    REQUIRE(mixed.detail.find("derived by the fusion engine") != std::string::npos);

    REQUIRE(c.ingest(observation_json("obs-2", "device:/dev/video0", "verified"), now()).code ==
            IngestCode::provenance_invalid);

    // A fixture source claiming real evidence is refused rather than believed.
    // Note the order: identity is checked first, so the interesting case is a fixture on a source this
    // consumer TRUSTS claiming real evidence. That is refused on provenance, not on identity.
    REQUIRE(c.ingest(observation_json("obs-3a", "test:fixture-1", "real"), now()).code ==
            IngestCode::source_identity);
    const auto conflict = c.ingest(observation_json("obs-3", "device:fixture-cam0", "real"), now());
    REQUIRE(conflict.code == IngestCode::provenance_conflict);

    // The same source claiming synthetic evidence is honest and accepted.
    const auto honest = c.ingest(observation_json("obs-4", "device:fixture-cam0", "synthetic"), now());
    INFO(honest.detail);
    REQUIRE(honest.accepted);
    REQUIRE(c.window().front().provenance == Provenance::synthetic);
}

TEST_CASE("consumer: timestamps, reordering and replay", "[consumer][contract]") {
    auto c = consumer();

    const auto future = c.ingest(observation_json("obs-1", "device:/dev/video0", "real", "detected",
                                                 now() + std::chrono::minutes(5)), now());
    REQUIRE(future.code == IngestCode::future_timestamp);

    REQUIRE(c.ingest(observation_json("obs-2", "device:/dev/video0", "real", "detected", now()), now())
                .accepted);
    REQUIRE(c.ingest(observation_json("obs-3", "device:/dev/video0", "real", "detected",
                                      now() - std::chrono::minutes(1)), now())
                .code == IngestCode::stale_or_replay);

    // A replay of an accepted observation is refused, and is NOT fused a second time.
    const auto replay = c.ingest(observation_json("obs-2", "device:/dev/video0", "real", "detected", now()),
                                 now());
    REQUIRE(replay.code == IngestCode::duplicate);
    REQUIRE(c.accepted() == 1);
    REQUIRE(c.window().size() == 1);

    const auto no_timestamp = [&] {
        auto e = observation_json("obs-9");
        e.erase("observed_at");
        return e;
    }();
    REQUIRE(c.ingest(no_timestamp, now()).code == IngestCode::malformed);
}

TEST_CASE("consumer: consent stops capture instead of continuing", "[consumer][contract]") {
    auto c = consumer();
    auto revoked = observation_json("obs-1");
    revoked["consent"] = "revoked";
    const auto refused = c.ingest(revoked, now());
    REQUIRE(refused.code == IngestCode::consent_missing);
    REQUIRE_FALSE(c.source_available());
    REQUIRE(c.window().empty());

    // With consent not required, the same event is a plain observation.
    SensorEventConsumer::Config cfg;
    cfg.trusted_source_prefixes = {"device:"};
    cfg.require_consent = false;
    auto lax = SensorEventConsumer(cfg);
    REQUIRE(lax.ingest(revoked, now()).accepted);
}

TEST_CASE("consumer: an offline source yields no evidence, not quiet", "[consumer][contract]") {
    auto c = consumer();
    REQUIRE(c.ingest(observation_json("obs-1"), now()).accepted);

    const auto offline = c.ingest(observation_json("obs-2", "device:/dev/video0", "real", "unavailable"),
                                  now());
    REQUIRE(offline.code == IngestCode::source_offline);
    REQUIRE_FALSE(c.source_available());
    // The window is unchanged: a disconnected sensor is a gap in the evidence, never a quiet scene.
    REQUIRE(c.window().size() == 1);

    // A later observation proves the source is back.
    REQUIRE(c.ingest(observation_json("obs-3"), now()).accepted);
    REQUIRE(c.source_available());
    REQUIRE(c.window().size() == 2);
}

TEST_CASE("consumer: accepted observations reach the fusion engine intact",
          "[consumer][contract][fusion]") {
    auto c = consumer();
    REQUIRE(c.ingest(observation_json("obs-1"), now()).accepted);
    REQUIRE(c.ingest(observation_json("obs-2"), now()).accepted);

    const auto fused = c.fuse();
    REQUIRE(fused.contributing == 2);
    // Real members produce a real verdict. The consumer adds no verdict logic of its own.
    REQUIRE(fused.provenance == EvidenceProvenance::real);

    // Synthetic members produce a synthetic verdict, and the two are never confused.
    auto synthetic = consumer();
    REQUIRE(synthetic.ingest(observation_json("obs-1", "device:/dev/video0", "synthetic"), now()).accepted);
    REQUIRE(synthetic.ingest(observation_json("obs-2", "device:/dev/video0", "synthetic"), now()).accepted);
    REQUIRE(synthetic.fuse().provenance == EvidenceProvenance::synthetic);

    // An empty window produces no verdict at all rather than a default one.
    auto empty = consumer();
    REQUIRE(empty.fuse().contributing == 0);
}
