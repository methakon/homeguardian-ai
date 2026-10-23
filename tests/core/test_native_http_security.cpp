// HG-SEC-01 security tests for the native demo HTTP transport.
//
// The vulnerability being pinned: the state-advancing scenario route was an UNAUTHENTICATED
// GET, reachable by any local process (or any page the browser would load) with no credential
// and no method restriction. These tests assert the corrected contract:
//
//   * GET can never advance a scenario (405, handler not reached);
//   * a session token only exists after the operator secret is proven (401 otherwise);
//   * the CSRF token is single-use, expiring and bound to that authorization (replay fails);
//   * exact same-origin is enforced (scheme + loopback host + THIS listener's port);
//   * malformed framing, bodies and headers fail safely;
//   * issuance is rate-limited and repeated bad secrets lock out;
//   * EVERY rejected request invokes the handler zero times - which is the only path that can
//     write an alert, so zero invocations is zero writes. The final case proves the counter is
//     not vacuous by driving one authorized run through the same counting handler.
//
// The handler here is the ONLY writer path, exactly as in the real bridge: the scenario
// pipeline writes alerts, and the transport never writes anything itself.

#include "catch2/catch.hpp"

#include <string>

#include "bridge/NativeHttp.h"

namespace {

using hg_native_http::SessionStore;

struct Harness {
    SessionStore sessions;
    int handler_calls = 0;
    int64_t now_ms = 1'700'000'000'000;
    uint16_t port = 18700;
    std::string secret = "operator-secret-xyz";

    Harness() { sessions.operator_secret = secret; }

    std::string handle(const std::string& request) {
        auto handler = [this](const hg_native_http::HandlerRequest&, int& status) {
            ++handler_calls;               // the only place an alert could ever be written
            status = 200;
            return std::string("{\"ok\":true}");
        };
        return hg_native_http::dispatch(request, "<html>demo</html>", handler, sessions, now_ms,
                                        port);
    }

    // A well-formed request. `extra` carries the headers a given test needs.
    std::string request(const std::string& method, const std::string& path,
                        const std::string& extra = std::string(),
                        const std::string& body = std::string(), bool with_length = true,
                        const std::string& length_override = std::string()) {
        std::string r = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                        std::to_string(port) + "\r\n" + extra;
        // A GET carries no Content-Length unless the test is deliberately sending a body.
        const bool method_is_post = (method == "POST");
        if (with_length && (method_is_post || !body.empty())) {
            r += "Content-Length: " +
                 (length_override.empty() ? std::to_string(body.size()) : length_override) + "\r\n";
        }
        r += "\r\n" + body;
        return r;
    }

    // A POST that passes every check EXCEPT the CSRF token (used to prove the gate).
    std::string post_state_no_csrf(const std::string& path = "/api/run-scenario?scenario=unusual") {
        return request("POST", path,
                       "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n"
                       "Content-Type: application/json\r\n",
                       "{}");
    }

    std::string post_session(const std::string& presented) {
        return request("POST", "/api/session",
                       "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n"
                       "Content-Type: application/json\r\n"
                       "X-HG-Secret: " + presented + "\r\n",
                       "{}");
    }

    // Pull the csrf value out of a session response without a JSON dependency.
    static std::string extract_csrf(const std::string& response) {
        const std::string key = "\"csrf\":\"";
        const auto at = response.find(key);
        if (at == std::string::npos) return {};
        const auto start = at + key.size();
        const auto end = response.find('"', start);
        if (end == std::string::npos) return {};
        return response.substr(start, end - start);
    }

    std::string mint() { return extract_csrf(handle(post_session(secret))); }

    static bool has(const std::string& response, const std::string& needle) {
        return response.find(needle) != std::string::npos;
    }
};

}  // namespace

