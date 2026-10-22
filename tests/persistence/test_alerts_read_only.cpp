#include "catch2/catch.hpp"

// ---------------------------------------------------------------------------
// READ-ONLY STORE ACCESS (HG-CPP-MCP-READONLY-25, Option A).
//
// The seam's decision: exactly ONE process writes the application's alert store (the D-03 ingress, which
// runs Observation -> Event -> Pipeline -> AlertPolicy and calls record_alert), and the MCP process reads
// that same store. These cases pin down what "read-only" has to mean for that to be true, on the real
// classes rather than on a convention:
//
//   * a reader never CREATES the store - a missing one is an error, not an empty database;
//   * a reader cannot WRITE even when it tries - the guarantee is SQLite's (SQLITE_OPEN_READONLY plus
//     PRAGMA query_only), not something a future caller has to remember;
//   * a reader that opens something that is not a HomeGuardian store fails closed;
//   * and it really does read, including state committed by another connection.
//
// The distinction these tests exist to protect: "no alerts" and "no store" are different statements about
// the state of the home, and a reader must never be able to answer the second one with the first.
// ---------------------------------------------------------------------------

#include <unistd.h>  // getpid: process-unique fixture paths

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/Alert.h"
#include "core/Logger.h"
#include "core/PersistenceAudit.h"
#include "persistence/AlertRepository.h"
#include "persistence/SQLiteDatabase.h"

using namespace homeguardian;
using json = nlohmann::json;

namespace {

// A store path unique to this process, so two suite instances in one working directory cannot collide.
std::string store_path(const char* suffix) {
    return std::string("test_readonly.db.") + std::to_string(::getpid()) + "." + suffix;
}

void remove_store(const std::string& path) {
    for (const char* side : {"", "-wal", "-shm"}) {
        std::error_code ec;
        std::filesystem::remove(path + side, ec);
    }
}

Alert synthetic_alert(const std::string& id) {
    return Alert::create(id, Severity::warning, "synthetic alert for a read-only test", {}, 0.8, "fusion",
                         json::object());
}

}  // namespace

TEST_CASE("read-only store: a missing store is an error and is NOT created",
          "[persistence][readonly][failclosed]") {
    Logger::initialize("error");
    const std::string path = store_path("missing");
    remove_store(path);

    bool threw = false;
    std::string what;
    try {
        PersistenceAudit reader(path, PersistenceAudit::Access::read_only);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    REQUIRE(threw);
    REQUIRE(what.find("read-only") != std::string::npos);

    // The assertion that matters: the reader did not conjure the store into existence and then present it
    // as an empty one. Before this change, opening a missing path CREATED it and the application reported
    // "no alerts" for "no storage".
    REQUIRE_FALSE(std::filesystem::exists(path));
    remove_store(path);
}

TEST_CASE("read-only store: it reads another connection's alerts and cannot write",
          "[persistence][readonly][single-writer]") {
    Logger::initialize("error");
    const std::string path = store_path("shared");
    remove_store(path);

    {
        // The single writer: creates, migrates, commits one alert, then closes.
        PersistenceAudit writer(path);
        REQUIRE(writer.record_alert(synthetic_alert("ro-alert-1")));
        REQUIRE(writer.total_alert_count() == 1);
    }

    PersistenceAudit reader(path, PersistenceAudit::Access::read_only);
    REQUIRE(reader.read_only());
    REQUIRE(reader.historical_alert_count() == 1);
    REQUIRE(reader.total_alert_count() == 1);

    const auto recent = reader.alerts().find_recent(10);
    REQUIRE(recent.size() == 1);
    REQUIRE(recent.front().alert_id == "ro-alert-1");
    REQUIRE(reader.healthy());

    // A reader that tries to write must FAIL, and the refusal must be visible rather than swallowed.
    const bool wrote = reader.record_alert(synthetic_alert("ro-alert-2"));
    REQUIRE_FALSE(wrote);
    REQUIRE(reader.total_alert_count() == 1);  // nothing was added
    REQUIRE(reader.write_failures() >= 1);
    REQUIRE_FALSE(reader.healthy());

    // A delete is a write too: retention must not be able to prune through a reader's connection.
    const size_t pruned =
        reader.prune_older_than(std::chrono::system_clock::now() + std::chrono::hours(1));
    REQUIRE(pruned == 0);
    REQUIRE(reader.total_alert_count() == 1);

    remove_store(path);
}

TEST_CASE("read-only store: a file that is not a HomeGuardian store fails closed",
          "[persistence][readonly][failclosed]") {
    Logger::initialize("error");
    const std::string path = store_path("foreign");
    remove_store(path);
    {
        std::ofstream f(path);
        f << "this file is text, not a sqlite database and not an alerts store";
    }

    bool threw = false;
    std::string what;
    try {
        PersistenceAudit reader(path, PersistenceAudit::Access::read_only);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    REQUIRE(threw);
    // It fails as "not this kind of store", not as a crash, and the file is left alone for the operator.
    REQUIRE(what.find("HomeGuardian") != std::string::npos);
    REQUIRE(std::filesystem::exists(path));
    { std::ifstream f(path); std::string line; std::getline(f, line); REQUIRE(line.find("text") != std::string::npos); }
    remove_store(path);
}

TEST_CASE("read-only store: the WAL behaviour is observed, not assumed",
          "[persistence][readonly][wal]") {
    Logger::initialize("error");
    const std::string path = store_path("wal");
    remove_store(path);

    {
        PersistenceAudit writer(path);  // opens WAL, as the application does
        REQUIRE(writer.record_alert(synthetic_alert("ro-wal-1")));
    }

    PersistenceAudit reader(path, PersistenceAudit::Access::read_only);
    // The reader must be able to see WAL-committed state. Whether SQLITE_OPEN_READONLY sufficed, or the
    // connection had to be opened read-write for a -shm reason and pinned with PRAGMA query_only, is
    // RECORDED here rather than hidden - and either way it cannot write (asserted above).
    INFO("opened_with_readonly_flag=" << reader.opened_with_readonly_flag());
    REQUIRE(reader.total_alert_count() == 1);
    REQUIRE(reader.read_only());
    remove_store(path);
}
