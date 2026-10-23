// test_producer_and_reliability: the device half of the contract, and what a hostile network does to it.
//
// TWO JOBS
//   1. The producer is a real, bounded thing (not a fixture helper): it builds schema-valid observations,
//      keeps its identity stable across a reconnect, resumes above the hub's watermark, refuses to invent
//      a message when it is paused, and never claims real provenance it cannot substantiate.
//   2. The reliability matrix: duplicate, delayed, out-of-order, reconnected, interrupted, unauthorized,
//      revoked, consent-withdrawn, drifting, unknown-type and mixed-provenance traffic - against the REAL
//      hub classes, with the disconnection case proven rather than asserted: an interruption must never
//      turn into a fabricated normal reading.

#include <catch2/catch.hpp>

#include "core/SecurityObservation.h"
#include "net/DeviceRegistry.h"
#include "net/IngestDetectors.h"
#include "net/ObservationIngest.h"
#include "net/ObservationProducer.h"
#include "net/ReplayState.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::net;

namespace {

int64_t fake_now = 1'700'000'000'000LL;
int64_t clock_ms() { return fake_now; }

std::string temp_path(const std::string& name) {
    return "/tmp/hg_prod_" + std::to_string(::getpid()) + "_" + name;
}

DeviceRegistry::Device device(const std::string& id, const std::string& credential) {
    DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "producer under test";
    d.device_type = "node";
    d.credential_id = credential;
    d.paired = true;
    d.capabilities = {"motion"};
    return d;
}

struct Hub {
    DeviceRegistry registry;
    std::string device_id = "hg-prod0000000001";
    std::string credential = "prod0000000001";

    Hub() {
        std::string why;
        REQUIRE(registry.pair(device(device_id, credential), &why));
        REQUIRE(registry.set_consent(device_id, true, device_id, "security_monitor", clock_ms()));
        REQUIRE(registry.set_capture_enabled(device_id, true));
    }

    PeerContext peer(const std::string& as_device = "", bool authenticated = true) const {
        PeerContext p;
        p.authenticated_device_id = as_device.empty() ? device_id : as_device;
        p.credential_id = authenticated ? credential : "0000000000000000";
        p.peer_authenticated = authenticated;
        p.device_paired = true;
        p.device_consent_granted = true;
        p.capture_enabled = true;
        return p;
    }
};

ObservationProducer::Config producer_config(const Hub& hub, const std::string& mode = "synthetic") {
    ObservationProducer::Config cfg;
    cfg.device_id = hub.device_id;
    cfg.credential_id = hub.credential;
    cfg.acquisition_mode = mode;
    cfg.provenance_claim = mode == "live" ? "real" : "synthetic";
    return cfg;
}

DeviceMessage hub_error(RejectReason reason, const std::string& detail) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::error;
    m.reason = reason;
    m.detail = detail;
    return m;
}

DeviceMessage hub_welcome(uint64_t resume_after) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::welcome;
    m.resume_after_sequence = resume_after;
    return m;
}

}  // namespace

// =====================================================================================================
// Task 1 - the producer
// =====================================================================================================

TEST_CASE("producer: an unusable configuration is refused before anything is emitted",
          "[producer][audit]") {
    std::string why;
    ObservationProducer::Config cfg;
    REQUIRE_FALSE(ObservationProducer(cfg, clock_ms).valid(&why));
    REQUIRE(why.find("device id") != std::string::npos);

    cfg.device_id = "hg-prod0000000001";
    REQUIRE_FALSE(ObservationProducer(cfg, clock_ms).valid(&why));
    REQUIRE(why.find("credential") != std::string::npos);

    cfg.credential_id = "prod0000000001";
    REQUIRE(ObservationProducer(cfg, clock_ms).valid(&why));

    // A synthetic acquisition claiming real provenance is refused LOCALLY: the hub would record it as
    // unsubstantiated, and a producer should not send an inconsistency it can see itself.
    cfg.acquisition_mode = "synthetic";
    cfg.provenance_claim = "real";
    REQUIRE_FALSE(ObservationProducer(cfg, clock_ms).valid(&why));
    REQUIRE(why.find("synthetic acquisition") != std::string::npos);

    cfg.acquisition_mode = "live";
    cfg.provenance_claim = "real";
    REQUIRE(ObservationProducer(cfg, clock_ms).valid(&why));
}

