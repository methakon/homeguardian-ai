#include "catch2/catch.hpp"

#include "core/Observation.h"
#include "core/SecurityObservation.h"

#include <chrono>
#include <string>

using namespace homeguardian;

namespace {

SecurityObservation make_observation(SecuritySourceType type, const std::string& state) {
    const auto now = std::chrono::system_clock::now();
    SecurityObservation o;
    o.device_id = "node:hg-0123456789abcdef";
    o.source = "node";
    o.source_type = type;
    o.acquired_at = now;
    o.ingested_at = now;
    o.provenance = Provenance::real;
    o.state = state;
    o.consent_subject = "node:hg-0123456789abcdef";
    o.consent_purpose = "security_monitor";
    o.authorized = true;
    o.features = nlohmann::json::object();
    o.features["counter"] = 12;
    return o;
}

}  // namespace

TEST_CASE("security observations are bounded, validated and deterministic",
          "[sensors][observation]") {
    const auto now = std::chrono::system_clock::now();

    SECTION("every required field is present for the pipeline") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.validate();  // must not throw
        const Observation observation = o.to_observation();
        REQUIRE(observation.source == o.device_id);
        REQUIRE(observation.detector_id == "sensor.motion");
        REQUIRE(observation.provenance == Provenance::real);
        REQUIRE(observation.status == DetectorStatus::detected);
        // Device identity, source type, both timestamps, provenance and the
        // authorization context all survive into the pipeline record.
        REQUIRE(observation.features["source_type"] == "motion");
        REQUIRE(observation.features["authorized"] == true);
        REQUIRE(observation.features["consent_purpose"] == "security_monitor");
        REQUIRE(observation.features.contains("acquired_at_ms"));
        REQUIRE(observation.features.contains("ingested_at_ms"));
        REQUIRE(observation.features["producer"] == "node");
    }

    SECTION("the observation id is deterministic so duplicates are detectable") {
        SecurityObservation a = make_observation(SecuritySourceType::contact, "open");
        SecurityObservation b = a;
        REQUIRE(a.to_observation().observation_id == b.to_observation().observation_id);
    }

    SECTION("acquired_at after ingested_at is rejected") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.ingested_at = now - std::chrono::seconds(5);
        REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);
    }

    SECTION("missing timestamps are rejected") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.acquired_at = {};
        REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);
        SecurityObservation p = make_observation(SecuritySourceType::motion, "on");
        p.ingested_at = {};
        REQUIRE_THROWS_AS(p.validate(), std::invalid_argument);
    }

    SECTION("bounded strings and payloads are enforced") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.device_id = std::string(SecurityObservation::kMaxDeviceIdChars + 1, 'x');
        REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);

        SecurityObservation p = make_observation(SecuritySourceType::motion, "on");
        p.state = std::string(SecurityObservation::kMaxStateChars + 1, 'x');
        REQUIRE_THROWS_AS(p.validate(), std::invalid_argument);

        SecurityObservation q = make_observation(SecuritySourceType::motion, "on");
        q.note = std::string(SecurityObservation::kMaxNoteChars + 1, 'x');
        REQUIRE_THROWS_AS(q.validate(), std::invalid_argument);

        SecurityObservation r = make_observation(SecuritySourceType::motion, "on");
        for (size_t i = 0; i <= SecurityObservation::kMaxFeatureKeys; ++i) {
            r.features["k" + std::to_string(i)] = i;
        }
        REQUIRE_THROWS_AS(r.validate(), std::invalid_argument);

        SecurityObservation s = make_observation(SecuritySourceType::motion, "on");
        s.features["blob"] = std::string(4096, 'z');
        REQUIRE_THROWS_AS(s.validate(), std::invalid_argument);
    }

    SECTION("confidence and quality stay in range") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.confidence = 1.5;
        REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);
        SecurityObservation p = make_observation(SecuritySourceType::motion, "on");
        p.quality = -0.1;
        REQUIRE_THROWS_AS(p.validate(), std::invalid_argument);
    }

    SECTION("an unauthorized report claims no detection") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.authorized = false;
        const Observation observation = o.to_observation();
        REQUIRE(observation.status == DetectorStatus::unavailable);
        REQUIRE(observation.features["authorized"] == false);
    }

    SECTION("an empty device id or source is rejected") {
        SecurityObservation o = make_observation(SecuritySourceType::motion, "on");
        o.device_id.clear();
        REQUIRE_THROWS_AS(o.validate(), std::invalid_argument);
        SecurityObservation p = make_observation(SecuritySourceType::motion, "on");
        p.source.clear();
        REQUIRE_THROWS_AS(p.validate(), std::invalid_argument);
    }
}