TEST_CASE("native http security: GET can never advance a scenario", "[native_http_security]") {
    Harness h;
    const auto r = h.handle(h.request("GET", "/api/run-scenario?scenario=unusual"));
    REQUIRE(Harness::has(r, "405 Method Not Allowed"));
    REQUIRE(h.handler_calls == 0);  // the state route is unreachable by GET, full stop

    // The session route is likewise not a GET route.
    REQUIRE(Harness::has(h.handle(h.request("GET", "/api/session")), "405 Method Not Allowed"));
    REQUIRE(h.handler_calls == 0);

    // And a GET may not carry a body.
    REQUIRE(Harness::has(h.handle(h.request("GET", "/api/status", "", "{}")), "400 Bad Request"));
    REQUIRE(h.handler_calls == 0);
}

TEST_CASE("native http security: unauthorized callers cannot obtain a session", "[native_http_security]") {
    Harness h;
    REQUIRE(Harness::has(h.handle(h.post_session("wrong-secret")), "401 Unauthorized"));
    REQUIRE(h.handle(h.post_session("wrong-secret")).find("\"csrf\"") == std::string::npos);
    REQUIRE(h.handle(h.post_session("")).find("\"csrf\"") == std::string::npos);
    REQUIRE(h.handler_calls == 0);
    // The correct secret still works (the failure path is not a permanent deny).
    REQUIRE(h.mint().size() == 64);
}

TEST_CASE("native http security: no launch secret means the state route is disabled",
          "[native_http_security]") {
    Harness h;
    h.sessions.operator_secret.clear();  // server started without an operator secret
    REQUIRE(Harness::has(h.handle(h.post_session("anything")), "503 Service Unavailable"));
    REQUIRE(Harness::has(h.handle(h.post_state_no_csrf()), "503 Service Unavailable"));
    REQUIRE(h.handler_calls == 0);  // fail closed: nothing runs
}

TEST_CASE("native http security: an unauthorized POST cannot execute the scenario",
          "[native_http_security]") {
    Harness h;
    // Valid origin, valid content type, valid body - but no CSRF token at all.
    REQUIRE(Harness::has(h.handle(h.post_state_no_csrf()), "403 Forbidden"));
    REQUIRE(h.handler_calls == 0);

    // A made-up token is equally refused.
    const auto forged = h.request("POST", "/api/run-scenario?scenario=unusual",
                                  "Origin: http://127.0.0.1:" + std::to_string(h.port) + "\r\n"
                                  "Content-Type: application/json\r\n"
                                  "X-HG-CSRF: deadbeef\r\n",
                                  "{}");
    REQUIRE(Harness::has(h.handle(forged), "403 Forbidden"));
    REQUIRE(h.handler_calls == 0);
}

TEST_CASE("native http security: CSRF tokens are single-use and expiring", "[native_http_security]") {
    Harness h;
    const std::string csrf = h.mint();
    REQUIRE(csrf.size() == 64);

    auto authorized = [&]() {
        return h.request("POST", "/api/run-scenario?scenario=unusual",
                         "Origin: http://127.0.0.1:" + std::to_string(h.port) + "\r\n"
                         "Content-Type: application/json\r\n"
                         "X-HG-CSRF: " + csrf + "\r\n",
                         "{}");
    };

    REQUIRE(Harness::has(h.handle(authorized()), "200 OK"));
    REQUIRE(h.handler_calls == 1);                    // the one authorized run
    REQUIRE(Harness::has(h.handle(authorized()), "403 Forbidden"));  // replay refused
    REQUIRE(h.handler_calls == 1);                    // ...and it executed nothing

    // Expiry: a token minted now must not work after its lifetime.
    const std::string csrf2 = h.mint();
    h.now_ms += hg_native_http::kSessionTtlMs + 1;
    const auto expired = h.request("POST", "/api/run-scenario?scenario=unusual",
                                   "Origin: http://127.0.0.1:" + std::to_string(h.port) + "\r\n"
                                   "Content-Type: application/json\r\n"
                                   "X-HG-CSRF: " + csrf2 + "\r\n",
                                   "{}");
    REQUIRE(Harness::has(h.handle(expired), "403 Forbidden"));
    REQUIRE(h.handler_calls == 1);
}

