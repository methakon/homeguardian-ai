#include "catch2/catch.hpp"
#include "core/PersistenceAudit.h"
#include "core/RuntimeStatus.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// PersistenceAudit + RuntimeStatus: durable metadata audit (SQLite, reusing the
// existing schema/repositories) and bounded observability counters. Simulator-
// only; uses TEMPORARY test databases. No raw media, no PII, no consent-as-
// authorization.
// ---------------------------------------------------------------------------

namespace {

std::string temp_db(const std::string& tag) {
    std::string p = "test_audit_" + tag + ".db";
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
    return p;
}

void cleanup_db(const std::string& p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
}

Alert make_alert(const std::string& id, const std::string& rule = "sim-presence") {
    return Alert::create(id, Severity::info, "synthetic simulator presence notice",
                         {"e1", "e2"}, 0.9, rule, nlohmann::json::object());
}

}  // namespace

// ===========================================================================
// PersistenceAudit
// ===========================================================================

TEST_CASE("Audit: persists alert metadata and counts it",
          "[audit][persist]") {
    Logger::initialize("error");
    std::string db = temp_db("basic");
    {
        PersistenceAudit audit(db);
        REQUIRE(audit.healthy());
        REQUIRE(audit.historical_alert_count() == 0);
        REQUIRE(audit.record_alert(make_alert("a-1")));
        REQUIRE(audit.record_alert(make_alert("a-2")));
        REQUIRE(audit.alerts_written() == 2);
        REQUIRE(audit.total_alert_count() == 2);
    }
    cleanup_db(db);
}

TEST_CASE("Audit: only metadata is persisted (no raw media fields)",
          "[audit][privacy]") {
    Logger::initialize("error");
    std::string db = temp_db("privacy");
    {
        PersistenceAudit audit(db);
        audit.record_alert(make_alert("a-1"));
        // The persisted alert round-trips with its metadata; there is no field
        // for raw audio/video/biometric data.
        auto found = audit.alerts().find_by_id("a-1");
        REQUIRE(found.has_value());
        REQUIRE(found->rule_name == "sim-presence");
        REQUIRE(found->severity == Severity::info);
        REQUIRE(found->contributing_event_ids.size() == 2);
    }
    cleanup_db(db);
}

TEST_CASE("Audit: historical vs current-process alerts are distinguishable",
          "[audit][restart]") {
    Logger::initialize("error");
    std::string db = temp_db("restart");
    // First "process": writes one alert.
    {
        PersistenceAudit audit(db);
        REQUIRE(audit.historical_alert_count() == 0);
        audit.record_alert(make_alert("a-1"));
    }
    // Second "process" (same DB): sees the prior alert as historical.
    {
        PersistenceAudit audit(db);
        REQUIRE(audit.historical_alert_count() == 1);   // from the prior process
        REQUIRE(audit.total_alert_count() == 1);
        audit.record_alert(make_alert("a-2"));           // current-process
        REQUIRE(audit.total_alert_count() == 2);
        REQUIRE(audit.historical_alert_count() == 1);    // historical unchanged
    }
    cleanup_db(db);
}

TEST_CASE("Audit: retention prunes old alerts, preserves newer",
          "[audit][retention]") {
    Logger::initialize("error");
    std::string db = temp_db("retention");
    {
        PersistenceAudit audit(db);
        audit.record_alert(make_alert("a-1"));
        audit.record_alert(make_alert("a-2"));
        REQUIRE(audit.total_alert_count() == 2);

        // Prune everything older than "now + 1h" -> both are older -> removed.
        auto cutoff = std::chrono::system_clock::now() + std::chrono::hours(1);
        size_t removed = audit.prune_older_than(cutoff);
        REQUIRE(removed == 2);
        REQUIRE(audit.total_alert_count() == 0);

        // Prune older than "now - 1h" -> nothing older -> nothing removed.
        audit.record_alert(make_alert("a-3"));
        auto old_cutoff = std::chrono::system_clock::now() - std::chrono::hours(1);
        REQUIRE(audit.prune_older_than(old_cutoff) == 0);
        REQUIRE(audit.total_alert_count() == 1);   // newer record preserved
    }
    cleanup_db(db);
}