TEST_CASE("security source types map states to detector status explicitly", "[sensors][mapping]") {
    SECTION("motion") {
        REQUIRE(make_observation(SecuritySourceType::motion, "on").to_observation().status ==
                DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::motion, "clear").to_observation().status ==
                DetectorStatus::not_detected);
        REQUIRE(make_observation(SecuritySourceType::motion, "sideways").to_observation().status ==
                DetectorStatus::inconclusive);
    }
    SECTION("contact") {
        REQUIRE(make_observation(SecuritySourceType::contact, "open").to_observation().status ==
                DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::contact, "closed").to_observation().status ==
                DetectorStatus::not_detected);
    }
    SECTION("presence") {
        REQUIRE(make_observation(SecuritySourceType::presence, "present").to_observation().status ==
                DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::presence, "absent").to_observation().status ==
                DetectorStatus::not_detected);
    }
    SECTION("availability: the DETECTED target is the problem") {
        REQUIRE(make_observation(SecuritySourceType::device_availability, "offline")
                    .to_observation()
                    .status == DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::device_availability, "online")
                    .to_observation()
                    .status == DetectorStatus::not_detected);
        REQUIRE(make_observation(SecuritySourceType::camera_availability, "unavailable")
                    .to_observation()
                    .status == DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::microphone_availability, "available")
                    .to_observation()
                    .status == DetectorStatus::not_detected);
    }
    SECTION("tamper and threshold crossing") {
        REQUIRE(make_observation(SecuritySourceType::tamper, "unexpected_disconnect")
                    .to_observation()
                    .status == DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::tamper, "clear").to_observation().status ==
                DetectorStatus::not_detected);
        REQUIRE(make_observation(SecuritySourceType::environmental_threshold, "crossed")
                    .to_observation()
                    .status == DetectorStatus::detected);
        REQUIRE(make_observation(SecuritySourceType::environmental_threshold, "within")
                    .to_observation()
                    .status == DetectorStatus::not_detected);
    }
    SECTION("the detector id carries the source type") {
        REQUIRE(make_observation(SecuritySourceType::contact, "open").to_observation().detector_id ==
                "sensor.contact");
        REQUIRE(make_observation(SecuritySourceType::tamper, "clear").to_observation().detector_id ==
                "sensor.tamper");
    }
}

TEST_CASE("no sensor report can ever suggest a critical severity", "[sensors][severity]") {
    const SecuritySourceType types[] = {
        SecuritySourceType::motion,        SecuritySourceType::contact,
        SecuritySourceType::presence,      SecuritySourceType::camera_availability,
        SecuritySourceType::microphone_availability, SecuritySourceType::device_availability,
        SecuritySourceType::tamper,        SecuritySourceType::environmental_threshold};
    const char* states[] = {"on", "off", "open", "closed", "present", "absent",
                            "online", "offline", "unavailable", "available",
                            "tamper", "clear", "crossed", "within", "sideways"};

    for (SecuritySourceType type : types) {
        for (const char* state : states) {
            SecurityObservation o = make_observation(type, state);
            const Severity s = o.suggested_severity();
            REQUIRE(s != Severity::critical);
            REQUIRE((s == Severity::info || s == Severity::warning));
            // And unauthorized reports are always informational.
            o.authorized = false;
            REQUIRE(o.suggested_severity() == Severity::info);
        }
    }

    SECTION("tamper and loss of a sensor are warnings, routine events are informational") {
        REQUIRE(make_observation(SecuritySourceType::tamper, "tamper").suggested_severity() ==
                Severity::warning);
        REQUIRE(make_observation(SecuritySourceType::device_availability, "offline")
                    .suggested_severity() == Severity::warning);
        REQUIRE(make_observation(SecuritySourceType::camera_availability, "unavailable")
                    .suggested_severity() == Severity::warning);
        REQUIRE(make_observation(SecuritySourceType::motion, "on").suggested_severity() ==
                Severity::info);
        REQUIRE(make_observation(SecuritySourceType::contact, "open").suggested_severity() ==
                Severity::info);
    }
}

