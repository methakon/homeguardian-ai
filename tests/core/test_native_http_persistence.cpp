#include "catch2/catch.hpp"
#include <unistd.h>   // getpid: process-unique fixture database paths

#include "bridge/NativeHttp.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include "core/PersistenceAudit.h"

#include <filesystem>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// HG-SEC-01 persistence regression.
//
// The zero-invocation proof in the mock-handler tests shows the handler is not
// called; this file proves the durable consequence - that a rejected request
// leaves the SQLite alert store and the scenario state it would advance
// untouched - against a real temporary database, through the real transport
// entry point. The authorized positive control runs the SAME path so the
// negative result cannot pass vacuously.
// ---------------------------------------------------------------------------

namespace {

std::string temp_db(const std::string& tag) {
    // Unique per process: two suite instances in one working directory must not
    // remove each other's database (SQLite reports that as "disk I/O error").
    std::string p = "test_sec01_" + tag + "_" + std::to_string(::getpid()) + ".db";
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

constexpr uint16_t kPort = 18700;
constexpr const char* kSecret = "operator-secret-xyz";

// The application side of the demonstration: the scenario handler the transport
// invokes. It advances scenario state and writes exactly one durable alert -
// precisely what a rejected request must not be able to cause.
struct DemoApplication {
    std::string db_path;
    PersistenceAudit audit;
    int handler_invocations = 0;
    int scenario_advances = 0;

    explicit DemoApplication(const std::string& path) : db_path(path), audit(path) {}

    std::string handle(const hg_native_http::HandlerRequest&, int& status) {
        ++handler_invocations;
        ++scenario_advances;
        audit.record_alert(Alert::create(
            "alert-" + std::to_string(scenario_advances), Severity::info,
            "synthetic demonstration alert", {"event-1"}, 0.9, "sim-demo",
            nlohmann::json::object()));
        status = 200;
        return "{\"ok\":true}";
    }

    size_t alert_rows() { return audit.total_alert_count(); }
};

struct Fixture {
    DemoApplication app;
    hg_native_http::SessionStore sessions;

    Fixture() : app(temp_db("persist")) { sessions.operator_secret = kSecret; }
    ~Fixture() { cleanup_db(app.db_path); }

    std::string dispatch(const std::string& request) {
        return hg_native_http::dispatch(
            request, "<html>demo</html>",
            [this](const hg_native_http::HandlerRequest& r, int& s) { return app.handle(r, s); },
            sessions, 1700000000000LL, kPort);
    }

    static bool has(const std::string& response, const std::string& needle) {
        return response.find(needle) != std::string::npos;
    }
    static bool accepted(const std::string& response) {
        return response.rfind("HTTP/1.1 2", 0) == 0;
    }

    std::string request(const std::string& method, const std::string& target,
                        const std::string& extra = {}, const std::string& body = {},
                        bool with_length = true) {
        std::string r = method + " " + target + " HTTP/1.1\r\n"
                      + "Host: 127.0.0.1:" + std::to_string(kPort) + "\r\n"
                      + "Origin: http://127.0.0.1:" + std::to_string(kPort) + "\r\n"
                      + extra;
        if (with_length && !body.empty())
            r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        r += "\r\n" + body;
        return r;
    }

    std::string mint() {
        const std::string out = dispatch(request(
            "POST", "/api/session",
            "Content-Type: application/json\r\nX-HG-Secret: " + std::string(kSecret) + "\r\n",
            "{}"));
        const std::string key = "\"csrf\":\"";
        auto at = out.find(key);
        if (at == std::string::npos) return {};
        at += key.size();
        return out.substr(at, out.find('"', at) - at);
    }
};

}  // namespace

TEST_CASE("HG-SEC-01 persistence: rejected requests write no alert rows and advance no state",
          "[native_http_persist][security]") {
    Logger::initialize("error");
    Fixture f;
    REQUIRE(f.app.audit.healthy());
    REQUIRE(f.app.alert_rows() == 0);

    const std::string json_type = "Content-Type: application/json\r\n";
    const std::string csrf = f.mint();
    REQUIRE(csrf.size() == 64);   // a session was genuinely authorized first

    // Each rejection is measured against the durable store immediately after it.
    auto rejects = [&](const char* label, const std::string& req, const char* expect) {
        const size_t rows_before = f.app.alert_rows();
        const int invocations_before = f.app.handler_invocations;
        const int advances_before = f.app.scenario_advances;

        const std::string out = f.dispatch(req);

        INFO(label);
        INFO("response: " << out.substr(0, 60));
        REQUIRE(Fixture::has(out, expect));
        REQUIRE(f.app.handler_invocations == invocations_before);  // handler never ran
        REQUIRE(f.app.scenario_advances == advances_before);       // state never advanced
        REQUIRE(f.app.alert_rows() == rows_before);                // no alert row written
    };

    // The state route is POST-only: a GET is refused by the method rule.
    rejects("GET cannot advance a scenario",
            f.request("GET", "/api/run-scenario?scenario=unusual"), "405 Method Not Allowed");

    // No credential at all, and a credential that is not the operator's.
    rejects("state route called with no CSRF token present",
            f.request("POST", "/api/run-scenario?scenario=unusual", json_type, "{}"),
            "403 Forbidden");
    // The state route authenticates on the session-bound CSRF token, so an unusable
    // session credential is refused there by the CSRF rule (403), not by a secret comparison.
    rejects("unusable session credential on the state route",
            f.request("POST", "/api/run-scenario?scenario=unusual",
                      json_type + "X-HG-Secret: not-the-secret\r\n", "{}"),
            "403 Forbidden");

    // An authorized session whose CSRF token is missing, wrong.
    rejects("no CSRF token",
            f.request("POST", "/api/run-scenario?scenario=unusual",
                      json_type + "X-HG-Secret: " + kSecret + "\r\n", "{}"),
            "403 Forbidden");
    rejects("wrong CSRF token",
            f.request("POST", "/api/run-scenario?scenario=unusual",
                      json_type + "X-HG-Secret: " + kSecret + "\r\n"
                               + "X-HG-CSRF: " + std::string(64, 'a') + "\r\n", "{}"),
            "403 Forbidden");
    {
        // Wrong scheme: the same authority over https is not this origin.
        std::string r = f.request("POST", "/api/run-scenario?scenario=unusual",
                                  json_type + "X-HG-CSRF: " + csrf + "\r\n", "{}");
        const std::string from = "Origin: http://127.0.0.1:" + std::to_string(kPort);
        REQUIRE(r.find(from) != std::string::npos);
        r.replace(r.find(from), from.size(),
                  "Origin: https://127.0.0.1:" + std::to_string(kPort));
        rejects("wrong-scheme Origin", r, "403 Forbidden");
    }

    // Malformed framing: a body with no declared length.
    rejects("body without Content-Length",
            f.request("POST", "/api/run-scenario?scenario=unusual",
                      json_type + "X-HG-CSRF: " + f.mint() + "\r\n", "{}", /*with_length=*/false),
            "411 Length Required");

    {
        const std::string single_use = f.mint();
        REQUIRE(single_use.size() == 64);

        const size_t before = f.app.alert_rows();
        const std::string first = f.dispatch(f.request(
            "POST", "/api/run-scenario?scenario=unusual",
            json_type + "X-HG-CSRF: " + single_use + "\r\n", "{}"));
        INFO("first authorized use: " << first.substr(0, 30));
        REQUIRE(Fixture::accepted(first));
        REQUIRE(f.app.alert_rows() == before + 1);  // the authorized use really did write

        rejects("replayed single-use CSRF token (second use)",
                f.request("POST", "/api/run-scenario?scenario=unusual",
                          json_type + "X-HG-CSRF: " + single_use + "\r\n", "{}"),
                "403 Forbidden");
    }

    // The SESSION route's own gate: a wrong operator secret mints nothing, so no
    // authorization exists to abuse, and the store stays untouched.
    {
        const int invocations_before = f.app.handler_invocations;
        const size_t rows_before = f.app.alert_rows();
        const std::string out = f.dispatch(f.request(
            "POST", "/api/session",
            "Content-Type: application/json\r\nX-HG-Secret: not-the-secret\r\n", "{}"));
        INFO("wrong secret on the session route");
        REQUIRE(Fixture::has(out, "HTTP/1.1 401"));
        // Relative to what earlier phases already did: the point is that THIS refusal
        // caused no invocation and no new row.
        REQUIRE(f.app.handler_invocations == invocations_before);
        REQUIRE(f.app.alert_rows() == rows_before);
    }

    // Durability: the file on disk still holds nothing, read back through a fresh
    // handle rather than trusted from the in-memory counter.
    {
        PersistenceAudit reopened(f.app.db_path);
        REQUIRE(reopened.healthy());
        REQUIRE(reopened.total_alert_count() == f.app.alert_rows());
    }

    // POSITIVE CONTROL: the same path, authorized, must advance state and write one row.
    const size_t before = f.app.alert_rows();
    const int advances_before = f.app.scenario_advances;
    const std::string out = f.dispatch(f.request(
        "POST", "/api/run-scenario?scenario=unusual",
        json_type + "X-HG-CSRF: " + f.mint() + "\r\n", "{}"));

    INFO("positive control response: " << out.substr(0, 60));
    INFO("alerts " << before << " -> " << f.app.alert_rows());
    REQUIRE(Fixture::accepted(out));
    REQUIRE(f.app.handler_invocations == 1 + advances_before);
    REQUIRE(f.app.scenario_advances == advances_before + 1);
    REQUIRE(f.app.alert_rows() == before + 1);
}
