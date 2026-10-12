#include "catch2/catch.hpp"
#include "core/PrivacyAuditLog.h"
#include "core/ConsentGate.h"
#include "core/ConsentGatedAcquisition.h"
#include "core/SimulatedSensorSource.h"
#include "core/SimulatorRuntime.h"
#include "core/Logger.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Roadmap H-03 — Privacy audit: all data access logged; consent enforcement
// verified. The log is metadata-only, bounded, thread-safe, and passive.
// ---------------------------------------------------------------------------

namespace {
PrivacyAuditEntry mk(const std::string& subject, const std::string& purpose,
                     bool authorized, const std::string& reason, bool acquired) {
    PrivacyAuditEntry e;
    e.timestamp = std::chrono::system_clock::now();
    e.subject_scope = subject;
    e.purpose = purpose;
    e.data_category = "audio";
    e.authorized = authorized;
    e.decision_reason = reason;
    e.acquired = acquired;
    return e;
}
}

TEST_CASE("PrivacyAudit: authorized entry is logged with correct counts",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log;
    log.record(mk("household", "presence", true, "none", true));
    auto c = log.counts();
    REQUIRE(c.total == 1);
    REQUIRE(c.authorized == 1);
    REQUIRE(c.denied == 0);
    REQUIRE(c.acquired == 1);
    REQUIRE(log.size() == 1);
    REQUIRE(log.verify_enforcement());
}

TEST_CASE("PrivacyAudit: denied entry is logged with its reason",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log;
    log.record(mk("household", "presence", false, "capture_disabled", false));
    auto c = log.counts();
    REQUIRE(c.total == 1);
    REQUIRE(c.authorized == 0);
    REQUIRE(c.denied == 1);
    REQUIRE(c.acquired == 0);
    auto recent = log.recent(10);
    REQUIRE(recent.size() == 1);
    REQUIRE(recent[0].decision_reason == "capture_disabled");
    REQUIRE_FALSE(recent[0].authorized);
    REQUIRE(log.verify_enforcement());
}

TEST_CASE("PrivacyAudit: bounded capacity evicts oldest but keeps total counts",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log(/*capacity=*/4);
    for (int i = 0; i < 10; ++i) {
        log.record(mk("s", "p", true, "none", true));
    }
    REQUIRE(log.size() == 4);                 // bounded
    REQUIRE(log.capacity() == 4);
    REQUIRE(log.counts().total == 10);        // counters are cumulative
    REQUIRE(log.counts().authorized == 10);
}

TEST_CASE("PrivacyAudit: string fields are length-clamped",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log;
    PrivacyAuditEntry e = mk("s", "p", true, "none", true);
    e.data_category = std::string(PrivacyAuditLog::kMaxFieldChars + 500, 'x');
    log.record(e);
    auto recent = log.recent(1);
    REQUIRE(recent.size() == 1);
    REQUIRE(recent[0].data_category.size() == PrivacyAuditLog::kMaxFieldChars);
}

TEST_CASE("PrivacyAudit: verify_enforcement detects unauthorized acquisition",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log;
    // A normal (compliant) trail passes.
    log.record(mk("s", "p", true, "none", true));
    log.record(mk("s", "p", false, "no_consent_record", false));
    REQUIRE(log.verify_enforcement());

    // Inject a bogus entry claiming acquisition without authorization; the
    // invariant must catch it (proves it is a real check, not a tautology).
    PrivacyAuditEntry bad = mk("s", "p", /*authorized=*/false, "none",
                               /*acquired=*/true);
    log.record(bad);
    REQUIRE_FALSE(log.verify_enforcement());
}

TEST_CASE("PrivacyAudit: clear resets entries and counters",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log;
    log.record(mk("s", "p", true, "none", true));
    REQUIRE(log.size() == 1);
    log.clear();
    REQUIRE(log.size() == 0);
    REQUIRE(log.counts().total == 0);
    REQUIRE(log.verify_enforcement());
}

TEST_CASE("PrivacyAudit: concurrent recording is thread-safe and bounded",
          "[privacy][audit]") {
    Logger::initialize("error");
    PrivacyAuditLog log(/*capacity=*/64);
    const int threads = 8;
    const int per_thread = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&log, per_thread]() {
            for (int i = 0; i < per_thread; ++i) {
                log.record(mk("household", "presence", true, "none", true));
            }
        });
    }
    for (auto& th : ts) th.join();
    REQUIRE(log.counts().total == static_cast<uint64_t>(threads * per_thread));
    REQUIRE(log.size() == 64);           // bounded under concurrency
    REQUIRE(log.verify_enforcement());
}

// --- Integration: SimulatorRuntime records delivery decisions ---

TEST_CASE("PrivacyAudit: simulator records authorized delivery decisions",
          "[privacy][audit][integration]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    PrivacyAuditLog log;
    rt.set_privacy_audit(&log);
    rt.start();
    rt.tick();
    rt.shutdown();
    auto c = log.counts();
    REQUIRE(c.total >= 1);            // at least one per-consumer decision logged
    REQUIRE(c.authorized >= 1);
    REQUIRE(c.acquired == c.authorized);  // media delivered iff authorized
    REQUIRE(log.verify_enforcement());    // no access without consent
}

TEST_CASE("PrivacyAudit: withdrawn consent is logged as a denial",
          "[privacy][audit][integration]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    PrivacyAuditLog log;
    rt.set_privacy_audit(&log);
    rt.start();
    rt.tick();
    rt.shutdown();
    auto c = log.counts();
    REQUIRE(c.denied >= 1);              // the withdrawn consumer's decision is denied
    REQUIRE(c.authorized >= 1);          // the still-granted consumer is authorized
    REQUIRE(log.verify_enforcement());
}

TEST_CASE("PrivacyAudit: ConsentGatedAcquisition logs a denied attempt",
          "[privacy][audit][integration]") {
    Logger::initialize("error");
    InMemoryConsentRepo repo;            // no records -> default deny
    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    SimulatedSensorSource sensor;
    ConsentGatedAcquisition acq(gate, sensor);
    PrivacyAuditLog log;
    acq.set_privacy_audit(&log);

    AcquisitionRequest req{"household", "presence", "camera"};
    auto result = acq.acquire(req);
    REQUIRE_FALSE(result.authorized);
    REQUIRE_FALSE(result.acquired);
    auto c = log.counts();
    REQUIRE(c.total == 1);
    REQUIRE(c.denied == 1);
    REQUIRE(c.acquired == 0);
    REQUIRE(log.verify_enforcement());
    REQUIRE(log.recent(1)[0].decision_reason != "none");  // a real deny reason
}

TEST_CASE("PrivacyAudit: logging is a passive observer (decision unchanged)",
          "[privacy][audit][integration]") {
    Logger::initialize("error");
    InMemoryConsentRepo repo;
    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    SimulatedSensorSource sensor;
    ConsentGatedAcquisition acq(gate, sensor);

    AcquisitionRequest req{"household", "presence", "camera"};
    auto without = acq.acquire(req);     // no audit attached
    PrivacyAuditLog log;
    acq.set_privacy_audit(&log);
    auto with = acq.acquire(req);        // audit attached
    // The decision is identical whether or not the audit is attached.
    REQUIRE(without.authorized == with.authorized);
    REQUIRE(without.acquired == with.acquired);
    REQUIRE(log.counts().total == 1);    // and the audit saw only the second call
}