TEST_CASE("security observations round-trip through a strict wire format", "[sensors][json]") {
    const SecurityObservation original = make_observation(SecuritySourceType::contact, "open");

    SECTION("to_json -> from_json preserves every ingestible field") {
        const nlohmann::json wire = SecurityObservation::to_json(original);
        std::string why;
        const auto parsed = SecurityObservation::from_json(wire, &why);
        REQUIRE(parsed.has_value());
        REQUIRE(why.empty());
        REQUIRE(parsed->device_id == original.device_id);
        REQUIRE(parsed->source_type == original.source_type);
        REQUIRE(parsed->state == original.state);
        REQUIRE(parsed->provenance == original.provenance);
        REQUIRE(parsed->authorized == original.authorized);
        REQUIRE(parsed->consent_purpose == original.consent_purpose);
        REQUIRE(parsed->features["counter"] == 12);
        // The wire format carries epoch MILLISECONDS, so the round trip is exact
        // to a millisecond rather than to the nanosecond clock resolution.
        const auto original_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            original.acquired_at.time_since_epoch());
        const auto parsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            parsed->acquired_at.time_since_epoch());
        REQUIRE(parsed_ms == original_ms);
    }

    SECTION("malformed payloads are rejected with a reason, never defaulted") {
        std::string why;
        REQUIRE_FALSE(SecurityObservation::from_json(nlohmann::json::array(), &why).has_value());
        REQUIRE(why.find("not an object") != std::string::npos);

        nlohmann::json missing = SecurityObservation::to_json(original);
        missing.erase("device_id");
        REQUIRE_FALSE(SecurityObservation::from_json(missing, &why).has_value());
        REQUIRE(why.find("device_id") != std::string::npos);

        nlohmann::json unknown_type = SecurityObservation::to_json(original);
        unknown_type["source_type"] = "mind_reading";
        REQUIRE_FALSE(SecurityObservation::from_json(unknown_type, &why).has_value());
        REQUIRE(why.find("unknown source_type") != std::string::npos);

        nlohmann::json bad_provenance = SecurityObservation::to_json(original);
        bad_provenance["provenance"] = "guessed";
        REQUIRE_FALSE(SecurityObservation::from_json(bad_provenance, &why).has_value());
        REQUIRE(why.find("provenance") != std::string::npos);

        nlohmann::json bad_time = SecurityObservation::to_json(original);
        bad_time["acquired_at_ms"] = "yesterday";
        REQUIRE_FALSE(SecurityObservation::from_json(bad_time, &why).has_value());
        REQUIRE(why.find("acquired_at_ms") != std::string::npos);

        nlohmann::json missing_time = SecurityObservation::to_json(original);
        missing_time.erase("acquired_at_ms");
        REQUIRE_FALSE(SecurityObservation::from_json(missing_time, &why).has_value());

        nlohmann::json too_long = SecurityObservation::to_json(original);
        too_long["device_id"] = std::string(SecurityObservation::kMaxDeviceIdChars + 5, 'd');
        REQUIRE_FALSE(SecurityObservation::from_json(too_long, &why).has_value());
        REQUIRE(why.find("validation failed") != std::string::npos);

        nlohmann::json bad_features = SecurityObservation::to_json(original);
        bad_features["features"] = "not an object";
        REQUIRE_FALSE(SecurityObservation::from_json(bad_features, &why).has_value());
    }

    SECTION("an ingested payload is bounded even when the sender is not") {
        nlohmann::json hostile = SecurityObservation::to_json(original);
        hostile["note"] = std::string(5000, 'n');
        std::string why;
        REQUIRE_FALSE(SecurityObservation::from_json(hostile, &why).has_value());
    }
}

