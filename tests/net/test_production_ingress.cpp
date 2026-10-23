// Real mTLS regression: no direct detector observe() calls in this fixture.
#include <catch2/catch.hpp>
#include "net/DeviceIngressServer.h"
#include "core/Pipeline.h"
#include "core/Logger.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace homeguardian;
using namespace homeguardian::net;
namespace {
const int64_t kNow = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count() - 120000;
struct Fixture {
    std::string dir;
    std::atomic<int64_t> clock{kNow};
    DeviceRegistry registry;
    ObservationIngest ingest{registry, ObservationIngest::defaults(), [this] { return clock.load(); }};
    IngestDetectors detectors{IngestDetectors::defaults(), [this] { return clock.load(); }};
    std::mutex mutex;
    std::vector<SecurityObservation> accepted;
    std::vector<Observation> derived;
    Pipeline pipeline{std::vector<std::shared_ptr<ICorrelationRule>>{}};
    std::unique_ptr<DeviceIngressServer> server;
    std::thread runner;
    TlsClient client;
    bool fail_delivery = false;

    Fixture() {
        Logger::initialize("off");
        const char* path = std::getenv("HG_INGRESS_TEST_DIR");
        REQUIRE(path != nullptr);
        dir = path;
        std::ifstream fingerprint(dir + "/fingerprint");
        std::string credential;
        fingerprint >> credential;
        REQUIRE_FALSE(credential.empty());
        DeviceRegistry::Device device;
        device.device_id = "hg-tls-fixture";
        device.device_type = "hub-fixture";
        device.credential_id = credential;
        device.paired = true;
        device.is_synthetic = true;
        std::string error;
        REQUIRE(registry.pair(device, &error));
        REQUIRE(registry.set_consent(device.device_id, true, "fixture-subject", "security_monitor", kNow));
        REQUIRE(registry.set_capture_enabled(device.device_id, true));
        REQUIRE(registry.save(dir + "/registry.json", &error));
        DeviceIngressServer::Config config;
        config.tls.bind_address = "127.0.0.1";
        config.tls.port = 0;
        config.tls.certificate_path = dir + "/server.pem";
        config.tls.private_key_path = dir + "/server.key";
        config.tls.ca_path = dir + "/ca.pem";
        config.tls.idle_timeout_ms = 1000;
        config.authorization_refresh_ms = 1;
        config.replay_state_path = dir + "/replay.json";
        std::remove(config.replay_state_path.c_str());
        server = std::make_unique<DeviceIngressServer>(config, registry, ingest, detectors,
            DeviceIngressServer::Sinks{
                [this](const SecurityObservation& o) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (fail_delivery) throw std::runtime_error("fixture delivery failure");
                    pipeline.process(o.to_observation().to_event());
                    accepted.push_back(o);
                },
                [this](const Observation& o) {
                    std::lock_guard<std::mutex> lock(mutex);
                    pipeline.process(o.to_event());
                    derived.push_back(o);
                }}, "", "", dir + "/registry.json");
        REQUIRE(server->start(&error));
        runner = std::thread([this] { server->run(); });
        try {
        REQUIRE(client.connect("127.0.0.1", server->port(), dir + "/ca.pem",
                               dir + "/client.pem", dir + "/client.key", 2000, &error));
        DeviceMessage hello;
        hello.protocol_version_major = kProtocolVersionMajor;
        hello.protocol_version_minor = kProtocolVersionMinor;
        hello.kind = MessageKind::hello;
        hello.device_id_claim = device.device_id;
        REQUIRE(client.send(hello, &error));
        DeviceMessage reply;
        REQUIRE(client.receive(&reply, 2000, &error));
        INFO(reply.detail);
        REQUIRE(reply.kind == MessageKind::welcome);
        } catch (...) {
            client.close();
            server->request_stop();
            runner.join();
            server.reset();
            throw;
        }
    }
    ~Fixture() {
        client.close();
        if (server) server->request_stop();
        if (runner.joinable()) runner.join();
        server.reset(); // join workers before the sinks and core state are destroyed
    }
    DeviceMessage sample(uint64_t sequence) {
        DeviceMessage m;
        m.protocol_version_major = kProtocolVersionMajor;
        m.protocol_version_minor = kProtocolVersionMinor;
        m.kind = MessageKind::observation;
        m.device_id_claim = "hg-tls-fixture";
        m.observation_id = "tls-sample-" + std::to_string(sequence);
        m.sequence = sequence;
        m.acquired_at_ms = clock.load();
        m.source_type = "motion";
        m.observation_type = "motion-changed-fraction";
        m.acquisition_mode = "synthetic";
        m.provenance_claim = "synthetic";
        m.confidence = 0.9;
        m.features = {{"state", "on"}, {"motion-changed-fraction", 0.2}};
        return m;
    }
    DeviceMessage send(const DeviceMessage& m) {
        std::string error;
        REQUIRE(client.send(m, &error));
        DeviceMessage reply;
        REQUIRE(client.receive(&reply, 2000, &error));
        REQUIRE(reply.kind == MessageKind::acknowledgement);
        REQUIRE(reply.observation_id == m.observation_id);
        if (reply.reason != RejectReason::none) std::cout << "ACK: " << reply.detail << '\n';
        return reply;
    }
    size_t derived_size() {
        std::lock_guard<std::mutex> lock(mutex);
        return derived.size();
    }
    void probe_new_connection(uint64_t sequence) {
        std::string error;
        TlsClient fresh;
        REQUIRE(fresh.connect("127.0.0.1", server->port(), dir + "/ca.pem",
                              dir + "/client.pem", dir + "/client.key", 2000, &error));
        DeviceMessage hello;
        hello.protocol_version_major = kProtocolVersionMajor;
        hello.protocol_version_minor = kProtocolVersionMinor;
        hello.kind = MessageKind::hello;
        hello.device_id_claim = "hg-tls-fixture";
        REQUIRE(fresh.send(hello, &error));
        DeviceMessage reply;
        INFO("a queued connection must not be starved by the maintenance wait");
        REQUIRE(fresh.receive(&reply, 2000, &error));
        REQUIRE(reply.kind == MessageKind::welcome);
        const DeviceMessage m = sample(sequence);
        REQUIRE(fresh.send(m, &error));
        REQUIRE(fresh.receive(&reply, 2000, &error));
        REQUIRE(reply.kind == MessageKind::acknowledgement);
        REQUIRE(reply.reason == RejectReason::none);
        fresh.close();
    }
};
}

