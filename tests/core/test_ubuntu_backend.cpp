#include "catch2/catch.hpp"
#include "backend/linux/SensorDiscovery.h"
#include "core/IMediaDevice.h"
#include "core/FakeMediaDevice.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "persistence/IConsentRepository.h"
#include "core/Logger.h"
#include <atomic>
#include <chrono>

using namespace homeguardian;

// Host-side tests for the Ubuntu (Linux) sensor backend's NON-CAPTURE surface:
// device discovery metadata and consent enforcement. These do NOT open the
// camera or microphone; discovery only enumerates device nodes and sound-server
// metadata, and the consent tests reuse the shared ConsentGuardedDevice gate.

TEST_CASE("Linux discovery: enumeration returns well-formed entries or empty (no throw)",
          "[ubuntu][discovery]") {
    Logger::initialize("error");
    LinuxSensorDiscovery disc;

    // Enumeration must never throw, even on a host with no devices.
    std::vector<SensorDeviceInfo> cams, mics, spks;
    REQUIRE_NOTHROW(cams = disc.list_cameras());
    REQUIRE_NOTHROW(mics = disc.list_microphones());
    REQUIRE_NOTHROW(spks = disc.list_speakers());

    // Every returned entry must carry a non-empty id and a known driver.
    auto check = [](const std::vector<SensorDeviceInfo>& v, SensorKind kind) {
        for (const auto& d : v) {
            REQUIRE(d.kind == kind);
            REQUIRE_FALSE(d.id.empty());
            REQUIRE_FALSE(d.driver.empty());
        }
    };
    check(cams, SensorKind::camera);
    check(mics, SensorKind::microphone);
    check(spks, SensorKind::speaker);
}

TEST_CASE("Ubuntu camera adapter: consent gate blocks start when capture disabled",
          "[ubuntu][consent]") {
    Logger::initialize("error");
    // media_capture_enabled=false (shipped default) denies everything, even
    // with a consent grant — capture is disabled by default.
    // Use the shared ConsentGate via a minimal in-test repository.
    // (No device is opened.)
    struct Repo : IConsentRepository {
        std::vector<ConsentRecord> recs;
        void save(const ConsentRecord& r) override { recs.push_back(r); }
        std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
        std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s) o.push_back(r); return o; }
        std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s && r.get_purpose()==p) o.push_back(r); return o; }
        size_t count() override { return recs.size(); }
        size_t delete_all() override { size_t n=recs.size(); recs.clear(); return n; }
    } repo;
    repo.save(ConsentRecord::create_granted("g1","harness","presence",{"camera"},"1.0.0","operator"));

    ConsentGate gate(repo, /*media_capture_enabled=*/false);
    FakeMediaDevice dev(DeviceKind::camera, "ubuntu-v4l2");
    ConsentGuardedDevice guard(dev, gate, {"harness","presence","camera"});

    REQUIRE_FALSE(guard.initialize().allowed);
    REQUIRE_FALSE(guard.start().allowed);
    REQUIRE(dev.state() == DeviceState::Idle);
    REQUIRE(dev.start_count() == 0);
}

TEST_CASE("Ubuntu camera adapter: withdrawal stops per-frame delivery",
          "[ubuntu][consent]") {
    Logger::initialize("error");
    struct Repo : IConsentRepository {
        std::vector<ConsentRecord> recs;
        void save(const ConsentRecord& r) override { recs.push_back(r); }
        std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
        std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s) o.push_back(r); return o; }
        std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s && r.get_purpose()==p) o.push_back(r); return o; }
        size_t count() override { return recs.size(); }
        size_t delete_all() override { size_t n=recs.size(); recs.clear(); return n; }
    } repo;
    auto now = std::chrono::system_clock::now();
    repo.save(ConsentRecord("g1","harness","presence",{"camera"},ConsentDecision::granted,
                            "1.0.0", now - std::chrono::hours(1), std::nullopt, "operator", 1));

    ConsentGate gate(repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "ubuntu-v4l2");
    ConsentGuardedDevice guard(dev, gate, {"harness","presence","camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);

    int delivered = 0;
    for (int i = 0; i < 5; ++i) { if (!guard.authorize_delivery()) break; dev.deliver_payload(); delivered++; }
    REQUIRE(delivered == 5);

    // Withdraw consent; the next frame must be denied and capture stopped.
    repo.save(ConsentRecord::create_withdrawn("w1","harness","presence","1.0.0","operator"));
    REQUIRE_FALSE(guard.authorize_delivery(now));
    REQUIRE(dev.state() != DeviceState::Capturing);
    int after = 0;
    for (int i = 0; i < 5; ++i) { if (!guard.authorize_delivery()) break; dev.deliver_payload(); after++; }
    REQUIRE(after == 0);
    REQUIRE(dev.delivered().size() == 5);
}

TEST_CASE("Ubuntu audio adapter: permission revocation stops delivery",
          "[ubuntu][consent]") {
    Logger::initialize("error");
    struct Repo : IConsentRepository {
        std::vector<ConsentRecord> recs;
        void save(const ConsentRecord& r) override { recs.push_back(r); }
        std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
        std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s) o.push_back(r); return o; }
        std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
            std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s && r.get_purpose()==p) o.push_back(r); return o; }
        size_t count() override { return recs.size(); }
        size_t delete_all() override { size_t n=recs.size(); recs.clear(); return n; }
    } repo;
    repo.save(ConsentRecord::create_granted("g1","harness","presence",{"audio"},"1.0.0","operator"));

    std::atomic<bool> permission{true};
    ConsentGate gate(repo, true);
    FakeMediaDevice dev(DeviceKind::audio, "ubuntu-alsa");
    ConsentGuardedDevice guard(dev, gate, {"harness","presence","audio"},
                               [&permission]{ return permission.load(); });

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    int delivered = 0;
    for (int i = 0; i < 4; ++i) { if (!guard.authorize_delivery()) break; dev.deliver_payload(); delivered++; }
    REQUIRE(delivered == 4);

    permission.store(false);  // simulate OS/sound-server access loss
    REQUIRE_FALSE(guard.authorize_delivery());
    REQUIRE(dev.state() != DeviceState::Capturing);
}