TEST_CASE("producer: emitted observations are schema-valid and bounded", "[producer][audit]") {
    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;
    REQUIRE(producer.next(&m, &why));
    REQUIRE(m.kind == MessageKind::observation);
    REQUIRE(m.device_id_claim == hub.device_id);
    REQUIRE(m.acquisition_mode == "synthetic");
    REQUIRE(m.provenance_claim == "synthetic");
    REQUIRE(m.sequence == 1);
    REQUIRE(m.features.contains("motion-changed-fraction"));

    // It survives the wire, strictly.
    RejectReason reason = RejectReason::bad_json;
    std::string detail;
    const auto decoded = decode(encode(m), &reason, &detail);
    REQUIRE(decoded.has_value());
    REQUIRE(reason == RejectReason::none);
    REQUIRE(decoded->sequence == 1);
    REQUIRE(decoded->observation_id == m.observation_id);

    // And it frames, so it is transmittable.
    REQUIRE(frame(encode(m)).has_value());

    // A feature set beyond the bound is refused rather than built into a huge frame.
    std::map<std::string, double> too_many;
    for (int i = 0; i < 9; ++i) too_many["k" + std::to_string(i)] = 0.1;
    REQUIRE_FALSE(producer.observe(too_many, &m, &why));
    REQUIRE(why.find("bounded maximum") != std::string::npos);
    REQUIRE(producer.stats().refused_locally == 1);
}

TEST_CASE("producer: identity and sequence are stable across a reconnect", "[producer][reconnect]") {
    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;
    REQUIRE(producer.next(&m, &why));
    REQUIRE(producer.sequence() == 2);

    // The hub welcomes us back and says it has already accepted up to 42.
    REQUIRE(producer.handle_hub_message(hub_welcome(42), &why));
    REQUIRE(producer.sequence() == 43);
    REQUIRE(producer.stats().resyncs == 1);
    REQUIRE(producer.device_id() == hub.device_id);          // the identity did not change
    REQUIRE(producer.credential_id() == hub.credential);

    REQUIRE(producer.next(&m, &why));
    REQUIRE(m.sequence == 43);                                // resumed above the watermark, not replayed

    // A welcome with an OLDER watermark never rewinds the producer: that would replay observations.
    REQUIRE(producer.handle_hub_message(hub_welcome(10), &why));
    REQUIRE(producer.sequence() == 44);
    REQUIRE(producer.stats().resyncs == 1);
}

TEST_CASE("producer: an interruption stops emission without inventing one", "[producer][offline]") {
    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;
    REQUIRE(producer.next(&m, &why));
    const uint64_t before = producer.sequence();

    producer.pause("the link dropped");
    REQUIRE_FALSE(producer.next(&m, &why));
    REQUIRE(why.find("paused") != std::string::npos);
    REQUIRE(why.find("no placeholder") != std::string::npos);
    // Nothing was consumed and nothing was invented: the sequence is where it was.
    REQUIRE(producer.sequence() == before);

    producer.resume();
    REQUIRE(producer.next(&m, &why));
    REQUIRE(m.sequence == before);              // recovery continues FORWARD from the same point
    REQUIRE(producer.sequence() == before + 1);
}

