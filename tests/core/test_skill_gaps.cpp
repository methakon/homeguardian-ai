#include "catch2/catch.hpp"

#include "core/AnomalyMonitor.h"
#include "core/DetectionFusionRule.h"
#include "core/HomeAssistantAdapter.h"
#include "core/Observation.h"
#include "core/Pipeline.h"
#include "core/PrecursorDetection.h"
#include "core/SecurityObservation.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Section-J test gaps for the skills-integration layer: multi-device
// concurrency, unauthorized/withdrawn sources, missing observations, adapter
// authentication failure, and explicit resource bounds.
// ---------------------------------------------------------------------------

namespace {

class StubTransport : public IHomeAssistantTransport {
public:
    std::map<std::string, Response> responses;
    mutable std::vector<std::string> requests;
    Response get(const std::string& path) override {
        requests.push_back(path);
        const auto it = responses.find(path);
        if (it != responses.end()) return it->second;
        Response r;
        r.ok = false;
        r.status = 404;
        r.error = "not found in stub";
        return r;
    }
    std::string describe() const override { return "stub"; }
};

IHomeAssistantTransport::Response ok_json(const std::string& body) {
    IHomeAssistantTransport::Response r;
    r.ok = true;
    r.status = 200;
    r.body = body;
    return r;
}

SecurityObservation device_event(const std::string& device, SecuritySourceType type,
                                const std::string& state,
                                std::chrono::system_clock::time_point now, bool authorized = true) {
    SecurityObservation o;
    o.device_id = device;
    o.source = "node";
    o.source_type = type;
    o.acquired_at = now;
    o.ingested_at = now;
    o.provenance = Provenance::real;
    o.state = state;
    o.consent_subject = device;
    o.consent_purpose = "security_monitor";
    o.authorized = authorized;
    o.features = nlohmann::json::object();
    return o;
}

std::shared_ptr<Pipeline> make_pipeline() {
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(std::make_shared<DetectionFusionRule>("detection-fusion"));
    return std::make_shared<Pipeline>(rules);
}

}  // namespace

TEST_CASE("multiple devices report simultaneously without interfering", "[gaps][sensors]") {
    const auto t0 = std::chrono::system_clock::now();
    DeviceAvailabilityTracker tracker;

    SECTION("each device's liveness is tracked independently") {
        // The timeout is set explicitly so the precondition is deliberate: at the
        // sweep, node:c's silence is 200s and node:a/b's is 400s.
        DeviceAvailabilityTracker::Config cfg;
        cfg.offline_after = std::chrono::seconds(300);
        DeviceAvailabilityTracker tracker3(cfg);

        tracker3.mark_seen("node:a", t0);
        tracker3.mark_seen("node:b", t0);
        tracker3.mark_seen("node:c", t0);
        REQUIRE(tracker3.device_count() == 3);

        // Only two of the three go quiet.
        tracker3.mark_seen("node:c", t0 + std::chrono::seconds(200));
        const auto offline = tracker3.sweep(t0 + std::chrono::seconds(400));
        REQUIRE(offline.size() == 2);
        REQUIRE(tracker3.online("node:c"));
        REQUIRE_FALSE(tracker3.online("node:a"));
        REQUIRE_FALSE(tracker3.online("node:b"));
    }

    SECTION("the same event from different devices is not de-duplicated away") {
        SensorDeduplicator dedup;
        SensorDeduplicator::Key base;
        base.source_type = SecuritySourceType::motion;
        base.state = "on";
        for (const char* device : {"node:a", "node:b", "node:c"}) {
            SensorDeduplicator::Key key = base;
            key.device_id = device;
            REQUIRE_FALSE(dedup.duplicate(key, t0));  // every device reports once
        }
        // ...and a repeat from one device alone is still suppressed.
        SensorDeduplicator::Key repeat = base;
        repeat.device_id = "node:a";
        REQUIRE(dedup.duplicate(repeat, t0 + std::chrono::seconds(1)));
    }

    SECTION("several devices' observations coexist in one pipeline history") {
        auto pipeline = make_pipeline();
        const std::vector<SecurityObservation> events = {
            device_event("node:a", SecuritySourceType::motion, "on", t0),
            device_event("node:b", SecuritySourceType::contact, "open", t0),
            device_event("node:c", SecuritySourceType::tamper, "tamper", t0)};
        for (const SecurityObservation& o : events) {
            pipeline->process(o.to_observation().to_event());
        }
        const auto history = pipeline->get_recent_events(std::chrono::milliseconds(60000));
        REQUIRE(history.size() == 3);
        // Each record keeps its own device identity and detector.
        std::map<std::string, std::string> seen;
        for (const Event& e : history) {
            const auto back = Observation::from_event(e);
            REQUIRE(back.has_value());
            seen[back->source] = back->detector_id;
        }
        REQUIRE(seen["node:a"] == "sensor.motion");
        REQUIRE(seen["node:b"] == "sensor.contact");
        REQUIRE(seen["node:c"] == "sensor.tamper");
    }

    SECTION("no observation from any device can reach critical severity") {
        for (const char* device : {"node:a", "node:b", "node:c"}) {
            for (SecuritySourceType type :
                 {SecuritySourceType::motion, SecuritySourceType::contact,
                  SecuritySourceType::presence, SecuritySourceType::camera_availability,
                  SecuritySourceType::microphone_availability, SecuritySourceType::device_availability,
                  SecuritySourceType::tamper, SecuritySourceType::environmental_threshold}) {
                const SecurityObservation o = device_event(device, type, "tamper", t0);
                REQUIRE(o.suggested_severity() != Severity::critical);
            }
        }
    }
}

