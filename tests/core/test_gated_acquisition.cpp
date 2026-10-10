#include "catch2/catch.hpp"
#include "core/ConsentGate.h"
#include "core/SimulatedSensorSource.h"
#include "core/ConsentGatedAcquisition.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/SQLiteDatabase.h"
#include "persistence/SchemaManager.h"
#include "persistence/ConsentRepository.h"
#include <filesystem>
#include <chrono>

using namespace homeguardian;

static const std::string ACQ_DB = "test_gated_acq.db";

struct GatedAcqFixture {
    GatedAcqFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(ACQ_DB);
        std::filesystem::remove(ACQ_DB + "-wal");
        std::filesystem::remove(ACQ_DB + "-shm");
    }
    ~GatedAcqFixture() {
        std::filesystem::remove(ACQ_DB);
        std::filesystem::remove(ACQ_DB + "-wal");
        std::filesystem::remove(ACQ_DB + "-shm");
    }
};

TEST_CASE("Gated acquisition: denied request produces no payload", "[f1][acquisition]") {
    GatedAcqFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(ACQ_DB);
    SchemaManager schema(db);
    schema.initialize();
    ConsentRepository repo(db);

    auto now = std::chrono::system_clock::now();
    // Only a DENIED record exists for this subject+purpose.
    repo.save(ConsentRecord::create_denied("d1", "p1", "presence", "1.0.0", "user"));

    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    SimulatedSensorSource sensor;
    sensor.enqueue({"e1", "sensor.simulated.motion", SimulatedSensorKind::motion, std::nullopt, nlohmann::json::object()});

    ConsentGatedAcquisition acq(gate, sensor);
    AcquisitionResult r = acq.acquire({"p1", "presence", "camera"});

    REQUIRE_FALSE(r.authorized);
    REQUIRE_FALSE(r.acquired);
    REQUIRE_FALSE(r.event.has_value());
    REQUIRE(r.deny_reason == DenyReason::decision_not_granted);
    // The sensor must NOT have been read for a denied request.
    REQUIRE(sensor.emitted_count() == 0);
    REQUIRE(sensor.pending() == 1);

    db->close();
}

TEST_CASE("Gated acquisition: authorized request acquires synthetic payload", "[f1][acquisition]") {
    GatedAcqFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(ACQ_DB);
    SchemaManager schema(db);
    schema.initialize();
    ConsentRepository repo(db);

    repo.save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    SimulatedSensorSource sensor;
    sensor.enqueue({"e1", "sensor.simulated.motion", SimulatedSensorKind::motion, std::nullopt, nlohmann::json::object()});

    ConsentGatedAcquisition acq(gate, sensor);
    AcquisitionResult r = acq.acquire({"p1", "presence", "camera"});

    REQUIRE(r.authorized);
    REQUIRE(r.acquired);
    REQUIRE(r.event.has_value());
    // The acquired event is explicitly synthetic.
    REQUIRE(r.event->get_payload().value("synthetic", false) == true);
    REQUIRE(sensor.emitted_count() == 1);

    db->close();
}

TEST_CASE("Gated acquisition: capture disabled blocks even with a grant", "[f1][acquisition]") {
    GatedAcqFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(ACQ_DB);
    SchemaManager schema(db);
    schema.initialize();
    ConsentRepository repo(db);

    repo.save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    // media_capture_enabled = false -> global deny.
    ConsentGate gate(repo, /*media_capture_enabled=*/false);
    SimulatedSensorSource sensor;
    sensor.enqueue({"e1", "sensor.simulated.motion", SimulatedSensorKind::motion, std::nullopt, nlohmann::json::object()});

    ConsentGatedAcquisition acq(gate, sensor);
    AcquisitionResult r = acq.acquire({"p1", "presence", "camera"});

    REQUIRE_FALSE(r.authorized);
    REQUIRE_FALSE(r.acquired);
    REQUIRE_FALSE(r.event.has_value());
    REQUIRE(r.deny_reason == DenyReason::capture_disabled);
    REQUIRE(sensor.emitted_count() == 0);

    db->close();
}

TEST_CASE("Gated acquisition: unauthorized event never reaches pipeline", "[f1][acquisition][pipeline]") {
    GatedAcqFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(ACQ_DB);
    SchemaManager schema(db);
    schema.initialize();
    ConsentRepository repo(db);

    // No consent at all -> every request is denied.
    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    SimulatedSensorSource sensor;
    sensor.enqueue({"e1", "sensor.simulated.motion", SimulatedSensorKind::motion, std::nullopt, nlohmann::json::object()});

    ConsentGatedAcquisition acq(gate, sensor);
    AcquisitionResult r = acq.acquire({"p1", "presence", "camera"});

    // Because acquisition is denied, there is no event to feed the pipeline.
    REQUIRE_FALSE(r.event.has_value());
    REQUIRE(r.deny_reason == DenyReason::no_consent_record);

    db->close();
}