TEST_CASE("producer: the hub's refusals are accounted for, and recovery is possible",
          "[producer][audit]") {
    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;
    REQUIRE(producer.next(&m, &why));

    REQUIRE(producer.handle_hub_message(hub_error(RejectReason::replayed_sequence, "already accepted"), &why));
    REQUIRE(producer.stats().hub_refusals == 1);
    // at(), not operator[]: stats() is const, and a const map has no operator[]
    REQUIRE(producer.stats().refusals_by_code.count("replayed_sequence") == 1);
    REQUIRE(producer.stats().refusals_by_code.at("replayed_sequence") == 1);
    REQUIRE(why.find("replayed_sequence") != std::string::npos);

    // The recovery path is a hello/welcome exchange, and the producer keeps working.
    REQUIRE(producer.handle_hub_message(hub_welcome(7), &why));
    REQUIRE(producer.next(&m, &why));
    REQUIRE(m.sequence == 8);
}

TEST_CASE("producer: a hub message that is not the hub's to send is refused", "[producer][refusal]") {
    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    std::string why;
    DeviceMessage echo;
    echo.kind = MessageKind::observation;
    REQUIRE_FALSE(producer.handle_hub_message(echo, &why));
    REQUIRE(why.find("not expected from the hub") != std::string::npos);
}

// =====================================================================================================
// Task 2 - the reliability matrix
// =====================================================================================================