TEST_CASE("Production ingress feeds only completed authorized deliveries to detectors", "[production-ingress][.]") {
    Fixture f;
    auto m = f.sample(1);
    REQUIRE(f.send(m).reason == RejectReason::none);
    REQUIRE(f.derived_size() == 1);
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        REQUIRE(f.derived.front().detector_id == "motion-activity");
        REQUIRE(f.derived.front().source == "hg-tls-fixture");
        REQUIRE(f.derived.front().provenance == Provenance::synthetic);
    }
    // Completed retransmission is ACKed but must not feed detectors again.
    REQUIRE(f.send(m).reason == RejectReason::none);
    REQUIRE(f.derived_size() == 1);
    m.observation_id = "different-id-same-sequence";
    REQUIRE(f.send(m).reason == RejectReason::replayed_sequence);
    REQUIRE(f.derived_size() == 1);
    m = f.sample(2);
    m.device_id_claim = "impostor";
    REQUIRE(f.send(m).reason == RejectReason::identity_mismatch);
    REQUIRE(f.derived_size() == 1);
    // Sink failure must not train the detector or refresh liveness.
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        f.fail_delivery = true;
    }
    m = f.sample(2);
    REQUIRE(f.send(m).reason == RejectReason::processing_failed);
    REQUIRE(f.derived_size() == 1);
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        f.fail_delivery = false;
    }
    REQUIRE(f.send(m).reason == RejectReason::none);
    REQUIRE(f.derived_size() == 2);
}