TEST_CASE("an unauthorized or withdrawn source delivers no detection", "[gaps][sensors]") {
    const auto t0 = std::chrono::system_clock::now();

    SECTION("an unauthorized report is recorded as unavailable, not as a detection") {
        const SecurityObservation o =
            device_event("node:a", SecuritySourceType::motion, "on", t0, /*authorized=*/false);
        const Observation observation = o.to_observation();
        REQUIRE(observation.status == DetectorStatus::unavailable);
        REQUIRE(observation.features["authorized"] == false);
        REQUIRE(o.suggested_severity() == Severity::info);
        // The denial travels with the record, so an audit can see it was refused.
        REQUIRE(observation.features["consent_subject"] == "node:a");
    }

    SECTION("a withdrawn source stops delivery and resumes only on re-authorization") {
        HomeAssistantConfig cfg;
        cfg.enabled = true;
        cfg.base_url = "http://127.0.0.1:8123";
        cfg.token_file = "/run/secrets/ha_token";
        cfg.entity_allowlist = {"binary_sensor.front_door"};
        cfg.allow_plaintext = true;

        auto stub = std::make_unique<StubTransport>();
        StubTransport* raw = stub.get();
        raw->responses["/api/states/binary_sensor.front_door"] =
            ok_json(R"({"entity_id":"binary_sensor.front_door","state":"on","attributes":{"device_class":"door"}})");

        HomeAssistantAdapter adapter(cfg, std::move(stub));
        adapter.set_authorized(true);
        const auto first = adapter.poll_once(t0);
        REQUIRE(first.polled);
        const size_t requests_before = raw->requests.size();
        REQUIRE(requests_before == 1);

        // Withdrawal: no further request at all.
        adapter.set_authorized(false);
        const auto withdrawn = adapter.poll_once(t0 + std::chrono::seconds(30));
        REQUIRE_FALSE(withdrawn.polled);
        REQUIRE(withdrawn.observations.empty());
        REQUIRE(raw->requests.size() == requests_before);  // nothing was asked
        REQUIRE(withdrawn.error.find("not authorized") != std::string::npos);

        // Re-authorization: reading resumes, and the gap did not fabricate events.
        adapter.set_authorized(true);
        const auto resumed = adapter.poll_once(t0 + std::chrono::seconds(60));
        REQUIRE(resumed.polled);
        REQUIRE(raw->requests.size() > requests_before);
        for (const SecurityObservation& o : resumed.observations) {
            REQUIRE(o.authorized);
            REQUIRE(o.provenance == Provenance::real);
        }
    }

    SECTION("an authentication failure is an error, never a fabricated event") {
        HomeAssistantConfig cfg;
        cfg.enabled = true;
        cfg.base_url = "http://127.0.0.1:8123";
        cfg.token_file = "/run/secrets/ha_token";
        cfg.entity_allowlist = {"binary_sensor.front_door", "camera.driveway"};
        cfg.allow_plaintext = true;

        auto stub = std::make_unique<StubTransport>();
        StubTransport* raw = stub.get();
        IHomeAssistantTransport::Response unauthorized;
        unauthorized.ok = false;
        unauthorized.status = 401;
        unauthorized.error = "unauthorized";
        raw->responses["/api/states/binary_sensor.front_door"] = unauthorized;
        raw->responses["/api/states/camera.driveway"] = unauthorized;

        HomeAssistantAdapter adapter(cfg, std::move(stub));
        adapter.set_authorized(true);
        const auto result = adapter.poll_once(t0);
        REQUIRE(result.polled);
        REQUIRE(result.errors == 2);
        REQUIRE(result.observations.empty());  // a 401 yields no observation at all
        REQUIRE(result.entities_checked == 0);
        REQUIRE(result.error.find("401") != std::string::npos);
        // The token is never echoed back in the error text.
        REQUIRE(result.error.find("Bearer") == std::string::npos);
        // And the entities are not marked alive by a failed read.
        REQUIRE_FALSE(adapter.availability().online("binary_sensor.front_door"));
    }
}