TEST_CASE("reliability: duplicates, out-of-order and delayed packets", "[reliability][audit]") {
    Hub hub;
    ObservationIngest ingest(hub.registry, ObservationIngest::defaults(), clock_ms);
    std::vector<SecurityObservation> delivered;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;

    REQUIRE(producer.next(&m, &why));
    const auto first = ingest.accept(hub.peer(), m,
                                     [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE(first.accepted);

    // DUPLICATE: the same packet again. It must not be counted twice.
    const auto duplicate = ingest.accept(hub.peer(), m,
                                         [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE_FALSE(duplicate.accepted);
    REQUIRE(delivered.size() == 1);
    REQUIRE((duplicate.reason == RejectReason::replayed_sequence ||
             duplicate.reason == RejectReason::duplicate_observation));

    // OUT OF ORDER: an older sequence arriving late.
    DeviceMessage stale = m;
    stale.sequence = m.sequence - 1;
    stale.observation_id = m.observation_id + "-stale";
    stale.acquired_at_ms = m.acquired_at_ms - 5000;
    REQUIRE_FALSE(ingest.accept(hub.peer(), stale, nullptr).accepted);

    // DELAYED but still within the age allowance: accepted, and the hub keeps the sensor's own timestamp.
    REQUIRE(producer.next(&m, &why));
    DeviceMessage delayed = m;
    delayed.acquired_at_ms = clock_ms() - 60'000;         // one minute late
    const auto late = ingest.accept(hub.peer(), delayed,
                                    [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE(late.accepted);
    REQUIRE(delivered.back().acquired_at.time_since_epoch().count() <
            delivered.back().ingested_at.time_since_epoch().count());
}

TEST_CASE("reliability: an interruption never becomes a fabricated normal reading",
          "[reliability][offline][audit]") {
    // This is the requirement stated as an experiment: a device that stops sending must produce exactly one
    // availability transition and ZERO observations of any sensed quantity, so nothing downstream can read
    // the gap as "the room was quiet".
    Hub hub;
    ObservationIngest ingest(hub.registry, ObservationIngest::defaults(), clock_ms);
    IngestDetectors detectors(IngestDetectors::defaults(), clock_ms);
    std::vector<SecurityObservation> delivered;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;

    REQUIRE(producer.next(&m, &why));
    REQUIRE(ingest.accept(hub.peer(), m, [&](const SecurityObservation& o) { delivered.push_back(o); })
                .accepted);
    const auto fed = detectors.observe(delivered.back());
    REQUIRE(fed.fed);                      // the outcome field is `fed`, not `accepted`
    REQUIRE_FALSE(fed.duplicate);

    // The link drops: the producer pauses (no placeholder is emitted), and time passes.
    producer.pause("network interruption");
    REQUIRE_FALSE(producer.next(&m, &why));
    fake_now += 10 * 60 * 1000;
    const auto transitions = detectors.sweep_availability();

    size_t availability = 0, sensed = 0;
    for (const auto& o : transitions) {
        if (o.source_type == SecuritySourceType::device_availability) {
            ++availability;
            REQUIRE(o.state == "offline");
        } else {
            ++sensed;
        }
    }
    REQUIRE(availability == 1);
    REQUIRE(sensed == 0);          // no fabricated reading, of any type

    // And recovery is a fresh observation plus an online transition - never a retroactive "all clear".
    fake_now += 1000;
    producer.resume();
    REQUIRE(producer.next(&m, &why));
    const auto back = ingest.accept(hub.peer(), m, [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE(back.accepted);
    REQUIRE(delivered.back().device_id == hub.device_id);
}

TEST_CASE("reliability: unauthorized, revoked, consent-withdrawn and unknown-type sources",
          "[reliability][refusal][audit]") {
    Hub hub;
    ObservationIngest ingest(hub.registry, ObservationIngest::defaults(), clock_ms);
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;

    SECTION("an unauthenticated peer") {
        REQUIRE(producer.next(&m, &why));
        const auto refused = ingest.accept(hub.peer(hub.device_id, /*authenticated=*/false), m, nullptr);
        REQUIRE_FALSE(refused.accepted);
        REQUIRE(refused.reason == RejectReason::unauthorized);
    }
    SECTION("a source that is not enrolled") {
        REQUIRE(producer.next(&m, &why));
        auto stranger = hub.peer("hg-stranger00000001");
        REQUIRE_FALSE(ingest.accept(stranger, m, nullptr).accepted);
    }
    SECTION("a revoked source") {
        REQUIRE(hub.registry.revoke(hub.device_id, "owner revoked the node"));
        REQUIRE(producer.next(&m, &why));
        REQUIRE_FALSE(ingest.accept(hub.peer(), m, nullptr).accepted);
    }
    SECTION("consent withdrawn mid-stream") {
        REQUIRE(producer.next(&m, &why));
        REQUIRE(ingest.accept(hub.peer(), m, nullptr).accepted);
        REQUIRE(hub.registry.set_consent(hub.device_id, false, hub.device_id, "security_monitor", clock_ms()));
        REQUIRE(producer.next(&m, &why));
        const auto refused = ingest.accept(hub.peer(), m, nullptr);
        REQUIRE_FALSE(refused.accepted);
        REQUIRE(refused.reason == RejectReason::consent_withdrawn);
    }
    SECTION("an unknown event type") {
        REQUIRE(producer.next(&m, &why));
        m.observation_type = "made-up-quantity";
        const auto refused = ingest.accept(hub.peer(), m, nullptr);
        REQUIRE_FALSE(refused.accepted);
        REQUIRE(refused.reason == RejectReason::unknown_observation_type);
    }
    SECTION("timestamp drift beyond the allowance") {
        REQUIRE(producer.next(&m, &why));
        m.acquired_at_ms = clock_ms() + 10 * 60 * 1000;      // the producer's clock ran away
        const auto refused = ingest.accept(hub.peer(), m, nullptr);
        REQUIRE_FALSE(refused.accepted);
        REQUIRE(refused.detail.find("future") != std::string::npos);
    }
}

TEST_CASE("reliability: one identity is one sequence stream, so a second producer cannot restart it",
          "[reliability][replay][audit]") {
    // Found by this suite's own first version, which had a live and a synthetic producer sharing one
    // device identity. The hub refused the second one - correctly: two producers on one identity restart
    // each other's sequence, which is indistinguishable from a replay. The property is worth pinning,
    // because it is what makes "stable identity across reconnection" mean something.
    Hub hub;
    ObservationIngest ingest(hub.registry, ObservationIngest::defaults(), clock_ms);
    DeviceMessage m;
    std::string why;

    ObservationProducer live(producer_config(hub, "live"), clock_ms);
    ObservationProducer imposter(producer_config(hub, "synthetic"), clock_ms);   // same device_id

    REQUIRE(live.next(&m, &why));
    REQUIRE(ingest.accept(hub.peer(), m, nullptr).accepted);

    REQUIRE(imposter.next(&m, &why));                 // sequence 1 again, from "the same device"
    const auto refused = ingest.accept(hub.peer(), m, nullptr);
    REQUIRE_FALSE(refused.accepted);
    REQUIRE((refused.reason == RejectReason::replayed_sequence ||
             refused.reason == RejectReason::duplicate_observation));
}

TEST_CASE("reliability: mixed synthetic and real provenance is never upgraded",
          "[reliability][provenance][audit]") {
    Hub hub;
    const std::string fixture_id = "hg-fixture000001";
    const std::string fixture_cred = "fixture000001";
    {
        std::string why;
        REQUIRE(hub.registry.pair(device(fixture_id, fixture_cred), &why));
        REQUIRE(hub.registry.set_consent(fixture_id, true, fixture_id, "security_monitor", clock_ms()));
        REQUIRE(hub.registry.set_capture_enabled(fixture_id, true));
    }
    ObservationIngest ingest(hub.registry, ObservationIngest::defaults(), clock_ms);
    std::vector<SecurityObservation> delivered;
    DeviceMessage m;
    std::string why;

    // Two DIFFERENT enrolled devices: a live node and a synthetic fixture. One identity is one sequence
    // stream, so provenance is compared across devices, not across two writers of one identity.
    ObservationProducer live(producer_config(hub, "live"), clock_ms);
    ObservationProducer::Config fixture_cfg = producer_config(hub, "synthetic");
    fixture_cfg.device_id = fixture_id;
    fixture_cfg.credential_id = fixture_cred;
    ObservationProducer synthetic(fixture_cfg, clock_ms);

    REQUIRE(live.next(&m, &why));
    const auto real_result = ingest.accept(hub.peer(), m,
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE(real_result.accepted);
    REQUIRE(real_result.real);
    REQUIRE(delivered.back().provenance == Provenance::real);

    REQUIRE(synthetic.next(&m, &why));
    PeerContext fixture_peer = hub.peer();
    fixture_peer.authenticated_device_id = fixture_id;
    fixture_peer.credential_id = fixture_cred;
    const auto synthetic_result = ingest.accept(fixture_peer, m,
                                                [&](const SecurityObservation& o) { delivered.push_back(o); });
    REQUIRE(synthetic_result.accepted);
    REQUIRE_FALSE(synthetic_result.real);
    REQUIRE(delivered.back().provenance == Provenance::synthetic);

    // Neither provenance is absorbed into the other.
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].provenance != delivered[1].provenance);
    REQUIRE(delivered[0].device_id != delivered[1].device_id);
}

TEST_CASE("contract: the shipped schema matches what the encoder actually emits",
          "[producer][contract]") {
    // The schema is a file, so it can drift from the code that produces messages. This reads it and checks
    // every required field against a real encoded message: a schema change that the encoder does not
    // satisfy fails here rather than in a field deployment.
    const char* root = std::getenv("HG_SOURCE_DIR");
    REQUIRE(root != nullptr);
    std::ifstream in(std::string(root) + "/docs/integration/observation.schema.json");
    REQUIRE(in.good());
    nlohmann::json schema;
    in >> schema;
    REQUIRE(schema["contract_version"] == "1.0");

    Hub hub;
    ObservationProducer producer(producer_config(hub), clock_ms);
    DeviceMessage m;
    std::string why;
    REQUIRE(producer.next(&m, &why));
    const nlohmann::json payload = encode(m);

    for (const auto& field : schema["required"]) {
        INFO("schema requires: " << field.get<std::string>());
        REQUIRE(payload.contains(field.get<std::string>()));
    }
    // And the schema's own version claim matches the protocol constants the encoder stamps.
    REQUIRE(schema["properties"]["protocol_version_major"]["const"] == kProtocolVersionMajor);
    REQUIRE(schema["properties"]["protocol_version_minor"]["const"] == kProtocolVersionMinor);
    REQUIRE(payload["protocol_version_major"] == kProtocolVersionMajor);
}