TEST_CASE("device availability distinguishes a dead sensor from a silent one",
          "[sensors][availability]") {
    const auto t0 = std::chrono::system_clock::now();
    DeviceAvailabilityTracker tracker;

    SECTION("first sight produces exactly one transition") {
        const auto first = tracker.mark_seen("node:hg-1", t0);
        REQUIRE(first.has_value());
        REQUIRE(first->online);
        REQUIRE(first->first_seen);
        REQUIRE(tracker.online("node:hg-1"));
    }

    SECTION("a healthy device is silent - no event per heartbeat") {
        tracker.mark_seen("node:hg-1", t0);
        for (int i = 1; i <= 20; ++i) {
            REQUIRE_FALSE(tracker.mark_seen("node:hg-1", t0 + std::chrono::seconds(i)).has_value());
        }
    }

    SECTION("silence past the timeout yields ONE offline transition") {
        tracker.mark_seen("node:hg-1", t0);
        REQUIRE(tracker.sweep(t0 + std::chrono::seconds(30)).empty());
        const auto offline = tracker.sweep(t0 + std::chrono::seconds(120));
        REQUIRE(offline.size() == 1);
        REQUIRE_FALSE(offline[0].online);
        REQUIRE(offline[0].device_id == "node:hg-1");
        REQUIRE(offline[0].reason.find("no contact") != std::string::npos);
        REQUIRE(tracker.sweep(t0 + std::chrono::seconds(200)).empty());  // reported once
    }

    SECTION("a reconnect is an availability transition only, never a state event") {
        tracker.mark_seen("node:hg-1", t0);
        tracker.sweep(t0 + std::chrono::seconds(120));
        const auto back = tracker.mark_seen("node:hg-1", t0 + std::chrono::seconds(130));
        REQUIRE(back.has_value());
        REQUIRE(back->online);
        REQUIRE_FALSE(back->first_seen);
        REQUIRE(back->reason.find("availability") != std::string::npos);
        REQUIRE(back->reason.find("no state event") != std::string::npos);
    }

    SECTION("an unknown device is not assumed online") {
        REQUIRE_FALSE(tracker.online("never-seen"));
    }

    SECTION("the registry is bounded and evicts the least recently seen device") {
        DeviceAvailabilityTracker::Config cfg;
        cfg.max_devices = 3;
        DeviceAvailabilityTracker bounded(cfg);
        for (int i = 0; i < 10; ++i) {
            bounded.mark_seen("node:" + std::to_string(i), t0 + std::chrono::seconds(i));
        }
        REQUIRE(bounded.device_count() <= 3);
        REQUIRE(bounded.online("node:9"));
        REQUIRE_FALSE(bounded.online("node:0"));
    }

    SECTION("an empty device id is ignored") {
        REQUIRE_FALSE(tracker.mark_seen("", t0).has_value());
        REQUIRE(tracker.device_count() == 0);
    }
}

TEST_CASE("sensor de-duplication is time-bounded and cannot grow without limit",
          "[sensors][dedup]") {
    const auto t0 = std::chrono::system_clock::now();
    SensorDeduplicator dedup;

    SensorDeduplicator::Key key;
    key.device_id = "node:hg-1";
    key.source_type = SecuritySourceType::motion;
    key.state = "on";

    SECTION("the first report passes and the immediate repeat is suppressed") {
        REQUIRE_FALSE(dedup.duplicate(key, t0));
        REQUIRE(dedup.duplicate(key, t0 + std::chrono::seconds(5)));
        REQUIRE(dedup.duplicate(key, t0 + std::chrono::seconds(59)));
    }

    SECTION("the same report passes again after the cooldown") {
        REQUIRE_FALSE(dedup.duplicate(key, t0));
        REQUIRE_FALSE(dedup.duplicate(key, t0 + std::chrono::seconds(61)));
    }

    SECTION("a different state or source is a different key") {
        REQUIRE_FALSE(dedup.duplicate(key, t0));
        SensorDeduplicator::Key other_state = key;
        other_state.state = "off";
        REQUIRE_FALSE(dedup.duplicate(other_state, t0 + std::chrono::seconds(1)));
        SensorDeduplicator::Key other_type = key;
        other_type.source_type = SecuritySourceType::contact;
        REQUIRE_FALSE(dedup.duplicate(other_type, t0 + std::chrono::seconds(1)));
    }

    SECTION("the table is bounded") {
        SensorDeduplicator::Config cfg;
        cfg.max_entries = 5;
        SensorDeduplicator bounded(cfg);
        for (int i = 0; i < 50; ++i) {
            SensorDeduplicator::Key k = key;
            k.device_id = "node:" + std::to_string(i);
            bounded.duplicate(k, t0 + std::chrono::seconds(i));
        }
        REQUIRE(bounded.size() <= 5);
    }
}

TEST_CASE("security source types parse strictly", "[sensors][parsing]") {
    REQUIRE(security_source_type_from_string("motion").value() == SecuritySourceType::motion);
    REQUIRE(security_source_type_from_string("environmental_threshold").value() ==
            SecuritySourceType::environmental_threshold);
    REQUIRE_FALSE(security_source_type_from_string("emotion").has_value());
    REQUIRE_FALSE(security_source_type_from_string("").has_value());
    REQUIRE(std::string(security_source_type_to_string(SecuritySourceType::tamper)) == "tamper");
}