TEST_CASE("Audit: write failure degrades status, does not throw or corrupt",
          "[audit][failure]") {
    Logger::initialize("error");
    std::string db = temp_db("failure");
    PersistenceAudit audit(db);
    REQUIRE(audit.record_alert(make_alert("a-1")));
    // Close the underlying DB to force subsequent writes to fail.
    audit.close();
    // A write after close fails safely: returns false, marks degraded, no throw.
    REQUIRE_FALSE(audit.record_alert(make_alert("a-2")));
    REQUIRE_FALSE(audit.healthy());
    REQUIRE(audit.write_failures() >= 1);
    REQUIRE(audit.alerts_written() == 1);   // only the successful one counted
    cleanup_db(db);
}

TEST_CASE("Audit: corrupt/unsupported DB file is handled without silent success",
          "[audit][corrupt]") {
    Logger::initialize("error");
    std::string db = "test_audit_corrupt.db";
    cleanup_db(db);
    // Write a file that is NOT a valid SQLite database.
    {
        std::ofstream f(db);
        f << "this is not a sqlite database at all, just text bytes";
    }
    // Opening/migrating a corrupt DB must throw (caller runs degraded) — it must
    // NOT silently report success or delete the file.
    bool threw = false;
    try {
        PersistenceAudit audit(db);
    } catch (const std::exception&) {
        threw = true;
    }
    REQUIRE(threw);
    // The corrupt file is preserved (not silently deleted).
    REQUIRE(std::filesystem::exists(db));
    cleanup_db(db);
}

TEST_CASE("Audit: schema migration creates the expected tables",
          "[audit][schema]") {
    Logger::initialize("error");
    std::string db = temp_db("schema");
    {
        PersistenceAudit audit(db);
        // events + alerts (v1) and profiles/routines/consent_records (v2) exist.
        REQUIRE_NOTHROW(audit.alerts().find_recent(1));
    }
    cleanup_db(db);
}

// ===========================================================================
// RuntimeStatus
// ===========================================================================

TEST_CASE("Status: counters are bounded integers and reset cleanly",
          "[status]") {
    Logger::initialize("error");
    RuntimeStatus s;
    s.events_processed = 5;
    s.alerts_delivered = 2;
    s.media_authorized = 10;
    s.reset_counters();
    REQUIRE(s.events_processed == 0);
    REQUIRE(s.alerts_delivered == 0);
    REQUIRE(s.media_authorized == 0);
    // Lifecycle facts are not per-cycle counters; they survive reset.
    s.startups = 3;
    s.reset_counters();
    REQUIRE(s.startups == 3);
}

TEST_CASE("Status: summary is privacy-safe and contains expected fields",
          "[status][privacy]") {
    Logger::initialize("error");
    RuntimeStatus s;
    s.app_state = AppState::Running;
    s.capture_enabled = true;
    s.events_processed = 7;
    s.alerts_delivered = 1;
    std::string sum = s.summary();
    // Contains the observable counters.
    REQUIRE(sum.find("state=Running") != std::string::npos);
    REQUIRE(sum.find("capture=on") != std::string::npos);
    REQUIRE(sum.find("events_proc=7") != std::string::npos);
    REQUIRE(sum.find("alerts_deliv=1") != std::string::npos);
    // Contains NO media/payload/secret markers.
    REQUIRE(sum.find("payload") == std::string::npos);
    REQUIRE(sum.find("secret") == std::string::npos);
    REQUIRE(sum.find("password") == std::string::npos);
    REQUIRE(sum.find("audio") == std::string::npos);
}

TEST_CASE("Status: degraded persistence is reflected in summary",
          "[status][privacy]") {
    Logger::initialize("error");
    RuntimeStatus s;
    s.persistence_healthy = false;
    REQUIRE(s.summary().find("persist=DEGRADED") != std::string::npos);
}