TEST_CASE("native http security: exact same-origin is required on POST", "[native_http_security]") {
    Harness h;
    const std::string csrf = h.mint();
    auto post_with_origin = [&](const std::string& origin) {
        return h.request("POST", "/api/run-scenario?scenario=unusual",
                         "Origin: " + origin + "\r\n"
                         "Content-Type: application/json\r\n"
                         "X-HG-CSRF: " + csrf + "\r\n",
                         "{}");
    };
    const std::string p = std::to_string(h.port);

    REQUIRE(Harness::has(h.handle(post_with_origin("http://evil.example")), "403 Forbidden"));
    REQUIRE(Harness::has(h.handle(post_with_origin("http://127.0.0.1:9999")), "403 Forbidden"));  // wrong port
    REQUIRE(Harness::has(h.handle(post_with_origin("https://127.0.0.1:" + p)), "403 Forbidden"));  // wrong scheme
    REQUIRE(Harness::has(h.handle(post_with_origin("http://localhost.evil.example:" + p)), "403 Forbidden"));
    REQUIRE(Harness::has(h.handle(post_with_origin("null")), "403 Forbidden"));

    // Origin is mandatory on POST: absent is refused too.
    const auto no_origin = h.request("POST", "/api/run-scenario?scenario=unusual",
                                     "Content-Type: application/json\r\n"
                                     "X-HG-CSRF: " + csrf + "\r\n",
                                     "{}");
    REQUIRE(Harness::has(h.handle(no_origin), "403 Forbidden"));
    REQUIRE(h.handler_calls == 0);

    // The listener's own origin is accepted.
    REQUIRE(Harness::has(h.handle(post_with_origin("http://127.0.0.1:" + p)), "200 OK"));
    REQUIRE(h.handler_calls == 1);
}

TEST_CASE("native http security: Host validation and framing are strict", "[native_http_security]") {
    Harness h;
    const std::string csrf = h.mint();
    const std::string p = std::to_string(h.port);
    const std::string ok_headers = "Origin: http://127.0.0.1:" + p + "\r\n"
                                   "Content-Type: application/json\r\n"
                                   "X-HG-CSRF: " + csrf + "\r\n";

    // Non-loopback Host, duplicate Host, missing Host.
    // A duplicate Host is refused as malformed framing (400) ...
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            "Host: evil.example\r\n" + ok_headers, "{}")),
                         "400 Bad Request"));
    // ... while a single NON-loopback Host is refused by the rebinding defence (403).
    REQUIRE(Harness::has(h.handle("POST /api/run-scenario?scenario=unusual HTTP/1.1\r\n"
                                  "Host: evil.example\r\n" + ok_headers + "\r\n{}"),
                         "403 Forbidden"));
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            "Host: 127.0.0.1:" + p + "\r\n" + ok_headers, "{}")),
                         "400 Bad Request"));
    REQUIRE(Harness::has(h.handle("POST /api/run-scenario?scenario=unusual HTTP/1.1\r\n" + ok_headers + "\r\n{}"),
                         "400 Bad Request"));
    REQUIRE(h.handler_calls == 0);

    // Content-Length required; over the cap; mismatched with the body; wrong content type;
    // body that is not an empty JSON object.
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            ok_headers, "{}", /*with_length=*/false)),
                         "411 Length Required"));
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            ok_headers, "{}", true, "999999")),
                         "413 Payload Too Large"));
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            ok_headers, "{}", true, "99")),
                         "400 Bad Request"));
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            "Origin: http://127.0.0.1:" + p + "\r\n"
                                            "Content-Type: text/plain\r\n"
                                            "X-HG-CSRF: " + csrf + "\r\n",
                                            "{}")),
                         "415 Unsupported Media Type"));
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            ok_headers, "scenario=unusual")),
                         "400 Bad Request"));
    // Chunked framing stays refused.
    REQUIRE(Harness::has(h.handle(h.request("POST", "/api/run-scenario?scenario=unusual",
                                            ok_headers + "Transfer-Encoding: chunked\r\n", "0\r\n\r\n")),
                         "400 Bad Request"));
    REQUIRE(h.handler_calls == 0);
}