TEST_CASE("Production ingress evaluates quiet devices without another sample", "[production-ingress][.]") {
    Fixture f;
    REQUIRE(f.send(f.sample(1)).reason == RejectReason::none);
    f.client.close();
    f.clock.store(kNow + 91000);
    // The detector clock advances, but the real production scheduler must run it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    size_t count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(f.mutex);
            count = f.accepted.size();
        }
        if (count == 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(count == 2);
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        const auto& offline = f.accepted.back();
        REQUIRE(offline.source_type == SecuritySourceType::device_availability);
        REQUIRE(offline.state == "offline");
        REQUIRE(offline.provenance == Provenance::synthetic);
        REQUIRE(offline.consent_subject == "fixture-subject");
        REQUIRE(offline.consent_purpose == "security_monitor");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::lock_guard<std::mutex> lock(f.mutex);
    REQUIRE(f.accepted.size() == 2); // one transition, never a repeated event stream
}

TEST_CASE("Production ingress reauthorizes quiet-device delivery", "[production-ingress][.]") {
    Fixture f;
    REQUIRE(f.send(f.sample(1)).reason == RejectReason::none);
    nlohmann::json authorization;
    { std::ifstream input(f.dir + "/registry.json"); input >> authorization; }
    // Edit the exact operator file, never mutate the live registry across threads.
    auto& device = authorization["devices"][0];
    RejectReason expected = RejectReason::unauthorized;
    bool remove_file = false;
    bool malformed = false;
    SECTION("credential rotated") {
        device["credential_id"] = "replacement-credential";
        expected = RejectReason::identity_mismatch;
    }
    SECTION("consent withdrawn") {
        device["consent_granted"] = false;
        device["capture_enabled"] = false;
        expected = RejectReason::consent_withdrawn;
    }
    SECTION("capture disabled") {
        device["capture_enabled"] = false;
        expected = RejectReason::capture_disabled;
    }
    SECTION("revoked") {
        device["revoked"] = true;
        expected = RejectReason::device_revoked;
    }
    SECTION("unpaired") {
        device["paired"] = false;
        device["capture_enabled"] = false;
        expected = RejectReason::device_unpaired;
    }
    SECTION("registry missing") { remove_file = true; }
    SECTION("registry malformed") { malformed = true; }
    if (remove_file) {
        REQUIRE(std::remove((f.dir + "/registry.json").c_str()) == 0);
    } else {
        { std::ofstream output(f.dir + "/replacement.json");
          output << (malformed ? "not JSON" : authorization.dump()); }
        REQUIRE(std::rename((f.dir + "/replacement.json").c_str(),
                            (f.dir + "/registry.json").c_str()) == 0);
    }
    REQUIRE(f.send(f.sample(2)).reason == expected);
    REQUIRE(f.derived_size() == 1);
    f.client.close();
    f.clock.store(kNow + 91000);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::lock_guard<std::mutex> lock(f.mutex);
    REQUIRE(f.accepted.size() == 1); // no unauthorized idle availability delivery
}

TEST_CASE("Queued real TLS connections are never starved by the maintenance wait",
          "[production-ingress][.]") {
    Fixture f;
    // Connection 1 is already established by the fixture; each probe below arrives
    // while the maintenance loop is asleep in its wait.
    for (uint64_t sequence = 2; sequence <= 10; ++sequence) {
        f.probe_new_connection(sequence);
    }
    std::lock_guard<std::mutex> lock(f.mutex);
    REQUIRE(f.accepted.size() == 9);
}

