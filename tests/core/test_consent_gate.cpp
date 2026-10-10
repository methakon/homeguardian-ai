#include "catch2/catch.hpp"
#include "core/ConsentGate.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/SQLiteDatabase.h"
#include "persistence/SchemaManager.h"
#include "persistence/ConsentRepository.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

static const std::string GATE_TEST_DB = "test_consent_gate.db";

struct ConsentGateFixture {
    std::shared_ptr<SQLiteDatabase> db;
    std::unique_ptr<ConsentRepository> repo;

    ConsentGateFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(GATE_TEST_DB);
        std::filesystem::remove(GATE_TEST_DB + "-wal");
        std::filesystem::remove(GATE_TEST_DB + "-shm");

        db = std::make_shared<SQLiteDatabase>();
        db->open(GATE_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        repo = std::make_unique<ConsentRepository>(db);
    }

    ~ConsentGateFixture() {
        repo.reset();
        if (db && db->is_open()) {
            db->close();
        }
        db.reset();
        std::filesystem::remove(GATE_TEST_DB);
        std::filesystem::remove(GATE_TEST_DB + "-wal");
        std::filesystem::remove(GATE_TEST_DB + "-shm");
    }
};

TEST_CASE("ConsentGate: capture disabled in config results in capture_disabled even if grant exists", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "fall_detection", {"camera"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, false);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::capture_disabled);
}

TEST_CASE("ConsentGate: granted, correct purpose and category, not expired is authorized", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "fall_detection", {"camera", "audio"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), now + std::chrono::hours(24), "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE(decision.authorized);
    REQUIRE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::none);
}

TEST_CASE("ConsentGate: no consent record for subject and purpose results in no_consent_record", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "telemetry", {"sensor"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::no_consent_record);
}

TEST_CASE("ConsentGate: denied decision results in decision_not_granted", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord denied("c1", "profile_1", "fall_detection", {}, ConsentDecision::denied, "1.0",
                         now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(denied);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::decision_not_granted);
}

TEST_CASE("ConsentGate: withdrawn decision after a grant results in decision_not_granted", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "fall_detection", {"camera"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(2), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentRecord withdrawn("c2", "profile_1", "fall_detection", {}, ConsentDecision::withdrawn, "1.0",
                            now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(withdrawn);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::decision_not_granted);
}

TEST_CASE("ConsentGate: expired grant results in expired", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord expired("c1", "profile_1", "fall_detection", {"camera"}, ConsentDecision::granted, "1.0",
                          now - std::chrono::hours(5), now - std::chrono::hours(1), "user", 1);
    f.repo->save(expired);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::expired);
}

TEST_CASE("ConsentGate: granted for a different purpose results in no_consent_record", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "heart_rate", {"sensor"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::no_consent_record);
}

TEST_CASE("ConsentGate: granted for right purpose but wrong data category results in category_missing", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_1", "fall_detection", {"camera"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_1", "fall_detection", "audio"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::category_missing);
}

TEST_CASE("ConsentGate: unknown profile or subject results in no_consent_record", "[consent_gate]") {
    ConsentGateFixture f;
    auto now = std::chrono::system_clock::now();

    ConsentRecord grant("c1", "profile_known", "fall_detection", {"camera"}, ConsentDecision::granted, "1.0",
                        now - std::chrono::hours(1), std::nullopt, "user", 1);
    f.repo->save(grant);

    ConsentGate gate(*f.repo, true);
    AcquisitionRequest req{"profile_unknown", "fall_detection", "camera"};
    GateDecision decision = gate.check(req, now);

    INFO("Deny reason: " << ConsentGate::reason_to_string(decision.reason));
    REQUIRE_FALSE(decision.authorized);
    REQUIRE_FALSE(static_cast<bool>(decision));
    REQUIRE(decision.reason == DenyReason::no_consent_record);
}