TEST_CASE("native http security: issuance is rate-limited and bad secrets lock out",
          "[native_http_security]") {
    Harness h;
    size_t minted = 0;
    bool limited = false;
    for (int i = 0; i < 8; ++i) {
        const auto r = h.handle(h.post_session(h.secret));
        if (Harness::has(r, "429 Too Many Requests")) { limited = true; break; }
        if (Harness::has(r, "200 OK")) ++minted;
    }
    REQUIRE(minted <= hg_native_http::kMaxIssuesPerWindow);
    REQUIRE(limited);
    REQUIRE(h.handler_calls == 0);

    // Lockout: repeated bad secrets stop even a correct one, temporarily.
    Harness l;
    for (int i = 0; i < hg_native_http::kMaxAuthFailures; ++i) {
        REQUIRE(Harness::has(l.handle(l.post_session("bad")), "401 Unauthorized"));
    }
    REQUIRE(Harness::has(l.handle(l.post_session(l.secret)), "401 Unauthorized"));  // locked
    l.now_ms += hg_native_http::kLockoutMs + 1;
    REQUIRE(l.handle(l.post_session(l.secret)).find("\"csrf\"") != std::string::npos);  // recovers
    REQUIRE(l.handler_calls == 0);
}

TEST_CASE("native http security: the scenario allowlist and read-only routes are preserved",
          "[native_http_security]") {
    Harness h;
    // Read-only GET routes still work, and still reach the handler exactly once each.
    REQUIRE(Harness::has(h.handle(h.request("GET", "/api/health")), "200 OK"));
    REQUIRE(h.handler_calls == 0);  // health is answered by the transport itself
    REQUIRE(Harness::has(h.handle(h.request("GET", "/api/status")), "200 OK"));
    REQUIRE(h.handler_calls == 1);
    REQUIRE(Harness::has(h.handle(h.request("GET", "/")), "text/html"));
    REQUIRE(h.handler_calls == 1);

    // Query validation is unchanged: unknown key, out-of-range cycles, bad scenario token.
    const std::string csrf = h.mint();
    const std::string p = std::to_string(h.port);
    auto run = [&](const std::string& q) {
        // A fresh token per call: they are single-use by design, so reusing one would test the
        // replay rule instead of the query validator.
        return h.handle(h.request("POST", "/api/run-scenario" + q,
                                  "Origin: http://127.0.0.1:" + p + "\r\n"
                                  "Content-Type: application/json\r\n"
                                  "X-HG-CSRF: " + h.mint() + "\r\n",
                                  "{}"));
    };
    REQUIRE(Harness::has(run("?scenario=unusual&bogus=1"), "400 Bad Request"));
    REQUIRE(Harness::has(run("?cycles=101"), "400 Bad Request"));
    REQUIRE(Harness::has(run("?scenario=NOT!VALID"), "400 Bad Request"));
    REQUIRE(h.handler_calls == 1);  // every rejected variant executed nothing
}

TEST_CASE("native http security: an authorized run still executes exactly once",
          "[native_http_security]") {
    // Proves the zero-invocation assertions above are not vacuous: the same counting handler
    // is reached exactly once when the caller is authorized.
    Harness h;
    const std::string csrf = h.mint();
    const auto r = h.handle(h.request("POST", "/api/run-scenario?scenario=unusual&cycles=3",
                                      "Origin: http://127.0.0.1:" + std::to_string(h.port) + "\r\n"
                                      "Content-Type: application/json\r\n"
                                      "X-HG-CSRF: " + csrf + "\r\n",
                                      "{}"));
    REQUIRE(Harness::has(r, "200 OK"));
    REQUIRE(h.handler_calls == 1);
}

TEST_CASE("native http security: the OS CSPRNG is used and never silently degraded",
          "[native_http_security]") {
    std::string a, b;
    REQUIRE(SessionStore::secure_random_hex(a, 32));
    REQUIRE(SessionStore::secure_random_hex(b, 32));
    REQUIRE(a.size() == 64);
    REQUIRE(b.size() == 64);
    REQUIRE(a != b);  // 256 bits of real entropy, not a counter or a fixed string
}