// Shutdown must not depend on a connection ever arriving: the maintenance worker
// sleeps in its own wait and has to be woken, joined and its durable state flushed
// only after every worker is gone.
TEST_CASE("Maintenance worker stops cleanly with no inbound frame", "[production-ingress][.]") {
    Logger::initialize("off");
    const char* path = std::getenv("HG_INGRESS_TEST_DIR");
    REQUIRE(path != nullptr);
    const std::string dir = path;
    std::ifstream fingerprint(dir + "/fingerprint");
    std::string credential;
    fingerprint >> credential;
    REQUIRE_FALSE(credential.empty());

    DeviceRegistry registry;
    DeviceRegistry::Device device;
    device.device_id = "hg-tls-fixture";
    device.device_type = "hub-fixture";
    device.credential_id = credential;
    device.paired = true;
    device.is_synthetic = true;
    std::string error;
    REQUIRE(registry.pair(device, &error));
    REQUIRE(registry.set_consent(device.device_id, true, "fixture-subject", "security_monitor", kNow));
    REQUIRE(registry.set_capture_enabled(device.device_id, true));
    REQUIRE(registry.save(dir + "/registry-idle.json", &error));

    ObservationIngest ingest{registry, ObservationIngest::defaults(), [&] { return kNow; }};
    IngestDetectors detectors{IngestDetectors::defaults(), [&] { return kNow; }};
    DeviceIngressServer::Config config;
    config.tls.bind_address = "127.0.0.1";
    config.tls.port = 0;
    config.tls.certificate_path = dir + "/server.pem";
    config.tls.private_key_path = dir + "/server.key";
    config.tls.ca_path = dir + "/ca.pem";
    config.authorization_refresh_ms = 1;
    config.replay_state_path = dir + "/replay-idle.json";
    std::remove(config.replay_state_path.c_str());

    std::atomic<int> deliveries{0};
    auto server = std::make_unique<DeviceIngressServer>(config, registry, ingest, detectors,
        DeviceIngressServer::Sinks{
            [&](const SecurityObservation&) { deliveries.fetch_add(1); },
            [&](const Observation&) { deliveries.fetch_add(1); }},
        "", "", dir + "/registry-idle.json");
    REQUIRE(server->start(&error));

    std::atomic<bool> finished{false};
    std::thread runner([&] { server->run(); finished.store(true); });
    // Let at least one maintenance tick run while no connection exists.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto started = std::chrono::steady_clock::now();
    server->request_stop();
    while (!finished.load() && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    INFO("shutdown took " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
    REQUIRE(finished.load());
    REQUIRE(elapsed < std::chrono::seconds(2));
    if (runner.joinable()) runner.join();
    // A second stop request from the destructor path must stay harmless.
    server->request_stop();
    server.reset();
    REQUIRE(deliveries.load() == 0); // nothing was authorized into an idle sweep
}

// Stopping while the maintenance worker is mid-sweep must not deliver a transition
// after the stop, and must not hang behind the core lock.
TEST_CASE("Shutdown racing authorization revocation delivers nothing after stop",
          "[production-ingress][.]") {
    for (int attempt = 0; attempt < 3; ++attempt) {
        Fixture f;
        REQUIRE(f.send(f.sample(1)).reason == RejectReason::none);
        nlohmann::json authorization;
        { std::ifstream input(f.dir + "/registry.json"); input >> authorization; }
        authorization["devices"][0]["credential_id"] = "replacement-credential";
        { std::ofstream output(f.dir + "/replacement.json"); output << authorization.dump(); }
        REQUIRE(std::rename((f.dir + "/replacement.json").c_str(),
                            (f.dir + "/registry.json").c_str()) == 0);
        f.client.close();
        f.clock.store(kNow + 91000);   // offline transition becomes due
        f.server->request_stop();      // ...at the same moment as the stop
        const auto started = std::chrono::steady_clock::now();
        if (f.runner.joinable()) f.runner.join();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        INFO("join took " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
        REQUIRE(elapsed < std::chrono::seconds(2));
        size_t at_stop = 0;
        { std::lock_guard<std::mutex> lock(f.mutex); at_stop = f.accepted.size(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::lock_guard<std::mutex> lock(f.mutex);
        REQUIRE(f.accepted.size() == at_stop);
    }
}