TEST_CASE("a missing observation is reported missing, not as normal", "[gaps][precursor]") {
    const auto t0 = std::chrono::system_clock::now();

    SECTION("the existing anomaly monitor reports a missing observation") {
        AnomalyMonitor::Config cfg;
        cfg.min_samples = 4;
        cfg.max_gap = std::chrono::seconds(30);
        AnomalyMonitor monitor(cfg);
        for (int i = 0; i < 6; ++i) {
            monitor.observe(0.2, t0 + std::chrono::seconds(i));
        }
        const AnomalyResult fresh = monitor.check_missing(t0 + std::chrono::seconds(10));
        REQUIRE(fresh.kind != AnomalyKind::missing);

        const AnomalyResult stale = monitor.check_missing(t0 + std::chrono::seconds(120));
        REQUIRE(stale.kind == AnomalyKind::missing);
        // A missing observation never votes: it is inconclusive, not "detected".
        REQUIRE(AnomalyMonitor::status_for(stale, cfg.min_samples) == DetectorStatus::inconclusive);
    }

    SECTION("the precursor suite degrades a stale finding to insufficient evidence") {
        PrecursorSuite::Config cfg;
        cfg.stale_after = std::chrono::seconds(60);
        PrecursorSuite suite(cfg);

        for (int i = 0; i < 12; ++i) suite.observe_motion(0.05, t0);
        for (int i = 0; i < 6; ++i) suite.observe_motion(0.40, t0);
        const PrecursorFusion fresh = suite.fusion(t0 + std::chrono::seconds(5));
        REQUIRE(fresh.label == PrecursorLabel::activity_change_detected);

        // No new observation for longer than stale_after: silence is not agreement.
        const PrecursorFusion stale = suite.fusion(t0 + std::chrono::seconds(600));
        REQUIRE(stale.label == PrecursorLabel::insufficient_evidence);
        REQUIRE(stale.explanation.find("no fresh observation") != std::string::npos);
        REQUIRE(stale.explanation.find("stale finding is not an update") != std::string::npos);

        // A single fresh observation restores normal evaluation.
        suite.observe_motion(0.40, t0 + std::chrono::seconds(601));
        REQUIRE(suite.fusion(t0 + std::chrono::seconds(602)).label ==
                PrecursorLabel::activity_change_detected);
    }

    SECTION("a suite with no observations at all reports insufficient evidence") {
        PrecursorSuite suite;
        const PrecursorFusion f = suite.fusion(t0);
        REQUIRE(f.label == PrecursorLabel::insufficient_evidence);
        REQUIRE(f.confirming_sources == 0);
        REQUIRE_FALSE(f.suppressed);
    }
}

TEST_CASE("resource bounds hold under sustained input", "[gaps][bounds]") {
    const auto t0 = std::chrono::system_clock::now();

    SECTION("the activity detector keeps its history bounded and stays correct") {
        ActivityChangeDetector detector;
        for (int i = 0; i < 5000; ++i) {
            detector.observe((i % 2 == 0) ? 0.10 : 0.12, t0 + std::chrono::seconds(i));
        }
        REQUIRE(detector.sample_count() == 5000);
        const auto snap = detector.snapshot();
        REQUIRE(snap.samples == 5000);
        REQUIRE(snap.baseline_level > 0.0);
        REQUIRE(snap.direction == ActivityChangeDetector::Direction::stable);
    }

    SECTION("the drift detector rejects oversized vectors and keeps its dimension") {
        VectorDriftDetector detector;
        const std::vector<double> big(FeatureBounds::kMaxVectorDim + 1, 1.0);
        const auto refused = detector.observe(big, t0);
        REQUIRE(refused.reason.find("too large") != std::string::npos);
        REQUIRE(detector.sample_count() == 0);

        for (int i = 0; i < 100; ++i) detector.observe({1.0, 0.0, 0.0}, t0);
        REQUIRE(detector.dim() == 3);
        REQUIRE(detector.sample_count() == 100);
    }

    SECTION("the availability registry and the deduplicator stay capped") {
        DeviceAvailabilityTracker::Config tracker_cfg;
        tracker_cfg.max_devices = 8;
        DeviceAvailabilityTracker tracker(tracker_cfg);
        for (int i = 0; i < 500; ++i) {
            tracker.mark_seen("node:" + std::to_string(i), t0 + std::chrono::seconds(i));
        }
        REQUIRE(tracker.device_count() <= 8);

        SensorDeduplicator::Config dedup_cfg;
        dedup_cfg.max_entries = 8;
        SensorDeduplicator dedup(dedup_cfg);
        for (int i = 0; i < 500; ++i) {
            SensorDeduplicator::Key key;
            key.device_id = "node:" + std::to_string(i);
            key.source_type = SecuritySourceType::motion;
            key.state = "on";
            dedup.duplicate(key, t0 + std::chrono::seconds(i));
        }
        REQUIRE(dedup.size() <= 8);
    }

    SECTION("an observation's feature payload stays bounded however it is built") {
        const SecurityObservation o = device_event("node:a", SecuritySourceType::motion, "on", t0);
        REQUIRE(o.to_observation().features.dump().size() <= Observation::kMaxFeatureJsonBytes);
        // The pipeline refuses an oversized payload rather than storing it.
        SecurityObservation hostile = o;
        hostile.features["blob"] = std::string(8192, 'x');
        REQUIRE_THROWS_AS(hostile.validate(), std::invalid_argument);
    }
}
